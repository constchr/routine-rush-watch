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

#include "rr_imu.h"
#include "rr_rtc.h"
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

static bool s_awake = true;
static int  s_shown_minute = -1;
static bool s_enabled;
static int64_t s_last_activity_ms;
static lv_timer_t *s_idle_timer;

static int64_t now_ms(void)
{
    return (int64_t) xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void build_and_show_face(void)
{
    rr_watchface_t w;
    memset(&w, 0, sizeof(w));

    rr_rtc_time_t t;
    if (rr_rtc_get(&t) == ESP_OK) {
        w.hour = t.hour; w.minute = t.minute;
        w.year = t.year; w.month = t.month; w.day = t.day;
    } else {
        w.year = 2000; w.month = 1; w.day = 1;
    }

    // Language would come from the cached child record; that is not cached yet
    // (Phase 5 pushes routines only), so default to Greek and note the gap
    // rather than inventing a lookup.
    strlcpy(w.language, "el", sizeof(w.language));

    w.steps_today = 0;
    w.steps_valid = false;      // Phase 9 flips this
    w.level = 0;
    w.queued_runs = rr_queue_count();

    int wd = rr_ui_iso_weekday(w.year, w.month, w.day);
    rr_store_next_routine(wd, w.hour, w.minute, &w.next);

    rr_ui_show_watchface(&w);
    s_shown_minute = w.minute;
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

    build_and_show_face();
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

    if (s_awake) {
        if (now_ms() - s_last_activity_ms >= AWAKE_MS) { go_to_sleep(); return; }

        // Re-render ONLY when the displayed minute actually changes. Rebuilding
        // on every tick would re-read routines.json from littlefs twice a
        // second to redraw a clock that had not moved — the face shows minutes,
        // so a minute is the natural refresh unit.
        if (rr_ui_last_screen_is_watchface()) {
            rr_rtc_time_t t;
            if (rr_rtc_get(&t) == ESP_OK && t.minute != s_shown_minute) {
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

esp_err_t rr_idle_init(void)
{
    s_last_activity_ms = now_ms();
    s_awake = true;
    s_enabled = true;

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
