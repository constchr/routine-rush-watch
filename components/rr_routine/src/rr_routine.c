// rr_routine — routine runtime (§8), Phase 4b.

#include "rr_routine.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "cJSON.h"

#include "rr_audio.h"
#include "rr_store.h"
#include "rr_identity.h"
#include "rr_rtc.h"
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
    char assignment_id[40];
    uint32_t started_epoch;
    bool clock_uncertain;
    rr_step_outcome_t outcome[RR_MAX_STEPS];
    char step_id[RR_MAX_STEPS][40];
    int  step_base_xp[RR_MAX_STEPS];
    int  step_limit_s[RR_MAX_STEPS];
    int  step_remaining_s[RR_MAX_STEPS];   /**< at the moment of Done/Skip */
    lv_timer_t *tick;
} s;

// The kids app's formula, mirrored exactly (packages/shared runs.ts
// calculateStepXP). Reimplemented rather than shared because the watch is C —
// so it is a LIABILITY: if the app's formula changes, this must change with
// it, or the child sees one number on the tablet and another on the wrist.
//
// streakMultiplier is 1.0 here on purpose: the watch does not know the child's
// streak, and guessing would make the provisional number worse than a plainly
// conservative one. The server's trigger computes the authoritative XP and the
// phone relays it back via RUN_ACK (§6.4).
static int calc_step_xp(int base_xp, int seconds_remaining, int time_limit_s)
{
    int speed_bonus = 0;
    if (time_limit_s > 0) {
        // floor(base * 0.5 * remaining/limit) — integer maths, same result.
        speed_bonus = (base_xp * seconds_remaining) / (2 * time_limit_s);
    }
    return base_xp + speed_bonus;
}

static void show_current_step(void);
static void on_done(void);
static void on_skip(void);

// Defined with the other hooks at the bottom; declared here because
// finish_routine() calls it. See rr_routine.h.
static void (*s_finish_hook)(void);

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


// Build the run record and queue it durably. Shape matches WatchRunRecord in
// packages/shared/src/api/watchRelay.ts — the relay is NOT reimplemented here;
// the phone feeds this straight into relayWatchRun().
//
// child_id is DELIBERATELY ABSENT. The watch is never told which child it is
// paired to (the server-side claim knows; the watch only ever learns routines),
// so the phone injects it at relay time from the pairing it already owns. That
// also means a watch re-paired to a sibling cannot carry a stale child_id.
static void queue_run_record(void)
{
    char local_id[RR_DEVICE_ID_LEN];
    rr_identity_new_uuid(local_id, sizeof(local_id));

    uint32_t now_epoch = rr_rtc_get_epoch();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) { ESP_LOGE(TAG, "OOM building run record"); return; }

    cJSON_AddStringToObject(root, "local_id", local_id);
    cJSON_AddStringToObject(root, "assignment_id", s.assignment_id);
    cJSON_AddStringToObject(root, "status", "completed");
    // ISO strings, because QUEUE_PULL carries a WatchRunRecord verbatim
    // (§6B.3) and that type declares started_at/completed_at as ISO.
    char started_iso[24], completed_iso[24];
    rr_rtc_epoch_to_iso(s.started_epoch, started_iso, sizeof(started_iso));
    rr_rtc_epoch_to_iso(now_epoch, completed_iso, sizeof(completed_iso));
    cJSON_AddStringToObject(root, "started_at", started_iso);
    cJSON_AddStringToObject(root, "completed_at", completed_iso);

    // Watch-internal, ignored by the phone's decoder: QUEUE_STATUS reports
    // oldest_ts as a u32 epoch, and re-parsing the ISO string on every status
    // read would be wasted work. Extra JSON fields are inert to JSON.parse.
    cJSON_AddNumberToObject(root, "completed_epoch", now_epoch);
    if (s.clock_uncertain) {
        // §6.5: the RTC had never been set this boot, so the timestamps are
        // meaningless. Say so rather than shipping plausible-looking garbage —
        // the server clamps to receipt time.
        cJSON_AddBoolToObject(root, "clock_uncertain", true);
    }

    int provisional_xp = 0;
    cJSON *steps = cJSON_AddArrayToObject(root, "steps");
    for (int i = 0; i < s.step_count && i < RR_MAX_STEPS; i++) {
        cJSON *st = cJSON_CreateObject();
        cJSON_AddStringToObject(st, "step_id", s.step_id[i]);

        // The watch's DONE/SKIPPED maps onto step_outcome's vocabulary. A step
        // never reached would be "missed", but this session always walks every
        // step, so only these two occur.
        const char *state = s.outcome[i] == RR_STEP_DONE ? "completed"
                          : s.outcome[i] == RR_STEP_SKIPPED ? "skipped" : "missed";
        cJSON_AddStringToObject(st, "state", state);

        int remaining = s.step_remaining_s[i];
        int limit = s.step_limit_s[i];
        int taken = limit > 0 ? (limit - remaining) : 0;
        cJSON_AddNumberToObject(st, "time_taken_s", taken);
        cJSON_AddNumberToObject(st, "seconds_remaining", remaining);
        cJSON_AddItemToArray(steps, st);

        if (s.outcome[i] == RR_STEP_DONE) {
            provisional_xp += calc_step_xp(s.step_base_xp[i], remaining, limit);
        }
    }
    cJSON_AddNumberToObject(root, "provisional_xp", provisional_xp);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) { ESP_LOGE(TAG, "OOM serialising run record"); return; }

    ESP_LOGI(TAG, "run record: local_id=%s xp=%d clock_uncertain=%d",
             local_id, provisional_xp, (int) s.clock_uncertain);
    if (rr_queue_append(json, strlen(json)) != ESP_OK) {
        ESP_LOGE(TAG, "FAILED to queue the run — it is lost");
    }
    free(json);
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

    queue_run_record();

    // Fanfare then chime, in that order — the effect queue is FIFO precisely so
    // these are two sounds and not one clobbering the other.
    rr_audio_play_tone(RR_TONE_ROUTINE_COMPLETE);
    rr_audio_play_tone(RR_TONE_XP_CHIME);

    rr_ui_show_routine_complete(s.routine_name, done, skipped);
    log_heap("at routine complete");

    // The watch is idle again. Anything that was waiting for that — today, a
    // scheduled routine the busy rule deferred — gets told immediately rather
    // than discovering it on its next poll.
    if (s_finish_hook != NULL) s_finish_hook();
}

static void advance(rr_step_outcome_t outcome)
{
    if (!s.active) return;
    if (s.step_idx < RR_MAX_STEPS) {
        s.outcome[s.step_idx] = outcome;
        s.step_remaining_s[s.step_idx] = s.remaining_s;
    }

    ESP_LOGI(TAG, "step %d/%d -> %s", s.step_idx + 1, s.step_count,
             outcome == RR_STEP_DONE ? "DONE" : "SKIPPED");

    // A blip for Done only. A skip is not an achievement and should not sound
    // like one — the kids app makes the same distinction.
    if (outcome == RR_STEP_DONE) rr_audio_play_tone(RR_TONE_STEP_DONE);

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
    if (s.step_idx < RR_MAX_STEPS) {
        strlcpy(s.step_id[s.step_idx], v.step_id, sizeof(s.step_id[0]));
        s.step_base_xp[s.step_idx] = v.base_xp;
        s.step_limit_s[s.step_idx] = v.time_limit_s;
    }
    if (s.assignment_id[0] == '\0') {
        strlcpy(s.assignment_id, v.assignment_id, sizeof(s.assignment_id));
    }

    rr_ui_show_step(&v, on_done, on_skip);
    log_heap(s.step_idx == 0 ? "after first step screen" : "after step advance");

    // NO SOUND ON A STEP SCREEN, deliberately. This used to speak the step in the
    // child's language (§10B.4), which read well for pre-readers but only worked
    // for the 16 starter templates — `template_step.label` is free text, so a
    // parent's own wording matched nothing and the step was silent anyway. The
    // voice set is gone; the screen (emoji + label + ring) is the prompt, and the
    // sounds that remain mark EVENTS: routine start, each tap, completion, XP,
    // the step target, and the alarm.
    //
    // Substituting a tone here was considered and rejected: a sound on every step
    // screen as well as every step tap teaches a child that a sound means nothing
    // in particular, which costs the alarm its meaning too.

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

    s.started_epoch = rr_rtc_get_epoch();
    rr_rtc_time_t rt;
    s.clock_uncertain = (rr_rtc_get(&rt) != ESP_OK) || !rt.osc_ok;

    ESP_LOGI(TAG, "════ START '%s' — %d step(s) ════", s.routine_name, s.step_count);
    rr_audio_play_tone(RR_TONE_ROUTINE_START);
    log_heap("at routine start");

    show_current_step();
    return ESP_OK;
}

bool rr_routine_is_active(void)
{
    return s.active;
}

// ═════════════════════════════════════════════════════════════════════════════
// Remote / scheduled start — the deferred entry point (see rr_routine.h)
// ═════════════════════════════════════════════════════════════════════════════

static void (*s_wake_hook)(void);

void rr_routine_set_wake_hook(void (*fn)(void))
{
    s_wake_hook = fn;
}

void rr_routine_set_finish_hook(void (*fn)(void))
{
    s_finish_hook = fn;
}

// The index is resolved BEFORE deferring and carried through as a plain int,
// not a pointer: lv_async_call takes ownership of nothing, so anything heap-
// allocated here would need freeing in the callback, and anything stack-
// allocated would be gone. An int fits in the void* itself.
static void deferred_start(void *arg)
{
    const int routine_idx = (int) (intptr_t) arg;

    // Re-check under the LVGL task. Between the accept and this callback the
    // child could have started something by hand, or a second start could have
    // been accepted. The synchronous check is what the phone was TOLD; this is
    // what actually protects the running routine.
    if (s.active) {
        ESP_LOGW(TAG, "deferred start dropped — a routine started in the meantime");
        return;
    }

    // Wake FIRST, then paint. wake_up() rebuilds the watch face on its way
    // back up, so starting first would show the step screen and then have the
    // face drawn straight over it.
    if (s_wake_hook != NULL) s_wake_hook();

    esp_err_t err = rr_routine_start(routine_idx);
    if (err != ESP_OK) {
        // Only reachable if the cache changed between the lookup and here.
        ESP_LOGE(TAG, "deferred start of routine %d failed: %s",
                 routine_idx, esp_err_to_name(err));
    }
}

rr_start_result_t rr_routine_request_start(const char *assignment_id)
{
    if (assignment_id == NULL || assignment_id[0] == '\0') return RR_START_ERROR;

    // Busy check first, and deliberately before the cache read: refusing costs
    // nothing, and there is no reason to spend a flash read and a JSON parse
    // on a request whose answer is already known.
    if (s.active) {
        ESP_LOGW(TAG, "start_routine %s REFUSED — '%s' is running (step %d/%d)",
                 assignment_id, s.routine_name, s.step_idx + 1, s.step_count);
        return RR_START_BUSY;
    }

    int idx = 0;
    esp_err_t err = rr_store_find_routine(assignment_id, &idx);
    if (err == ESP_ERR_NOT_FOUND) return RR_START_UNKNOWN_ROUTINE;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start_routine %s: cache unreadable: %s",
                 assignment_id, esp_err_to_name(err));
        return RR_START_ERROR;
    }

    // Hand off to the LVGL task. lv_async_call touches an LVGL list, so it
    // takes the display lock like any other LVGL call from a foreign task —
    // the callback itself then runs inside lv_timer_handler, where building
    // screens and creating timers is legal.
    bsp_display_lock(0);
    lv_result_t rc = lv_async_call(deferred_start, (void *) (intptr_t) idx);
    bsp_display_unlock();

    if (rc != LV_RESULT_OK) {
        ESP_LOGE(TAG, "start_routine %s: could not queue the start", assignment_id);
        return RR_START_ERROR;
    }

    ESP_LOGI(TAG, "start_routine %s ACCEPTED → routine index %d", assignment_id, idx);
    return RR_START_OK;
}
