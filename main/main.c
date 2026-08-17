// Routine Rush Watch — application entry point.
//
// AUDIO: tonal cues (alarm, routine start/complete, step tap, XP, daily step
// target) plus the volume + quiet-hours policy. The Phase 8 ADPCM voice prompts
// were removed — pre-rendered speech cannot cover free-text step labels.
//
// The watch mints a persistent device_id, generates an ephemeral pairing
// nonce, renders both as a QR on the AMOLED, and waits. The parent app scans
// it and performs the Supabase register+claim under its own authenticated
// session — the watch itself never talks to Supabase, because wifi is off and
// BLE is the only uplink (spec §2.2, §6B).
//
// Phase 1's TIME_SYNC path is still here and is now bond-gated (WRITE_ENC).
//
// Not implemented: the sync engine and power tuning (Phase 10). Steps and the
// audio policy are local-only by design (§10.1 leaves step sync to v1.1).

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_pm.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"

#include "rr_ble.h"
#include "rr_identity.h"
#include "rr_store.h"
#include "rr_routine.h"
#include "rr_imu.h"
#include "rr_idle.h"
#include "rr_pm.h"
#include "rr_battery.h"
#include "rr_rtc.h"
#include "rr_ui.h"
#include "rr_reset_button.h"
#include "rr_sched.h"
#include "rr_audio.h"
#include "rr_steps.h"
#include "rr_powerlog.h"
#include "rr_sleepdiag.h"

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)
#error "Routine Rush Watch requires ESP-IDF v5.5+ (board BSP declares idf >=5.5.0)."
#endif

static const char *TAG = "rr_watch";

// The exact shape apps/parent/app/watch/scan.tsx parses. It accepts either
// this JSON object or a routinerushparent:// URL with the same two query
// params; JSON is shorter, so the QR stays a lower version and scans faster.
//
// Both fields are strings and both are REQUIRED — the scanner rejects the
// payload outright if either is missing.
#define PAIRING_PAYLOAD_FMT "{\"device_id\":\"%s\",\"nonce\":\"%s\"}"

// ── What "paired" means, in one place ────────────────────────────────────────
//
// The watch face is a claim that this watch belongs to a child, so it is
// gated on PAIRING STATE — never on "is there anything in the cache". Those
// came apart after a factory reset: the reset cleared the paired flag but left
// child.json behind, so a cache-based check would still have drawn a face,
// wearing the previous child's avatar, over the pairing QR.
//
// Both conditions are required. Paired-but-no-child is a real intermediate
// state (bonded, first ROUTINE_PUSH not yet arrived) and it gets the honest
// "waiting for routines" screen instead of a face with an empty avatar slot.
// ⚠️ CALLED TWICE A SECOND by rr_idle's tick, so it must be cheap.
//
// It was not: every call did stat + fopen + malloc + fread + cJSON parse on
// /lfs/cache/child.json. That is ~172,800 littlefs reads a day to answer a
// question that changes at most once per pairing, and it kept the SPI flash out
// of idle around the clock.
//
// Cached, and the cache is sound rather than merely convenient:
//   • Once TRUE it stays true for this boot. The only thing that makes it false
//     again is a factory reset, and rr_ble_factory_reset() ends in esp_restart()
//     — so a stale `true` cannot outlive the state it describes.
//   • While FALSE (unpaired, or paired-but-awaiting-the-first-push) it is
//     re-checked, because that is the transition rr_idle polls for to promote the
//     "Paired ✓" screen to the live face. Rate-limited to once a second: the
//     pairing moment does not need 2 Hz resolution.
static bool s_face_ok;
static int64_t s_face_checked_ms;

// ── AN UNANSWERED READY OFFER OUTRANKS THE FACE ─────────────────────────────
//
// A scheduled or remotely-started routine puts up the READY screen and then
// waits. The panel is allowed to sleep on rr_idle's normal timeout while it
// waits — but the OFFER must not be lost with it, or a child who looks at
// their watch a minute later sees a clock and never learns a routine was
// asked for.
//
// ⚠️ WHY THIS SIDE EFFECT LIVES INSIDE A PREDICATE, which is otherwise a bad
// idea and is not being defended as good design:
//
//   • rr_idle has NO wake callback. The face gate is the only thing it
//     consults on the way out of sleep (wake_up -> build_and_show_face ->
//     face_allowed), so it is the one hook that reliably runs at the moment
//     the screen comes back.
//   • rr_idle.c is under concurrent edit by someone else, so adding a proper
//     wake hook there would collide with work in flight.
//
// If a wake hook ever appears in rr_idle, move the re-show onto it and leave
// this function a pure predicate again.
//
// It is safe to poll: rr_routine_show_ready() rebuilds NOTHING when READY is
// already the screen on display, which matters because rr_idle calls this
// twice a second and rebuilding would re-read the routine emoji off littlefs
// at 2 Hz and swallow the tap the screen is waiting for.
//
// Returning false is the other half — without it the face would be painted
// straight over the offer we just re-showed.
static bool watchface_allowed(void)
{
    if (rr_routine_ready_pending()) {
        rr_routine_show_ready();
        return false;
    }

    if (s_face_ok) return true;

    const int64_t now = (int64_t) xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (s_face_checked_ms != 0 && now - s_face_checked_ms < 1000) return false;
    s_face_checked_ms = now;

    if (!rr_identity_is_paired()) return false;

    rr_child_t child;
    if (rr_store_get_child(&child) != ESP_OK) return false;

    ESP_LOGI(TAG, "watch face unlocked (paired + child cached) — gate now cached");
    s_face_ok = true;
    return true;
}

// The queue depth changed — a run was just completed, or a phone acked one.
//
// Two consumers, wired here because rr_store sits under both of them and must
// not call either directly (see rr_store_set_queue_changed_hook):
//   • the watch face, whose "N to upload" badge was otherwise stale for up to a
//     minute after a successful drain — which reads as a failed sync;
//   • a connected phone, which previously only ever heard about acks it had
//     itself caused, so a routine finishing mid-connection went unannounced.
// ⚠️ THIS RUNS INSIDE rr_queue_append() AND rr_queue_ack(), AND ONE OF THOSE IS
// ON THE NimBLE HOST TASK. Keep it to flag-setting and cheap calls.
//
// The first version called rr_ble_notify_queue_status() here, which reaches
// rr_queue_oldest_ts(). Both that and rr_queue_ack() then used 1 KB STACK
// buffers, so an ack nested two 1 KB frames inside nimble_host's ~4 KB stack and
// the watch panicked on every single RUN_ACK ("Stack protection fault, task
// nimble_host"). Those buffers are on the heap now, but the lesson stands: this
// hook is called from a BLE callback and must stay shallow.
//
// The notify itself is not lost — run_ack_access_cb() already sends one after
// rr_queue_ack() returns, which is the correct place for it: outside the frame,
// with the ATT response already on its way.
static void queue_changed(void)
{
    rr_idle_notify_queue_changed();

    // Advertise briskly for a short window so a phone in the next room finds the
    // watch quickly. Only on a NON-EMPTY queue, so the ack that empties it does
    // not buy 30 s of fast advertising nobody needs — and adv_refresh() is a
    // no-op while connected, which is exactly when acks arrive.
    if (rr_queue_count() > 0) rr_ble_note_queue_activity();
}

// rr_routine's finish hook is void(void); rr_sched_rearm carries a reason
// string so the log says WHY a fire time was recomputed. This is the seam.
static void rr_sched_rearm_on_idle(void)
{
    rr_sched_rearm("routine finished");
}

// Idle-sleep suspension. Two unrelated reasons, both "the screen is doing a
// job right now":
//   - a routine is running: a child reading a countdown must not be blanked;
//   - the watch is unpaired: the pairing QR has to stay lit to be scannable.
//     Blanking it after 8 s makes a reset watch look bricked, and the wrist
//     raise that would wake it is not something you do while holding a phone
//     up to scan.
//
// An unanswered READY offer is DELIBERATELY NOT on this list, and that is a
// change: the old alarm screen suspended sleep so it could not blank itself
// before anyone answered. READY does not need to hold the panel lit, because
// sleeping no longer loses it — watchface_allowed() above re-shows it on the
// next wake. Holding a lit AMOLED for up to half an hour of grace window to
// preserve state we now keep properly is the wrong trade on a battery.
static bool idle_suspended(void)
{
    return rr_routine_is_active() || !rr_identity_is_paired();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Routine Rush Watch — tonal audio + policy, steps, paged sync");
    ESP_LOGI(TAG, "ESP-IDF %s", esp_get_idf_version());

    // ── Power management: DFS ON, and light sleep ON as of Phase 10 ─────────
    //
    // Phase 8 turned DFS on and left light sleep off, for a reason that was
    // correct at the time and has since been dealt with: the BLE controller
    // would have had its clock stopped with nothing arranged to wake it. That
    // needed CONFIG_BT_LE_SLEEP_ENABLE, which was thought impossible on this
    // board for want of a 32.768 kHz crystal — GPIO0/GPIO1 are QSPI pins. It
    // was not impossible; CONFIG_BT_LE_LP_CLK_SRC_MAIN_XTAL is IDF's answer for
    // exactly that board. See rr_pm.h and sdkconfig.defaults.
    //
    // Everything about the decision now lives in rr_pm, because it is no longer
    // a single call: it is a lock held while the panel is lit, GPIO wake sources
    // registered for the IMU and the reset button, and — the part that was
    // missing before — a way to MEASURE whether any of it worked.
    //
    // Called this early on purpose. The locks must exist before the display,
    // BLE or any driver that takes one of its own, and the display lock is
    // acquired inside rr_pm_init() because the screen is lit from boot.
    //
    // ⚠️ LIGHT SLEEP IS OPT-IN AND CURRENTLY OFF BY DEFAULT — build with
    //     idf.py -DRR_LIGHT_SLEEP=1 build
    // to turn it on. This is NOT caution left over from Phase 8; it is a
    // measured, reproduced fault, and the flag exists so the remaining work can
    // be done without shipping a watch that misses alarms in the meantime.
    //
    // WHAT HAPPENS WITH IT ON, on hardware, at 5+ minutes of uptime:
    //   E task_wdt: Task watchdog got triggered ... IDLE (CPU 0)
    //   E task_wdt: Tasks currently running: CPU 0: esp_timer
    // repeating every 5 s, WITH THE 30 s HEARTBEAT ABSENT ENTIRELY — so the
    // main task is being starved too, not just the idle task.
    //
    // WHY. Automatic light sleep costs a fixed entry/exit overhead each time.
    // LVGL drives its tick from an esp_timer with a period of the same order
    // (single-digit milliseconds), and that timer runs whether or not anything
    // is on screen. So the scheduler thrashes: sleep, wake on the tick almost
    // immediately, sleep again — never completing an idle pass. `ls` stuck at 1
    // with `slp 0%` is the same story from the other side: it entered light
    // sleep and got essentially no sleep out of it.
    //
    // THE FIX, NOT YET DONE: stop LVGL's tick and refresh timers while the panel
    // is dark (they have nothing to drive) and restore them in wake_up(), so the
    // idle period between step samples is tens of milliseconds rather than two.
    // That is display surgery, and the Phase 4b "went unresponsive" incident is
    // exactly what happens when it is done carelessly — hence a flag rather than
    // a rushed change.
    //
    // EVERYTHING ELSE FROM PHASE 10 STAYS ON and is independently useful: the
    // I2S locks are released (rr_audio), advertising is at 852.5 ms, the
    // wrist-raise gate is live, and the PM locks and telemetry are in place and
    // correct — the lock dump with light sleep enabled showed every
    // sleep-blocking lock at Active 0, which is what makes the LVGL tick the
    // remaining obstacle rather than one of several.
#ifdef RR_LIGHT_SLEEP
    ESP_ERROR_CHECK_WITHOUT_ABORT(rr_pm_init(true));
    ESP_LOGW(TAG, "⚠ light sleep ENABLED by RR_LIGHT_SLEEP — expect IDLE task "
                  "watchdog warnings until the LVGL tick is gated on the panel");
#else
    ESP_ERROR_CHECK_WITHOUT_ABORT(rr_pm_init(false));
#endif

    // NVS first — app_main owns the NVS lifecycle for the whole firmware.
    //
    // Two consumers need it and neither may initialise it itself: rr_identity
    // stores the device_id, and NimBLE's bond store persists pairing keys.
    // Doing it here (the conventional ESP-IDF place) means module init order
    // can be reasoned about locally instead of each module racing to be the
    // one that initialised NVS.
    //
    // The erase-and-retry branch is the canonical pattern: NO_FREE_PAGES means
    // the partition is full, NEW_VERSION_FOUND means it was written by a newer
    // NVS format. Both are unrecoverable without a wipe — and wiping costs us
    // the device_id, so the watch would need re-pairing. That is the correct
    // trade: the alternative is a device that cannot boot at all.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS unusable (%s) — erasing; device_id will be regenerated "
                      "and the watch will need re-pairing", esp_err_to_name(nvs_err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    // Report how full NVS is, every boot.
    //
    // This partition is 24 KB — six pages — and it holds the things that make
    // the watch THIS child's watch: the device_id the server-side pairing is
    // keyed on, the BLE bond, the UTC offset, the scheduler's last-fired marker,
    // and the step count. If it ever fills, the recovery path above ERASES IT,
    // which regenerates the device_id and silently orphans the pairing. That is
    // a bad thing to discover from a parent saying the app stopped finding the
    // watch, so the headroom is visible in the log from now on.
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialised — %d/%d entries used (%d free), %d namespaces",
                 st.used_entries, st.total_entries, st.free_entries, st.namespace_count);
        if (st.free_entries * 4 < st.total_entries) {
            ESP_LOGW(TAG, "NVS is over 75%% full — if it fills, the erase-and-retry "
                          "recovery costs the device_id and the watch needs re-pairing");
        }
    } else {
        ESP_LOGI(TAG, "NVS initialised");
    }

    // Identity next: the QR cannot be built without it, and a failure here is
    // fatal to pairing (see rr_identity.c on why we refuse a volatile id).
    ESP_ERROR_CHECK(rr_identity_init());

    // Display + LVGL. Also brings up the shared I2C bus via the BSP.
    ESP_ERROR_CHECK(rr_ui_init());

    ESP_ERROR_CHECK(rr_rtc_init(bsp_i2c_get_handle()));

    rr_rtc_time_t now;
    char tbuf[24];
    if (rr_rtc_get(&now) == ESP_OK) {
        rr_rtc_format(&now, tbuf, sizeof(tbuf));
        ESP_LOGI(TAG, "RTC at boot: %s (osc_ok=%d)", tbuf, (int) now.osc_ok);
    }

    // Local factory reset (hold BOOT 10 s). Armed early and unconditionally:
    // it is the ONLY recovery for a watch unlinked while out of BLE range.
    ESP_ERROR_CHECK(rr_reset_button_init());

    // LittleFS: the routine cache lives here (spec §5). Mount before BLE, since
    // a ROUTINE_PUSH can arrive as soon as we advertise and its handler writes
    // straight to the cache.
    ESP_ERROR_CHECK(rr_store_init());

    // Fan queue-depth changes out to the face and the phone. Registered before
    // rr_ble_init() so a run appended by a very early ROUTINE_PUSH + start still
    // notifies; both targets no-op safely until their own init has run.
    rr_store_set_queue_changed_hook(queue_changed);

    // Radio up: the phone bonds over BLE as part of the same pairing moment,
    // so the peripheral must be advertising by the time the parent scans.
    ESP_ERROR_CHECK(rr_ble_init());

    if (rr_identity_is_paired()) {
        // Already in service — do NOT show the QR again. Re-pairing needs an
        // explicit reset (see rr_identity.h).
        ESP_LOGI(TAG, "already paired — skipping the pairing QR");

#ifdef RR_DEV_NONCE_WHEN_PAIRED
        // ⚠️ TEMPORARY TEST AFFORDANCE — NEVER DEFINE THIS FOR A REAL BUILD.
        //
        // A paired watch shows no QR, so there is no nonce, so the laptop BLE
        // harnesses (tools/*.py) cannot authorise and cannot push test
        // schedules. This mints one anyway and prints it, purely so Phase 7
        // can be driven from a workstation without unpairing the phone.
        //
        // Build with: idf.py -DRR_DEV_NONCE=1 build
        {
            char devnonce[RR_NONCE_LEN];
            rr_identity_new_nonce(devnonce, sizeof(devnonce));
            rr_identity_set_active_nonce(devnonce);
            ESP_LOGW(TAG, "╔══ DEV BUILD: nonce auth enabled on a PAIRED watch ══");
            ESP_LOGW(TAG, "║ nonce: %s", devnonce);
            ESP_LOGW(TAG, "╚══ this must NOT ship ═══════════════════════════════");
        }
#endif
        if (rr_store_has_routines() == ESP_OK) {
            rr_store_log_routines();     // prove the cache survived the reboot
            rr_ui_show_paired_status();
        } else {
            ESP_LOGW(TAG, "paired but no routine cache — awaiting a ROUTINE_PUSH");
            rr_ui_show_waiting_status();
        }
    } else {
        char nonce[RR_NONCE_LEN];
        rr_identity_new_nonce(nonce, sizeof(nonce));
        // The nonce gate compares against this value (rr_ble ROUTINE_PUSH).
        rr_identity_set_active_nonce(nonce);

        char payload[128];
        int n = snprintf(payload, sizeof(payload), PAIRING_PAYLOAD_FMT,
                         rr_identity_device_id(), nonce);
        if (n < 0 || n >= (int) sizeof(payload)) {
            ESP_LOGE(TAG, "pairing payload truncated (%d bytes) — refusing to show a bad QR", n);
            return;
        }

        ESP_LOGI(TAG, "──────── PAIRING ────────");
        ESP_LOGI(TAG, "device_id: %s", rr_identity_device_id());
        ESP_LOGI(TAG, "nonce:     %s", nonce);
        ESP_LOGI(TAG, "payload:   %s", payload);
        ESP_LOGI(TAG, "─────────────────────────");

        ESP_ERROR_CHECK(rr_ui_show_pairing_qr(payload));
    }

    // ── Phase 6: IMU + the idle watch face ──────────────────────────────────
    // rr_imu is the SHARED sensor owner: raise-to-wake uses it now, Phase 9's
    // pedometer extends the same handle rather than re-initialising.
    ESP_ERROR_CHECK_WITHOUT_ABORT(rr_imu_init(bsp_i2c_get_handle()));
    ESP_ERROR_CHECK_WITHOUT_ABORT(rr_battery_init(bsp_i2c_get_handle()));

    // Both gates MUST be set before rr_idle_init(): it renders on entry, and
    // an ungated render there is exactly what painted the face over the QR.
    rr_idle_set_face_gate(watchface_allowed);
    rr_idle_set_suspend_check(idle_suspended);

    // A start request has to light the screen — the whole point of the nudge is
    // that the watch is on a wrist, asleep, and the child has not touched it.
    // It lights up on READY, not on a running step 1. Wired here, not called
    // directly from rr_routine, because
    // rr_power depends on rr_ble which depends on rr_routine; the hook keeps
    // that a straight line instead of a build cycle. Same shape as the two
    // gates above. Phase 7's scheduler inherits it for free — it goes through
    // rr_routine_request_start() too.
    rr_routine_set_wake_hook(rr_idle_wake_manual);

    // ── Phase 9: step counting ──────────────────────────────────────────────
    // Before rr_idle_init(), because the first face this boot draws reads the
    // count — an uninitialised module there would paint the placeholder once and
    // not correct it until the next wake.
    //
    // rr_steps asks rr_imu to start the on-chip engine; it does not touch the
    // sensor. A failure is non-fatal and deliberately visible: the face keeps
    // showing "—" rather than a confident zero.
    ESP_ERROR_CHECK_WITHOUT_ABORT(rr_steps_init());

#ifdef RR_PED_SELFTEST
    // ⚠️ RE-PROVING THE ON-CHIP STEP ENGINE. Build with:
    //     idf.py -DRR_PED_SELFTEST=1 build
    //
    // Phase 9 established that the QMI8658's embedded pedometer is INERT on this
    // part (rr_imu.h carries the evidence), and everything since has rested on
    // that finding. It is worth being able to re-run the experiment in one
    // command rather than re-deriving it, because the whole step-counting power
    // story depends on it: an on-chip engine counts with the CPU asleep and is
    // nearly free, while the software detector this board actually uses costs a
    // 25 Hz wake forever.
    //
    // The test reports the accelerometer alongside the counter, so "the engine
    // is dead" and "nobody moved the watch" cannot be confused — which is the
    // mistake that cost three inconclusive rounds the first time.
    ESP_LOGW(TAG, "╔══ PEDOMETER SELFTEST BUILD — WALK WITH THE WATCH NOW ══");
    if (rr_imu_pedometer_enable() == ESP_OK) {
        rr_imu_pedometer_selftest(30);
        rr_imu_pedometer_disable();
    } else {
        ESP_LOGE(TAG, "on-chip pedometer would not even enable");
    }
    // Put the accelerometer back where the software detector needs it: the
    // selftest and the enable path both move range and ODR.
    rr_imu_step_sampling_hold(true);
    ESP_LOGW(TAG, "╚══ selftest done — software detector resumes ══════════");
#endif


    ESP_ERROR_CHECK_WITHOUT_ABORT(rr_idle_init());

    // ── Phase 7: audio + the scheduler ──────────────────────────────────────
    // Audio FIRST: the scheduler's only alerting channel is the speaker (no
    // vibration motor exists on this board, §2), so a scheduler started before
    // the codec could fire its first alarm silently.
    ESP_ERROR_CHECK_WITHOUT_ABORT(rr_audio_init());

#ifdef RR_AUDIO_BOOT_TEST
    // ⚠️ VERIFYING THE PHASE 10 I2S REWRITE. Build with:
    //     idf.py -DRR_AUDIO_BOOT_TEST=1 build
    //
    // rr_audio now brings up I2S itself instead of calling bsp_audio_init(),
    // because the BSP left both channels enabled and that held the PM locks that
    // made light sleep impossible (see rr_audio.c). The whole point of the
    // rewrite is that playback is UNCHANGED — but "the codec initialised without
    // returning an error" is not evidence of that, and the failure mode is a
    // watch whose alarm is silent. There is no vibration motor (§2), so a silent
    // alarm is a routine that simply never happens.
    //
    // So: make a noise at boot and let a human confirm it. This is deliberately
    // a build flag rather than default behaviour — a watch that chirps every
    // time it reboots would be its own bug.
    ESP_LOGW(TAG, "╔══ AUDIO BOOT TEST — YOU SHOULD HEAR A TONE NOW ══");
    rr_audio_play_tone(RR_TONE_ROUTINE_COMPLETE);
#endif

    // Same two hooks rr_routine already uses, and for the same reason: rr_power
    // depends on rr_ble, which depends on rr_sched, so the arrows only point
    // one way and main.c ties the ends together.
    rr_sched_set_wake_hook(rr_idle_wake_manual);
    rr_routine_set_finish_hook(rr_sched_rearm_on_idle);

#ifdef RR_SCHED_TEST_MIN
    // ── Scheduler-integrity test: does a routine still fire under light sleep? ──
    //
    // This is the last gate before RR_LIGHT_SLEEP ships (docs/POWER.md). The risk
    // it tests is the one that matters: a watch that saves power and misses a
    // routine is a failure, and rr_powerlog's lateness counter cannot see it
    // because rr_sched waits on a task notification with a computed timeout, not
    // on a fixed vTaskDelay.
    //
    // ⚠️ IT MOVES THE CLOCK, IT DOES NOT ADD A SCHEDULE. That is deliberate: a
    // synthetic fire injected past rr_store would test a code path that does not
    // exist in production. Winding the RTC to RR_SCHED_TEST_MIN minutes before an
    // ALREADY-CACHED schedule means the genuine chain runs — rr_store_next_schedule
    // finds it, rr_sched arms the LP timer for the real interval, the CPU light
    // sleeps through it, and the fire has to survive that sleep to arrive.
    //
    // The cached 'Morning' routine is 07:30 local on weekday 5 (Friday), so this
    // sets the clock to Friday 07:30 minus RR_SCHED_TEST_MIN. rr_rtc holds UTC and
    // the offset is applied on the way out (rr_rtc.h), so the target is built in
    // UTC by subtracting the stored offset.
    //
    // 2026-08-21 is a Friday; epoch of 2026-08-21 07:30:00 UTC = 1787297400.
    // With a +3 h offset the RTC lands on Fri 04:10 UTC, which renders as the
    // local Fri 07:10 the scheduler needs to see. Verified before writing it.
    {
        const int32_t off_s = rr_rtc_get_utc_offset();
        const uint32_t target_utc = 1787297400u - (uint32_t) off_s
                                  - (uint32_t) (RR_SCHED_TEST_MIN * 60);
        ESP_LOGW(TAG, "╔══ SCHEDULER-INTEGRITY TEST ═══════════════════════════");
        ESP_LOGW(TAG, "║ winding the RTC to %d min before the cached Friday 07:30",
                 RR_SCHED_TEST_MIN);
        ESP_LOGW(TAG, "║ UNPLUG NOW. Expect: screen wake + alarm tone in ~%d min.",
                 RR_SCHED_TEST_MIN);
        ESP_LOGW(TAG, "║ THE CLOCK IS NOW WRONG — the phone's next TIME_SYNC fixes it.");
        ESP_LOGW(TAG, "╚═══════════════════════════════════════════════════════");
        ESP_ERROR_CHECK_WITHOUT_ABORT(rr_rtc_set_epoch(target_utc));
    }
#endif

    ESP_ERROR_CHECK_WITHOUT_ABORT(rr_sched_init());

#ifdef RR_SLEEPDIAG
    // Bench-only. 20 s in, so the screen has blanked (AWAKE_MS = 30 s means it
    // has NOT yet — the census reports screen state so this is visible rather
    // than assumed) and boot-time one-shot timers have retired.
    ESP_ERROR_CHECK_WITHOUT_ABORT(rr_sleepdiag_start(40, 30));
#endif

#ifdef RR_POWERLOG
    // Opt-in: `idf.py -DRR_POWERLOG=1 build`. Off by default so a shipping image
    // carries no measurement task — see docs/POWER.md for the procedure.
    rr_powerlog_start(60);
#endif

    int beats = 0;
    while (1) {
        // 30 s, not 5. Each tick reads the RTC over I2C, LINE-SCANS runs.log for
        // the queue depth, and writes a console line — cheap individually, but it
        // is pure diagnostics running forever on a battery device. 30 s still
        // gives a usable trace when a monitor is attached.
        vTaskDelay(pdMS_TO_TICKS(30000));
        // Both clocks, every tick. The timezone bug survived as long as it did
        // because this line printed one time and the face showed another, with
        // nothing saying which was which or that an offset was missing.
        if (rr_rtc_get(&now) == ESP_OK) {
            rr_rtc_format(&now, tbuf, sizeof(tbuf));
            rr_rtc_time_t local;
            char lbuf[24] = "no set_tz";
            if (rr_rtc_has_utc_offset() && rr_rtc_get_local(&local) == ESP_OK) {
                rr_rtc_format(&local, lbuf, sizeof(lbuf));
            }
            // The live PM state rides the heartbeat, not just the boot banner.
            // Boot lines are unrecoverable over USB-Serial-JTAG (resetting the
            // chip tears down the CDC, so the first second is lost), and
            // docs/POWER.md's before/after procedure depends on knowing whether
            // DFS and light sleep were actually in force for a given capture.
            //
            // ⚠️ THIS USED TO PRINT esp_pm_get_configuration()'s light_sleep_
            // enable FLAG, as "ls=0". That is the value we ourselves passed in
            // — it could never have been anything else, so it looked like
            // evidence while carrying none, and it was duly read as "light
            // sleep is not engaging" when all it said was "we did not ask for
            // it". rr_pm_describe() reports MEASURED sleeps and the share of
            // wall-clock spent in them, so a lock left held by mistake shows up
            // as a number that stops moving instead of a flag that still says 1.
            char pm_buf[48];
            rr_pm_describe(pm_buf, sizeof(pm_buf));

            // Proof the step detector is being fed live data. See
            // rr_steps_describe_input() — "steps unchanged" alone cannot tell a
            // frozen sensor from a watch nobody moved, and that ambiguity hid a
            // completely broken step counter for two phases.
            char accel_buf[80];
            rr_steps_describe_input(accel_buf, sizeof(accel_buf));

            ESP_LOGI(TAG, "UTC %s | local %s | ble=%s | routine=%s | screen=%s | queued=%d "
                          "| steps %" PRIu32 "%s | %s | %s | heap %u",
                     tbuf, lbuf,
                     rr_ble_is_connected() ? "CONNECTED" : "advertising",
                     rr_routine_is_active() ? "RUNNING" : "idle",
                     rr_idle_is_awake() ? "awake" : "asleep",
                     rr_queue_count(),
                     rr_steps_today(), rr_steps_valid() ? "" : "(invalid)",
                     pm_buf, accel_buf,
                     (unsigned) esp_get_free_heap_size());

            // Once, on the second beat — by then the screen has slept and the
            // steady-state set of lock holders is what it will be all night. If
            // `slp` is stuck at 0%, this is the line that says who to blame.
            if (++beats == 2) rr_pm_dump_locks();
        }
    }
}
