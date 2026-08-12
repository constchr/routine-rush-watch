// rr_sched — the on-watch scheduler (§7). See rr_sched.h for the wake design
// and why the RTC alarm interrupt is not the wake source on this board.
//
// ── The loop, in one place ──────────────────────────────────────────────────
//
//   1. read the RTC, convert to LOCAL (schedules are authored local "HH:MM")
//   2. if an alarm is already pending, service it (ring / defer / snooze / drop)
//   3. otherwise find the next occurrence after the last one handled
//   4. sleep until it is due — one wake, no polling — or until re-armed
//
// Exactly one alarm is in flight at a time. A second routine scheduled at or
// near the same minute is NOT consumed while the first is pending; it is
// picked up as soon as that one resolves, which is §7's "if multiple routines
// collide, queue them".

#include "rr_sched.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "rr_audio.h"
#include "rr_routine.h"
#include "rr_rtc.h"
#include "rr_store.h"
#include "rr_ui.h"

static const char *TAG = "rr_sched";

#define TASK_STACK 5120
#define TASK_PRIO  3

// The last occurrence acted on, as a LOCAL epoch. Persisted so a reboot does
// not re-ring something that already rang — and, just as important, so a
// reboot DURING a fire still catches it: the stored value is older than the
// occurrence, so the search finds it again and the grace window decides.
#define NVS_NAMESPACE "rr_sched"
#define NVS_KEY_LAST  "last_handled"
#define NVS_KEY_SKIP  "handled_ids"

// A routine scheduled at the same minute as one already handled has to be
// findable without re-finding the handled one. Four is well past any real
// collision — the app schedules a handful of routines a day, not dozens.
#define MAX_SKIP 4

typedef struct {
    bool valid;
    int64_t occ_epoch;        /**< LOCAL epoch of the scheduled moment */
    int64_t not_before;       /**< snooze target; 0 = ring as soon as possible */
    bool ringing;             /**< the alarm screen is up and unanswered */
    int64_t shown_at;
    bool deferral_logged;     /**< so a 30-min defer logs once, not 90 times */
    char assignment_id[40];
    char routine_name[64];
    char routine_emoji[16];
    int  minute_of_day;
} pending_t;

static TaskHandle_t s_task;
static void (*s_wake_hook)(void);

static int64_t s_last_handled;                  /**< LOCAL epoch */
static char    s_skip[MAX_SKIP][40];            /**< ids handled AT s_last_handled */
static int     s_skip_count;

static pending_t s_pending;
static char s_rearm_reason[32] = "boot";

// Touched from the LVGL task (the alarm buttons) and read by the scheduler
// task. Both are single writes of a small enum, and the scheduler re-reads
// them on its next pass; a mutex here would buy nothing but a deadlock risk
// against the display lock.
typedef enum { ANSWER_NONE = 0, ANSWER_START, ANSWER_SNOOZE } answer_t;
static volatile answer_t s_answer;

// ── time helpers ────────────────────────────────────────────────────────────

/** Local epoch = UTC epoch + the stored offset. 0 if the clock is unusable. */
static int64_t local_now(void)
{
    rr_rtc_time_t t;
    if (rr_rtc_get(&t) != ESP_OK) return 0;
    if (!t.osc_ok) return 0;             // never set: there is no "now" to use
    uint32_t utc = rr_rtc_get_epoch();
    if (utc == 0) return 0;
    return (int64_t) utc + rr_rtc_get_utc_offset();
}

/** ISO weekday (1=Mon..7=Sun) for a local epoch. 1970-01-01 was a Thursday. */
static int weekday_of(int64_t local_epoch)
{
    int64_t days = local_epoch / 86400;
    if (local_epoch < 0) days -= 1;
    return (int) (((days + 3) % 7 + 7) % 7) + 1;   // +3: Thu is ISO day 4
}

static int64_t midnight_of(int64_t local_epoch)
{
    int64_t d = local_epoch % 86400;
    if (d < 0) d += 86400;
    return local_epoch - d;
}

static void fmt_local(int64_t local_epoch, char *buf, size_t cap)
{
    const int64_t day = midnight_of(local_epoch);
    const int mins = (int) ((local_epoch - day) / 60);
    static const char *const WD[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    snprintf(buf, cap, "%s %02d:%02d", WD[weekday_of(local_epoch) - 1], mins / 60, mins % 60);
}

// ── persistence ─────────────────────────────────────────────────────────────

// The "already dealt with" marker, and why BOTH halves are persisted.
//
// s_last_handled alone is not enough. The search is inclusive of that minute,
// because two routines can share one trigger time and the second still has to
// be findable. So the ids ALREADY dealt with at that minute have to be
// remembered too — and if they live only in RAM, a reboot forgets them and the
// watch re-rings a routine that already rang.
//
// That is not hypothetical: it happened on hardware. A routine rang at 09:10,
// was snoozed, the watch was rebooted at 09:17, and it rang again — the reboot
// dropped the skip list, the search re-found 09:10, and 7 minutes was still
// inside the 30-minute grace window so it looked like a legitimate missed fire.
// Persisting the pair makes "handled" mean the same thing either side of a
// reboot, while a genuinely missed fire (one where last_handled is older than
// the occurrence) still rings.
typedef struct {
    int32_t count;
    char    ids[MAX_SKIP][40];
} handled_ids_t;

static void handled_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    int64_t v = 0;
    if (nvs_get_i64(h, NVS_KEY_LAST, &v) == ESP_OK) s_last_handled = v;

    handled_ids_t blob;
    size_t len = sizeof(blob);
    if (nvs_get_blob(h, NVS_KEY_SKIP, &blob, &len) == ESP_OK && len == sizeof(blob) &&
        blob.count >= 0 && blob.count <= MAX_SKIP) {
        s_skip_count = blob.count;
        for (int i = 0; i < s_skip_count; i++) strlcpy(s_skip[i], blob.ids[i], sizeof(s_skip[0]));
    }
    nvs_close(h);
}

static void handled_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        // Survivable: the watch keeps working, but a reboot could re-ring the
        // routine that just fired. Worth a line, since that symptom is baffling
        // without it.
        ESP_LOGW(TAG, "could not persist last-handled — a reboot may repeat this fire");
        return;
    }

    handled_ids_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.count = s_skip_count;
    for (int i = 0; i < s_skip_count && i < MAX_SKIP; i++) {
        strlcpy(blob.ids[i], s_skip[i], sizeof(blob.ids[0]));
    }

    esp_err_t a = nvs_set_i64(h, NVS_KEY_LAST, s_last_handled);
    esp_err_t b = nvs_set_blob(h, NVS_KEY_SKIP, &blob, sizeof(blob));
    if (a == ESP_OK && b == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

/** Mark one occurrence dealt with — in RAM and on flash, always together. */
static void handled_commit(int64_t occ, const char *id)
{
    if (occ > s_last_handled) {
        s_last_handled = occ;
        s_skip_count = 0;          // a new minute: the previous ids no longer apply
    }
    if (id != NULL && id[0] != '\0' && s_skip_count < MAX_SKIP) {
        strlcpy(s_skip[s_skip_count], id, sizeof(s_skip[0]));
        s_skip_count++;
    }
    handled_save();
}

// ── the alarm screen's two answers (these run on the LVGL task) ─────────────

static void on_alarm_start(void)  { s_answer = ANSWER_START;  rr_sched_rearm("alarm answered"); }
static void on_alarm_snooze(void) { s_answer = ANSWER_SNOOZE; rr_sched_rearm("snoozed"); }

// ── firing ──────────────────────────────────────────────────────────────────

static void ring(void)
{
    char when[24];
    fmt_local(s_pending.occ_epoch, when, sizeof(when));
    ESP_LOGI(TAG, "════ ALARM: '%s' scheduled %s ════", s_pending.routine_name, when);

    // Screen first: the tone and the screen arrive together, and waking after
    // the sound has already started reads as a glitch rather than an alarm.
    if (s_wake_hook != NULL) s_wake_hook();

    // The ONLY alerting channel on this board — there is no vibration motor
    // (§2). If this is silent the routine is invisible, so the failure is
    // logged at ERROR rather than swallowed.
    if (rr_audio_play_tone(RR_TONE_ALARM) != ESP_OK) {
        ESP_LOGE(TAG, "ALARM TONE DID NOT PLAY — the fire is visual only");
    }

    rr_child_t child;
    const char *lang = (rr_store_get_child(&child) == ESP_OK) ? child.language : "en";

    s_answer = ANSWER_NONE;
    rr_ui_show_alarm(s_pending.routine_name, s_pending.routine_emoji,
                     s_pending.minute_of_day / 60, s_pending.minute_of_day % 60,
                     lang, on_alarm_start, on_alarm_snooze);

    s_pending.ringing = true;
    s_pending.shown_at = local_now();
}

static void pending_clear(const char *why)
{
    if (s_pending.valid) {
        ESP_LOGI(TAG, "pending fire cleared: %s", why);
    }
    memset(&s_pending, 0, sizeof(s_pending));
    s_answer = ANSWER_NONE;
}

/**
 * Service the alarm currently in flight.
 * Returns the seconds to wait before looking again.
 */
static int service_pending(int64_t now)
{
    const int64_t late_by = now - s_pending.occ_epoch;

    // ── the child answered ──────────────────────────────────────────────────
    if (s_pending.ringing && s_answer != ANSWER_NONE) {
        const answer_t a = s_answer;
        s_answer = ANSWER_NONE;

        if (a == ANSWER_START) {
            rr_audio_stop();
            // THE SHARED START PATH. Not a second mechanism — the same call
            // the phone's remote start makes, so the busy rule, the cache
            // lookup and the LVGL hand-off are decided in exactly one place.
            const rr_start_result_t r = rr_routine_request_start(s_pending.assignment_id);
            if (r != RR_START_OK) {
                ESP_LOGW(TAG, "scheduled start of '%s' refused (%d)",
                         s_pending.routine_name, (int) r);
            }
            pending_clear("started by the child");
            return 1;
        }

        // Snooze. Deliberately keeps the SAME occurrence pending rather than
        // inventing a new one, so the grace window still measures from the
        // scheduled moment: five more minutes, not a fresh half hour.
        s_pending.ringing = false;
        s_pending.not_before = now + RR_SCHED_SNOOZE_S;
        rr_audio_stop();
        rr_ui_show_last_status();
        if (s_pending.not_before - s_pending.occ_epoch > RR_SCHED_GRACE_WINDOW_S) {
            ESP_LOGW(TAG, "snoozed '%s', but +%d min lands past the %d-min grace "
                          "window — it will NOT ring again",
                     s_pending.routine_name, RR_SCHED_SNOOZE_S / 60,
                     RR_SCHED_GRACE_WINDOW_S / 60);
        } else {
            ESP_LOGI(TAG, "snoozed '%s' for %d min", s_pending.routine_name,
                     RR_SCHED_SNOOZE_S / 60);
        }
        return 1;
    }

    // ── nobody answered ─────────────────────────────────────────────────────
    if (s_pending.ringing) {
        const int64_t up_for = now - s_pending.shown_at;
        if (up_for >= RR_SCHED_ALARM_TIMEOUT_S) {
            ESP_LOGI(TAG, "alarm unanswered for %ds — auto-snoozing", RR_SCHED_ALARM_TIMEOUT_S);
            s_pending.ringing = false;
            s_pending.not_before = now + RR_SCHED_SNOOZE_S;
            rr_audio_stop();
            rr_ui_show_last_status();
            return 1;
        }
        return (int) (RR_SCHED_ALARM_TIMEOUT_S - up_for);
    }

    // ── the moment has passed ───────────────────────────────────────────────
    if (late_by > RR_SCHED_GRACE_WINDOW_S) {
        ESP_LOGW(TAG, "MISSED '%s' — %" PRId64 " min late, past the %d-min grace window. "
                      "Not firing: a routine that rings long after its moment is "
                      "worse than one that does not ring.",
                 s_pending.routine_name, late_by / 60, RR_SCHED_GRACE_WINDOW_S / 60);
        pending_clear("grace window expired");
        return 1;
    }

    // ── snoozed, not due yet ────────────────────────────────────────────────
    if (s_pending.not_before > now) {
        return (int) (s_pending.not_before - now);
    }

    // ── DO NOT INTERRUPT a running routine ──────────────────────────────────
    // Same rule remote start obeys: a child three steps into getting dressed
    // is not thrown back to step 1 because the clock said so. It waits, and
    // the grace window above is what eventually gives up.
    if (rr_routine_is_active()) {
        if (!s_pending.deferral_logged) {
            s_pending.deferral_logged = true;
            ESP_LOGW(TAG, "DEFERRED '%s' — a routine is already running. Retrying "
                          "while idle, until %" PRId64 " min of grace run out.",
                     s_pending.routine_name,
                     (RR_SCHED_GRACE_WINDOW_S - late_by) / 60);
        }
        return RR_SCHED_BUSY_RETRY_S;
    }

    if (late_by > 0 && s_pending.deferral_logged) {
        ESP_LOGI(TAG, "watch is idle again — firing deferred '%s', %" PRId64 " min late "
                      "(inside the %d-min grace window)",
                 s_pending.routine_name, late_by / 60, RR_SCHED_GRACE_WINDOW_S / 60);
    }

    ring();
    return RR_SCHED_ALARM_TIMEOUT_S;
}

// ── the search ──────────────────────────────────────────────────────────────

/**
 * Take the next occurrence after s_last_handled into s_pending, if one is due.
 * Returns the seconds to sleep before the next look.
 */
static int find_next(int64_t now)
{
    // Bound how far back the search starts, so a watch that was off for a week
    // walks one day of history rather than seven.
    int64_t floor = s_last_handled;
    const int64_t oldest = now - RR_SCHED_MAX_LOOKBACK_S;
    int skip_count = s_skip_count;
    if (floor < oldest) {
        floor = oldest;
        skip_count = 0;    // the skip list belongs to a minute we have left behind
    }

    for (int guard = 0; guard < 64; guard++) {
        const int from_wd = weekday_of(floor);
        const int from_mins = (int) ((floor - midnight_of(floor)) / 60);

        rr_schedule_hit_t hit;
        const char *skip_argv[MAX_SKIP];
        for (int i = 0; i < skip_count && i < MAX_SKIP; i++) skip_argv[i] = s_skip[i];

        esp_err_t err = rr_store_next_schedule(from_wd, from_mins,
                                               skip_count > 0 ? skip_argv : NULL,
                                               skip_count, &hit);
        if (err != ESP_OK) {
            ESP_LOGI(TAG, "nothing scheduled — idle until re-armed");
            return RR_SCHED_RESYNC_CAP_S;
        }

        const int64_t occ = midnight_of(floor)
                          + (int64_t) hit.days_ahead * 86400
                          + (int64_t) hit.minute_of_day * 60;

        if (occ > now) {
            char when[24];
            fmt_local(occ, when, sizeof(when));
            const int64_t in_s = occ - now;
            ESP_LOGI(TAG, "next fire: '%s' %s (in %" PRId64 "h %" PRId64 "m) [re-armed: %s]",
                     hit.routine_name, when, in_s / 3600, (in_s % 3600) / 60, s_rearm_reason);
            return (int) (in_s > RR_SCHED_RESYNC_CAP_S ? RR_SCHED_RESYNC_CAP_S : in_s);
        }

        // Due, or past. Consume it either way — leaving it would make the
        // search return the same occurrence forever.
        const int64_t late_by = now - occ;
        handled_commit(occ, hit.assignment_id);
        floor = occ;
        skip_count = s_skip_count;

        if (late_by > RR_SCHED_GRACE_WINDOW_S) {
            char when[24];
            fmt_local(occ, when, sizeof(when));
            ESP_LOGW(TAG, "slept through '%s' (%s, %" PRId64 " min ago) — outside the "
                          "%d-min grace window, marking it skipped",
                     hit.routine_name, when, late_by / 60, RR_SCHED_GRACE_WINDOW_S / 60);
            continue;   // look for the next one
        }

        memset(&s_pending, 0, sizeof(s_pending));
        s_pending.valid = true;
        s_pending.occ_epoch = occ;
        s_pending.minute_of_day = hit.minute_of_day;
        strlcpy(s_pending.assignment_id, hit.assignment_id, sizeof(s_pending.assignment_id));
        strlcpy(s_pending.routine_name, hit.routine_name, sizeof(s_pending.routine_name));
        strlcpy(s_pending.routine_emoji, hit.routine_emoji, sizeof(s_pending.routine_emoji));
        return 0;   // service it immediately
    }

    ESP_LOGE(TAG, "schedule search did not converge — backing off");
    return RR_SCHED_RESYNC_CAP_S;
}

// ── the task ────────────────────────────────────────────────────────────────

static void sched_task(void *arg)
{
    (void) arg;

    // Part A's verification, run here rather than in app_main so it costs a
    // few seconds of a background task instead of delaying every boot.
    rr_rtc_alarm_selftest(3);

    handled_load();
    if (s_last_handled != 0) {
        char when[24];
        fmt_local(s_last_handled, when, sizeof(when));
        ESP_LOGI(TAG, "resuming after reboot — last handled fire was %s", when);
    }

    bool warned_no_clock = false, warned_no_tz = false;

    for (;;) {
        int wait_s;
        const int64_t now = local_now();

        if (now == 0) {
            // No trustworthy clock, so no trustworthy schedule. Firing off an
            // unset RTC would ring at an arbitrary hour, which on a child's
            // wrist is worse than staying quiet until TIME_SYNC arrives.
            if (!warned_no_clock) {
                warned_no_clock = true;
                ESP_LOGW(TAG, "RTC not set — scheduling is SUSPENDED until a phone "
                              "sends TIME_SYNC");
            }
            wait_s = 60;
        } else {
            if (warned_no_clock) {
                warned_no_clock = false;
                ESP_LOGI(TAG, "clock is valid again — scheduling resumed");
            }
            if (!rr_rtc_has_utc_offset() && !warned_no_tz) {
                warned_no_tz = true;
                // Schedules are LOCAL "HH:MM". With no offset, local == UTC and
                // every fire lands wrong by the timezone. Say so loudly — this
                // is the exact class of bug that made the Phase 6 next-routine
                // hint wrong, and it is invisible from the outside.
                ESP_LOGW(TAG, "no set_tz received — treating local as UTC, so fires "
                              "will be off by this watch's real offset");
            }

            wait_s = s_pending.valid ? service_pending(now) : find_next(now);

            // find_next() returns 0 when it has just taken an occurrence that
            // is already due. Ring it in this same pass rather than sleeping a
            // second first — the alarm is supposed to land on the minute.
            if (wait_s == 0 && s_pending.valid) wait_s = service_pending(now);
        }

        if (wait_s < 1) wait_s = 1;
        if (wait_s > RR_SCHED_RESYNC_CAP_S) wait_s = RR_SCHED_RESYNC_CAP_S;

        // The whole point: the task is blocked here for the entire interval —
        // one wake per fire, and the CPU is free to idle in between. A
        // re-arm (new routines, clock change, routine finished) cuts it short.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((uint32_t) wait_s * 1000));
    }
}

// ── API ─────────────────────────────────────────────────────────────────────

esp_err_t rr_sched_init(void)
{
    if (s_task != NULL) return ESP_OK;

    if (xTaskCreate(sched_task, "rr_sched", TASK_STACK, NULL, TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "could not start the scheduler task — NOTHING WILL FIRE");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "scheduler up (grace %d min, snooze %d min)",
             RR_SCHED_GRACE_WINDOW_S / 60, RR_SCHED_SNOOZE_S / 60);
    return ESP_OK;
}

void rr_sched_rearm(const char *reason)
{
    if (reason != NULL) strlcpy(s_rearm_reason, reason, sizeof(s_rearm_reason));
    if (s_task != NULL) xTaskNotifyGive(s_task);
}

bool rr_sched_alarm_is_showing(void)
{
    return s_pending.valid && s_pending.ringing;
}

void rr_sched_set_wake_hook(void (*fn)(void))
{
    s_wake_hook = fn;
}
