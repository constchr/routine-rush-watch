// rr_routine — routine runtime (§8), Phase 4b.

#include "rr_routine.h"

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "lvgl.h"

#include "rr_store.h"
#include "rr_ui.h"

static const char *TAG = "rr_routine";

#define RR_MAX_STEPS 32

static struct {
    bool active;
    int  routine_idx;
    int  step_idx;
    int  step_count;
    int  remaining_s;
    int  total_s;
    char routine_name[64];
    rr_step_outcome_t outcome[RR_MAX_STEPS];
    lv_timer_t *tick;
} s;

static void show_current_step(void);
static void on_done(void);
static void on_skip(void);

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "heap %s: free=%u largest=%u", when,
             (unsigned) esp_get_free_heap_size(),
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

static void stop_tick(void)
{
    if (s.tick != NULL) {
        lv_timer_delete(s.tick);
        s.tick = NULL;
    }
}

// 1 Hz. Only ever touches the ring and the mm:ss label, so the emoji is never
// re-read from flash while a step runs.
static void tick_cb(lv_timer_t *t)
{
    (void) t;
    if (!s.active || s.total_s <= 0) return;

    if (s.remaining_s > 0) {
        s.remaining_s--;
        rr_ui_set_countdown(s.remaining_s, s.total_s);

        if (s.remaining_s == 0) {
            // §8: time's up does NOT auto-fail and does NOT advance. The step
            // stays active with Done/Skip available — the same choice the kids
            // app makes, so a child who is still brushing their teeth is not
            // told they failed. Stop ticking; there is nothing left to count.
            ESP_LOGI(TAG, "step %d/%d timer expired — step STAYS ACTIVE (no auto-advance)",
                     s.step_idx + 1, s.step_count);
            stop_tick();
        }
    }
}

static void finish_routine(void)
{
    stop_tick();
    s.active = false;

    int done = 0, skipped = 0;
    for (int i = 0; i < s.step_count && i < RR_MAX_STEPS; i++) {
        if (s.outcome[i] == RR_STEP_DONE) done++;
        else if (s.outcome[i] == RR_STEP_SKIPPED) skipped++;
    }

    ESP_LOGI(TAG, "════ ROUTINE COMPLETE — '%s': %d done, %d skipped ════",
             s.routine_name, done, skipped);
    for (int i = 0; i < s.step_count && i < RR_MAX_STEPS; i++) {
        ESP_LOGI(TAG, "   step %d: %s", i + 1,
                 s.outcome[i] == RR_STEP_DONE ? "DONE" :
                 s.outcome[i] == RR_STEP_SKIPPED ? "SKIPPED" : "pending");
    }

    rr_ui_show_routine_complete(s.routine_name, done, skipped);
    log_heap("at routine complete");

    // No completion record is written. Phase 5 owns the durable queue; writing
    // a partial record here would create data the sync engine cannot reconcile.
}

static void advance(rr_step_outcome_t outcome)
{
    if (!s.active) return;
    if (s.step_idx < RR_MAX_STEPS) s.outcome[s.step_idx] = outcome;

    ESP_LOGI(TAG, "step %d/%d -> %s", s.step_idx + 1, s.step_count,
             outcome == RR_STEP_DONE ? "DONE" : "SKIPPED");

    stop_tick();
    s.step_idx++;

    if (s.step_idx >= s.step_count) {
        finish_routine();
        return;
    }
    show_current_step();
}

static void on_done(void) { advance(RR_STEP_DONE); }
static void on_skip(void) { advance(RR_STEP_SKIPPED); }

static void show_current_step(void)
{
    rr_step_view_t v;
    if (rr_store_get_step(s.routine_idx, s.step_idx, &v) != ESP_OK) {
        ESP_LOGE(TAG, "step %d missing from the cache — ending the routine", s.step_idx);
        finish_routine();
        return;
    }

    s.total_s = v.time_limit_s;
    s.remaining_s = v.time_limit_s;

    rr_ui_show_step(&v, on_done, on_skip);
    log_heap(s.step_idx == 0 ? "after first step screen" : "after step advance");

    if (v.time_limit_s > 0) {
        s.tick = lv_timer_create(tick_cb, 1000, NULL);
        ESP_LOGI(TAG, "countdown started: %d s", v.time_limit_s);
    } else {
        // Untimed step (§8 screen 3): no ring, no timer, Done/Skip only.
        ESP_LOGI(TAG, "untimed step — no countdown");
    }
}

esp_err_t rr_routine_start(int routine_idx)
{
    rr_step_view_t probe;
    esp_err_t err = rr_store_get_step(routine_idx, 0, &probe);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot start routine %d: no cached steps", routine_idx);
        return err;
    }

    memset(&s, 0, sizeof(s));
    s.active = true;
    s.routine_idx = routine_idx;
    s.step_idx = 0;
    s.step_count = probe.step_count;
    strlcpy(s.routine_name, probe.routine_name, sizeof(s.routine_name));

    ESP_LOGI(TAG, "════ START '%s' — %d step(s) ════", s.routine_name, s.step_count);
    log_heap("at routine start");

    show_current_step();
    return ESP_OK;
}

bool rr_routine_is_active(void)
{
    return s.active;
}
