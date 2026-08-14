// rr_idle — the display sleep/wake cycle and the idle watch face (§9B.2, §10).
//
// The state machine is small on purpose:
//
//   AWAKE  --(no interaction for AWAKE_MS)-->  ASLEEP
//   ASLEEP --(short press on BOOT | a scheduled routine falling due)-->  AWAKE
//
// ⚠️ THERE IS NO AUTOMATIC WAKE ANY MORE (Phase 10). Raise-to-wake was removed
// by product decision: it kept the IMU switching between a motion-detect mode
// and a sampling mode, and step counting — which needs one continuous sampling
// mode all day — was the casualty. Waking is now an explicit act (the child
// presses BOOT) or the watch's own decision (the scheduler says a routine is
// due). See the note at the top of rr_imu.c for what actually broke and why the
// first explanation for it was wrong.
//
// That second path is not a nicety, it is the product: a child must never have
// to press anything to discover a routine has started. rr_sched calls
// rr_idle_wake_manual() through a hook wired in main.c.
//
// While ASLEEP the panel is dark, touch is off, and the accelerometer keeps
// sampling at 25 Hz so the daily step count stays real.
//
// Phase 10 added the other half. Blanking the panel was only ever the dominant
// draw, not the only one: the CPU stayed at full tilt through every dark hour.
// go_to_sleep() now also releases the no-light-sleep lock (rr_pm.h) so the chip
// can actually sleep between wakes, and wake_up() takes it back before anything
// touches the panel. The ordering of those two calls is load-bearing — see the
// comments at each.

#include "rr_idle.h"

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "lvgl.h"

#include "rr_battery.h"
#include "rr_imu.h"
#include "rr_pm.h"
#include "rr_rtc.h"
#include "rr_steps.h"
#include "rr_store.h"
#include "rr_ui.h"

static const char *TAG = "rr_idle";

// §9B.2 says "render the watch face for ~5-8s -> fade back off", and this was
// 8 s to match.
//
// Raised to 30 s in Phase 10, because the cost of a short timeout changed when
// raise-to-wake was removed. When a glance re-lit the screen for free, blanking
// after 8 s was almost invisible; now the only way back is a deliberate button
// press, so an early blank means the child has to press again to finish reading
// what they were already looking at.
//
// ⚠️ THIS IS THE DOMINANT SCREEN-ON POWER TERM. The AMOLED is the largest draw
// on the device, so this number multiplies directly into battery life —
// 30 s costs ~3.75x the panel-on time per wake that 8 s did. It is deliberately
// a single constant for that reason: if runtime disappoints, this is the first
// thing to trade back, and docs/POWER.md's idle-awake measurement is what tells
// you how much it is worth.
#define AWAKE_MS 30000

// How long the "Paired ✓" confirmation holds before the face replaces it.
// Long enough to read, short enough that the pairing flow ends on the face
// rather than on a status screen the child has no use for.
#define PAIRED_HOLD_MS 2000

static bool s_awake = true;
static int  s_shown_minute = -1;
static bool s_enabled;
static int64_t s_last_activity_ms;
static int64_t s_paired_at_ms;      // 0 = no pending promote-to-face
static bool s_gate_open;            // last observed face-gate state
static bool s_gate_ever_open;       // the gate has been open at least once
static lv_timer_t *s_idle_timer;
static volatile bool s_queue_dirty;   // set by rr_idle_notify_queue_changed()

// Enable/disable every LVGL input device. There is exactly one on this board
// (the FT3168), but asking LVGL rather than assuming keeps this correct if a
// second is ever added.
static void set_touch_enabled(bool on)
{
    bsp_display_lock(0);
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev != NULL) {
        lv_indev_enable(indev, on);
        indev = lv_indev_get_next(indev);
    }
    bsp_display_unlock();
}

static int64_t now_ms(void)
{
    return (int64_t) xTaskGetTickCount() * portTICK_PERIOD_MS;
}

// ── The face gate ───────────────────────────────────────────────────────────
// The watch face is the "this watch belongs to a child" screen. Drawing it is
// a claim about pairing state, so it asks the owner of that state rather than
// inferring it from whatever happens to be on flash.
static bool (*s_face_gate)(void);

static bool face_allowed(void)
{
    // Closed by default — see rr_idle.h on why the unset case must not render.
    return s_face_gate != NULL && s_face_gate();
}

// Returns false when the gate refused, leaving the current screen untouched.
static bool build_and_show_face(void)
{
    if (!face_allowed()) return false;

    rr_watchface_t w;
    memset(&w, 0, sizeof(w));

    // LOCAL, not UTC. The RTC is canonical UTC (rr_rtc.h) and the offset is
    // applied here, on the way to the screen.
    //
    // This governs more than the clock digits: w.year/month/day feed the date
    // AND the ISO weekday, and w.hour/minute feed rr_store_next_routine() —
    // whose schedules are authored as LOCAL "HH:MM" in the app. Reading UTC
    // here did not just show the wrong time, it compared local schedule times
    // against a UTC clock, so "next routine" was off by the offset too, and on
    // the wrong DAY either side of midnight.
    rr_rtc_time_t t;
    if (rr_rtc_get_local(&t) == ESP_OK) {
        w.hour = t.hour; w.minute = t.minute;
        w.year = t.year; w.month = t.month; w.day = t.day;
    } else {
        w.year = 2000; w.month = 1; w.day = 1;
    }

    // Child record: avatar + language, now actually cached (the app pushes it
    // alongside the routines). Falls back gracefully before the first push.
    rr_child_t child;
    if (rr_store_get_child(&child) == ESP_OK) {
        strlcpy(w.language, child.language, sizeof(w.language));
        strlcpy(w.avatar_id, child.avatar_id, sizeof(w.avatar_id));
        w.level = child.level;
    } else {
        strlcpy(w.language, "en", sizeof(w.language));
    }

    // Battery is read HERE — on face build, i.e. on wake — not on a timer.
    // A percentage a few minutes stale is fine; waking the CPU to refresh it
    // is exactly the kind of thing that quietly costs a day of runtime.
    rr_battery_t b;
    if (rr_battery_read(&b) == ESP_OK) {
        w.batt_valid = true;
        w.batt_percent = b.percent;
        w.charging = b.charging;
    }

    // Refreshed HERE, on face build, so the number on screen is the engine's
    // current count and not up to a poll interval stale. One I2C read, the same
    // cost as the battery read just above it.
    rr_steps_refresh();
    w.steps_today = (int) rr_steps_today();
    w.steps_valid = rr_steps_valid();
    w.queued_runs = rr_queue_count();

    int wd = rr_ui_iso_weekday(w.year, w.month, w.day);
    rr_store_next_routine(wd, w.hour, w.minute, &w.next);

    rr_ui_show_watchface(&w);
    s_shown_minute = w.minute;
    return true;
}

static void go_to_sleep(void)
{
    if (!s_awake) return;
    s_awake = false;

    // Blank the panel. On AMOLED this is the dominant saving — unlit pixels
    // draw nothing, so a dark screen and no screen are nearly the same.
    bsp_display_backlight_off();

    // Stop polling the touch panel.
    //
    // LVGL's input device is read on a timer and lvgl_port_touchpad_read() does
    // an unconditional I2C transaction to the FT3168 every time — ~33 reads a
    // second, all night, to a controller nobody is touching.
    //
    // ⚠️ TRADE-OFF: this removes TOUCH-to-wake. The wake paths that remain are
    // the ones §9B.2 actually specifies — wrist-raise via the IMU, and the
    // button as a fallback — so this matches the spec rather than reducing it,
    // but it IS a behaviour change: tapping a dark screen no longer wakes it.
    // The proper fix is the FT3168's own INT line (GPIO15, already wired by the
    // BSP as .int_gpio_num), which would give touch-to-wake for zero idle cost.
    // That needs the lvgl_port indev driven from the interrupt instead of a
    // timer, which is a bigger change than this one.
    set_touch_enabled(false);

    // ⚠️ NOTHING IS ARMED HERE ANY MORE. This used to hand the watching over to
    // the IMU's wake-on-motion, and that is exactly what broke step counting:
    // arming WoM freezes the QMI8658's output registers, so rr_steps spent every
    // dark hour — most of the day — reading a constant and counting nothing.
    //
    // The accelerometer now keeps sampling straight through sleep, which is what
    // makes the daily step count real. The screen is woken deliberately instead:
    // a short press on BOOT, or the scheduler when a routine is due.

    // ── Let the CPU sleep (Phase 10) ────────────────────────────────────────
    //
    // LAST, and deliberately so. Everything above this line touches the panel
    // over QSPI or the touch controller over I2C, and light sleep stops the
    // clocks those transfers ride on — the Phase 4b "went unresponsive"
    // failure. Releasing the lock only once the display is dark and quiet means
    // there is no window where a sleep can land mid-transfer.
    //
    // This is the change the whole phase is about: the screen is off for the
    // vast majority of the day, and until now the CPU stayed at full tilt
    // through all of it.
    rr_pm_display_hold(false);

    ESP_LOGI(TAG, "asleep — display off, touch off, accel still sampling "
                  "(wake: BOOT press or a scheduled routine) — heap %u",
             (unsigned) esp_get_free_heap_size());
}

static void wake_up(const char *reason)
{
    s_last_activity_ms = now_ms();
    if (s_awake) return;
    s_awake = true;

    // FIRST — before any panel or touch traffic. Symmetric with go_to_sleep()
    // releasing it last: the lock brackets every transfer, so no QSPI or I2C
    // transaction can be issued in a state where the clocks might stop under it.
    rr_pm_display_hold(true);

    set_touch_enabled(true);

    // Unpaired: there is no face to wake to. Put back exactly what was there —
    // the pairing QR — instead of inventing a face out of leftover cache.
    if (!build_and_show_face()) rr_ui_show_last_status();
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "awake (%s) — heap %u", reason, (unsigned) esp_get_free_heap_size());
}

void rr_idle_notify_activity(void)
{
    s_last_activity_ms = now_ms();
}

static void idle_tick(lv_timer_t *t)
{
    (void) t;
    if (!s_enabled) return;

    // A routine on screen must never be blanked mid-step: a child looking at
    // "brush your teeth" with a running countdown has not stopped interacting
    // just because they have not tapped.
    if (rr_idle_is_suspended()) {
        s_last_activity_ms = now_ms();
        return;
    }

    // Any touch counts as interaction. LVGL tracks this for us, so we do not
    // need a second input path just to notice the screen was poked.
    //
    // Only meaningful while awake now: the indev is disabled on sleep (see
    // go_to_sleep), so a dark screen reports nothing and this loop would just be
    // walking a list to find stale state.
    lv_indev_t *indev = s_awake ? lv_indev_get_next(NULL) : NULL;
    while (indev != NULL) {
        if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            if (!s_awake) { wake_up("touch"); return; }
            s_last_activity_ms = now_ms();
        }
        indev = lv_indev_get_next(indev);
    }

    // ── the gate OPENING is the pairing event ───────────────────────────────
    // Polled here rather than pushed from rr_ble, because rr_power already
    // depends on rr_ble and a call back the other way would be a cycle. The
    // gate is derived state, so watching it needs no new plumbing: it goes
    // true the moment the first authenticated push has persisted both the
    // paired flag and the child record.
    //
    // Then hold the "Paired ✓" confirmation briefly before the face replaces
    // it, so the flow ends where the child will actually use the watch.
    // ⚠️ PAIRING HAPPENS ONCE, AND THE GATE NO LONGER DOES. This used to read a
    // false->true transition as "just paired", which held while the gate could
    // only ever open once per boot. It cannot any more: the READY offer closes
    // the gate deliberately (main.c watchface_allowed) so the face does not paint
    // over a routine waiting to be started, and every offer therefore ends with
    // the gate REOPENING.
    //
    // Without the s_gate_ever_open latch that reopen is indistinguishable from a
    // pairing, so the 2-second promote fires at the end of every routine — and
    // the thing it replaces is the "Τέλος!" completion screen, two seconds after
    // a child finishes. The transition is only a pairing the FIRST time.
    const bool gate_open = face_allowed();
    if (gate_open && !s_gate_open && !s_gate_ever_open) {
        s_gate_ever_open = true;
        s_paired_at_ms = now_ms();
        ESP_LOGI(TAG, "pairing complete — face in %d ms", PAIRED_HOLD_MS);
    }
    s_gate_open = gate_open;

    if (s_paired_at_ms != 0 && now_ms() - s_paired_at_ms >= PAIRED_HOLD_MS) {
        s_paired_at_ms = 0;
        // Only worth drawing if the panel is actually lit. A sync can land
        // while the watch is asleep on a wrist, and rendering behind a dark
        // screen would just burn the flash reads — wake_up() builds the face
        // itself, and by then the gate is open.
        if (s_awake && build_and_show_face()) s_last_activity_ms = now_ms();
    }

    // A queue change (a run completed, or a phone acked one) invalidates the
    // face's upload badge. Repaint here rather than at the call site — see
    // rr_idle_notify_queue_changed() on why that must not happen inline.
    if (s_queue_dirty) {
        s_queue_dirty = false;
        if (s_awake && rr_ui_last_screen_is_watchface()) {
            ESP_LOGI(TAG, "queue changed — refreshing the face badge");
            build_and_show_face();
        }
    }

    if (s_awake) {
        if (now_ms() - s_last_activity_ms >= AWAKE_MS) { go_to_sleep(); return; }

        // Re-render ONLY when the displayed minute actually changes. Rebuilding
        // on every tick would re-read routines.json from littlefs twice a
        // second to redraw a clock that had not moved — the face shows minutes,
        // so a minute is the natural refresh unit.
        if (rr_ui_last_screen_is_watchface()) {
            rr_rtc_time_t t;
            // Local, to match s_shown_minute. Whole-hour offsets would compare
            // the same either way, but the :30 and :45 zones (India, Nepal,
            // Chatham) would not — the face would refresh a minute early or
            // late forever.
            if (rr_rtc_get_local(&t) == ESP_OK && t.minute != s_shown_minute) {
                build_and_show_face();
            }
        }
    }
}

static bool (*s_suspend_fn)(void);

void rr_idle_set_suspend_check(bool (*fn)(void))
{
    s_suspend_fn = fn;
}

bool rr_idle_is_suspended(void)
{
    return s_suspend_fn != NULL && s_suspend_fn();
}

void rr_idle_set_face_gate(bool (*fn)(void))
{
    s_face_gate = fn;
}

// Lets rr_ui rebuild the face after a transient screen (the reset countdown)
// without rr_ui needing to know how a face is assembled.
static void repaint_face(void)
{
    build_and_show_face();
}

esp_err_t rr_idle_init(void)
{
    s_last_activity_ms = now_ms();
    s_awake = true;
    s_enabled = true;
    // Seed the edge detector, so a paired boot does not read as a fresh
    // pairing and re-promote a face that is already up. The ever-open latch is
    // seeded from the same reading: a watch that boots already paired has done
    // its pairing, and must never run the promote again.
    s_gate_open = face_allowed();
    s_gate_ever_open = s_gate_open;
    rr_ui_set_watchface_repaint(repaint_face);

    // NOT unconditional. app_main has already decided what this boot shows —
    // the face for a paired watch, the pairing QR for a reset one — and the
    // gate is what stops this call from overwriting the latter.
    build_and_show_face();
    bsp_display_backlight_on();

    bsp_display_lock(0);
    s_idle_timer = lv_timer_create(idle_tick, 500, NULL);
    bsp_display_unlock();

    ESP_LOGI(TAG, "idle face up; sleeping after %d ms of no interaction", AWAKE_MS);
    return ESP_OK;
}

bool rr_idle_is_awake(void) { return s_awake; }

void rr_idle_wake_manual(void) { wake_up("manual"); }

void rr_idle_notify_queue_changed(void) { s_queue_dirty = true; }
