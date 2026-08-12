// rr_idle — the display sleep/wake cycle and the idle watch face (§9B.2, §10).
//
// The state machine is small on purpose:
//
//   AWAKE  --(no interaction for AWAKE_MS)-->  ASLEEP
//   ASLEEP --(IMU wake-on-motion | touch | BOOT tap)-->  AWAKE
//
// While ASLEEP the panel is dark and the IMU is armed to watch for movement on
// its own hardware, so the CPU is not polling an accelerometer to find out
// whether a wrist moved (§10 — polling would defeat the entire battery case).
//
// What is NOT here: CPU light-sleep and BLE duty-cycling. This turns the
// display off, which is the dominant draw, but real power tuning is Phase 10.
// Saying so plainly matters because "sleep" that only blanks a screen is easy
// to mistake for a solved power story.

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
#include "rr_rtc.h"
#include "rr_steps.h"
#include "rr_store.h"
#include "rr_ui.h"

static const char *TAG = "rr_idle";

// §9B.2: "render the watch face for ~5-8s -> fade back off". 8 s, because a
// glance at a wrist is often interrupted and re-raising is annoying.
#define AWAKE_MS 8000

// QMI8658 wake-on-motion threshold, in MILLI-G. Lower = more sensitive.
//
// Tuned on hardware: 8 mg (0.008 g) was so sensitive that desk vibration
// through the USB cable re-woke the watch within a second of every sleep —
// a wake loop, not a wake feature. 96 mg ignores ambient noise while still
// catching a deliberate arm movement.
//
// This is the one number that still wants tuning on a REAL WRIST rather than a
// bench: too low drains the battery in a pocket, too high misses a raise and
// the child decides the watch is broken. Flagged for Phase 10.
#define WOM_THRESHOLD 96

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
static lv_timer_t *s_idle_timer;

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

    // Hand the watching over to the IMU's own hardware.
    rr_imu_arm_wake_on_motion(WOM_THRESHOLD, rr_idle_notify_wake);

    ESP_LOGI(TAG, "asleep — display off, IMU armed (heap %u)",
             (unsigned) esp_get_free_heap_size());
}

static void wake_up(const char *reason)
{
    s_last_activity_ms = now_ms();
    if (s_awake) return;
    s_awake = true;

    // Disarm first: while awake we do not want a motion interrupt every time
    // the child moves their arm, and Phase 9's pedometer wants the sensor in
    // normal sampling mode anyway.
    rr_imu_disarm_wake_on_motion();

    // Unpaired: there is no face to wake to. Put back exactly what was there —
    // the pairing QR — instead of inventing a face out of leftover cache.
    if (!build_and_show_face()) rr_ui_show_last_status();
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "awake (%s) — heap %u", reason, (unsigned) esp_get_free_heap_size());
}

// Called from the IMU interrupt task, not an ISR.
void rr_idle_notify_wake(void)
{
    wake_up("wrist raise");
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
    lv_indev_t *indev = lv_indev_get_next(NULL);
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
    const bool gate_open = face_allowed();
    if (gate_open && !s_gate_open) {
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
    // pairing and re-promote a face that is already up.
    s_gate_open = face_allowed();
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
