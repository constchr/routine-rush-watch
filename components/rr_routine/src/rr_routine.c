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

// ── The READY offer ─────────────────────────────────────────────────────────
//
// The deferred callback used to call rr_routine_start() and drop the child
// into a running step 1. It raises the READY screen instead, and START is the
// only thing that starts anything. Both start paths — the scheduler and the
// phone's RR_CONTROL start_routine — already funnel through here, so this is
// one change in one place rather than a second mechanism bolted to one of them.
//
// RAM ONLY. Nothing about an offer is persisted: see rr_routine.h.
static struct {
    bool pending;
    int  routine_idx;
    char assignment_id[40];
    char routine_name[64];
    char routine_emoji[16];
    int  step_count;
    char language[4];
    /** THIS offer is the screen on display. Cleared with the rest on memset. */
    bool painted;
} s_ready;

static void (*s_answer_hook)(const char *assignment_id, bool started);

void rr_routine_set_answer_hook(void (*fn)(const char *assignment_id, bool started))
{
    s_answer_hook = fn;
}

bool rr_routine_ready_pending(void) { return s_ready.pending; }

// Both answers report to whoever raised the offer BEFORE acting on it, so the
// scheduler's bookkeeping is closed even if the start below fails.
static void answer_ready(bool started)
{
    char id[sizeof(s_ready.assignment_id)];
    char name[sizeof(s_ready.routine_name)];
    strlcpy(id, s_ready.assignment_id, sizeof(id));
    strlcpy(name, s_ready.routine_name, sizeof(name));
    const int idx = s_ready.routine_idx;

    // Cleared FIRST: the answer hook re-arms the scheduler, and a scheduler
    // pass that still saw pending==true would think the offer was unanswered.
    memset(&s_ready, 0, sizeof(s_ready));

    if (s_answer_hook != NULL) s_answer_hook(id, started);

    if (started) {
        esp_err_t err = rr_routine_start(idx);
        if (err != ESP_OK) {
            // Only reachable if the cache was replaced between the offer and
            // the tap. The child tapped START and nothing happened, so put the
            // face back rather than leaving a dead READY screen up.
            ESP_LOGE(TAG, "START tapped but routine %d would not start: %s",
                     idx, esp_err_to_name(err));
            rr_ui_show_last_status();
        }
        return;
    }

    // NOT NOW is a DISMISSAL, not a snooze — see rr_sched. Stop the alarm
    // (it repeats for ~80 s and the child has just said no) and put the watch
    // face back, or the dismissed offer stays on the glass.
    ESP_LOGI(TAG, "READY '%s' dismissed — not now", name);
    rr_audio_stop();
    rr_ui_dismiss_ready();
}

static void on_ready_start(void)   { answer_ready(true);  }
static void on_ready_not_now(void) { answer_ready(false); }

static void paint_ready(void)
{
    rr_ui_show_ready(s_ready.routine_name, s_ready.routine_emoji, s_ready.step_count,
                     s_ready.language, on_ready_start, on_ready_not_now);
    s_ready.painted = true;
}

/**
 * Paint the pending offer unless THIS offer is already what is on screen.
 *
 * ⚠️ THE GUARD IS PER-OFFER, NOT PER-SCREEN-KIND, and the difference is both
 * bugs at once. Its caller is main.c's face gate, polled twice a second, so a
 * missing guard re-reads the emoji off littlefs at 2 Hz and destroys the touch
 * this screen is waiting for. But guarding on "is a READY screen up" alone is
 * wrong in the other direction: a NEW offer arriving while a PREVIOUS one is
 * still displayed would be silently skipped, leaving the wrong routine on the
 * glass with the new one's callbacks nowhere.
 *
 * `painted` is cleared by the memset that registers each offer, so it means
 * "this offer, the one currently in s_ready, has been drawn" — which is the
 * question both cases actually need answered.
 *
 * On hardware the two callers raced: the wake hook opened the face gate, which
 * painted the offer, and deferred_offer then painted it again ~8 ms later
 * (two "READY screen:" lines per remote start, two emoji reads). Whoever gets
 * there first now wins and the other is a no-op.
 */
void rr_routine_show_ready(void)
{
    if (!s_ready.pending) return;
    if (s_ready.painted && rr_ui_last_screen_is_ready()) return;

    ESP_LOGI(TAG, "showing the READY offer for '%s'", s_ready.routine_name);
    paint_ready();
}

void rr_routine_cancel_ready(const char *assignment_id)
{
    if (!s_ready.pending) return;
    if (assignment_id == NULL || strcmp(assignment_id, s_ready.assignment_id) != 0) return;

    ESP_LOGI(TAG, "READY offer for '%s' withdrawn", s_ready.routine_name);
    memset(&s_ready, 0, sizeof(s_ready));
    rr_audio_stop();
    rr_ui_dismiss_ready();
}

// The index is resolved BEFORE deferring and carried through as a plain int,
// not a pointer: lv_async_call takes ownership of nothing, so anything heap-
// allocated here would need freeing in the callback, and anything stack-
// allocated would be gone. An int fits in the void* itself.
static void deferred_offer(void *arg)
{
    const int routine_idx = (int) (intptr_t) arg;

    // Re-check under the LVGL task. Between the accept and this callback the
    // child could have started something by hand, or a second start could have
    // been accepted. The synchronous check is what the phone was TOLD; this is
    // what actually protects the running routine.
    if (s.active) {
        ESP_LOGW(TAG, "deferred offer dropped — a routine started in the meantime");
        return;
    }

    rr_step_view_t v;
    if (rr_store_get_step(routine_idx, 0, &v) != ESP_OK) {
        // Only reachable if the cache changed between the lookup and here.
        ESP_LOGE(TAG, "deferred offer of routine %d dropped — no cached steps", routine_idx);
        return;
    }

    rr_child_t child;
    const char *lang = (rr_store_get_child(&child) == ESP_OK) ? child.language : "en";

    memset(&s_ready, 0, sizeof(s_ready));
    s_ready.pending = true;
    s_ready.routine_idx = routine_idx;
    s_ready.step_count = v.step_count;
    strlcpy(s_ready.assignment_id, v.assignment_id, sizeof(s_ready.assignment_id));
    strlcpy(s_ready.routine_name, v.routine_name, sizeof(s_ready.routine_name));
    strlcpy(s_ready.routine_emoji, v.routine_emoji, sizeof(s_ready.routine_emoji));
    strlcpy(s_ready.language, lang, sizeof(s_ready.language));

    // Wake FIRST, then paint. wake_up() rebuilds the watch face on its way
    // back up, so painting first would show READY and then have the face drawn
    // straight over it.
    if (s_wake_hook != NULL) s_wake_hook();

    ESP_LOGI(TAG, "════ READY: '%s' — %d step(s), waiting for START ════",
             s_ready.routine_name, s_ready.step_count);
    // Through the same guarded path the face gate uses. Waking from sleep has
    // usually painted this offer already (pending is set above, so the gate is
    // shut by the time wake_up() renders) — the guard is what keeps that from
    // costing a second render and a second emoji read.
    rr_routine_show_ready();
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
    // An offer already up is REPLACED, not refused. The busy rule protects a
    // routine in progress; READY is nobody mid-task, and a parent who just
    // tapped "start bedtime" means that one rather than the breakfast offer
    // still sitting unanswered on the glass. The scheduler notices its own
    // occurrence went unanswered when the grace window runs out.
    if (s_ready.pending) {
        ESP_LOGW(TAG, "start_routine %s REPLACES the unanswered offer for '%s'",
                 assignment_id, s_ready.routine_name);
    }

    bsp_display_lock(0);
    lv_result_t rc = lv_async_call(deferred_offer, (void *) (intptr_t) idx);
    bsp_display_unlock();

    if (rc != LV_RESULT_OK) {
        ESP_LOGE(TAG, "start_routine %s: could not queue the offer", assignment_id);
        return RR_START_ERROR;
    }

    // ACCEPTED means "READY will be on screen", NOT "running" — see
    // rr_start_result_t. The routine begins when the child taps START.
    ESP_LOGI(TAG, "start_routine %s ACCEPTED → READY for routine index %d", assignment_id, idx);
    return RR_START_OK;
}
