// Routine Rush Watch — application entry point.
//
// PHASE 9: step counting on the QMI8658's on-chip pedometer.
//
// The watch mints a persistent device_id, generates an ephemeral pairing
// nonce, renders both as a QR on the AMOLED, and waits. The parent app scans
// it and performs the Supabase register+claim under its own authenticated
// session — the watch itself never talks to Supabase, because wifi is off and
// BLE is the only uplink (spec §2.2, §6B).
//
// Phase 1's TIME_SYNC path is still here and is now bond-gated (WRITE_ENC).
//
// Not implemented: the sync engine, power tuning (Phase 10), and the full
// audio subsystem (Phase 8 — only the alarm tone path exists, see rr_audio.h).
// Steps are local-only by design (§10.1 leaves backend sync to v1.1).

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
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
#include "rr_battery.h"
#include "rr_rtc.h"
#include "rr_ui.h"
#include "rr_reset_button.h"
#include "rr_sched.h"
#include "rr_audio.h"
#include "rr_steps.h"

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
static bool watchface_allowed(void)
{
    if (!rr_identity_is_paired()) return false;

    rr_child_t child;
    return rr_store_get_child(&child) == ESP_OK;
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
//   - a scheduled alarm is ringing and nobody has answered it yet: an alarm
//     that blanks itself after 8 s is not an alarm. rr_sched auto-snoozes it
//     after a minute, which is what ends this suspension.
static bool idle_suspended(void)
{
    return rr_routine_is_active() || !rr_identity_is_paired() || rr_sched_alarm_is_showing();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Routine Rush Watch — Phase 9 (step counting)");
    ESP_LOGI(TAG, "ESP-IDF %s", esp_get_idf_version());

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

    // A remotely-started routine has to light the screen — the whole point of
    // the nudge is that the watch is on a wrist, asleep, and the child has not
    // touched it. Wired here, not called directly from rr_routine, because
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


    ESP_ERROR_CHECK_WITHOUT_ABORT(rr_idle_init());

    // ── Phase 7: audio + the scheduler ──────────────────────────────────────
    // Audio FIRST: the scheduler's only alerting channel is the speaker (no
    // vibration motor exists on this board, §2), so a scheduler started before
    // the codec could fire its first alarm silently.
    ESP_ERROR_CHECK_WITHOUT_ABORT(rr_audio_init());

    // Same two hooks rr_routine already uses, and for the same reason: rr_power
    // depends on rr_ble, which depends on rr_sched, so the arrows only point
    // one way and main.c ties the ends together.
    rr_sched_set_wake_hook(rr_idle_wake_manual);
    rr_routine_set_finish_hook(rr_sched_rearm_on_idle);

    ESP_ERROR_CHECK_WITHOUT_ABORT(rr_sched_init());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
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
            ESP_LOGI(TAG, "UTC %s | local %s | ble=%s | routine=%s | screen=%s | queued=%d "
                          "| steps %" PRIu32 "%s | heap %u",
                     tbuf, lbuf,
                     rr_ble_is_connected() ? "CONNECTED" : "advertising",
                     rr_routine_is_active() ? "RUNNING" : "idle",
                     rr_idle_is_awake() ? "awake" : "asleep",
                     rr_queue_count(),
                     rr_steps_today(), rr_steps_valid() ? "" : "(invalid)",
                     (unsigned) esp_get_free_heap_size());
        }
    }
}
