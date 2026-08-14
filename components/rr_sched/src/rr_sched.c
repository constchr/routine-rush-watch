// rr_sched — the on-watch scheduler (§7). See rr_sched.h for the wake design
// and why the RTC alarm interrupt is not the wake source on this board.
//
// ── The loop, in one place ──────────────────────────────────────────────────
//
//   1. read the RTC, convert to LOCAL (schedules are authored local "HH:MM")
//   2. if a fire is already pending, service it (ring / repeat / defer / drop)
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
// NOT rr_ui: the fire no longer builds a screen of its own. It hands the
// occurrence to rr_routine, which owns the READY screen for BOTH start paths.

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
    bool offered;             /**< READY is up for this fire and unanswered */
    int64_t last_ring;        /**< when the alarm tone last played */
    int  rings;               /**< capped by RR_SCHED_ALARM_MAX_RINGS */
    bool deferral_logged;     /**< so a 30-min defer logs once, not 90 times */
    char assignment_id[40];
    char routine_name[64];
} pending_t;

static TaskHandle_t s_task;
static void (*s_wake_hook)(void);

static int64_t s_last_handled;                  /**< LOCAL epoch */
static char    s_skip[MAX_SKIP][40];            /**< ids handled AT s_last_handled */
static int     s_skip_count;

static pending_t s_pending;
static char s_rearm_reason[32] = "boot";

// Touched from the LVGL task (the READY buttons, via rr_routine's answer hook)
// and read by the scheduler task. Both are single writes of a small enum, and
// the scheduler re-reads them on its next pass; a mutex here would buy nothing
// but a deadlock risk against the display lock.
//
// ANSWER_DISMISS replaced ANSWER_SNOOZE: "Not now" ends THIS occurrence rather
// than pushing it five minutes out. Tomorrow's schedule is untouched — the
// occurrence is committed to the handled list like any other, and the search
// moves on to the next one.
typedef enum { ANSWER_NONE = 0, ANSWER_START, ANSWER_DISMISS } answer_t;
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

// ── the child's answer, relayed from the READY screen ───────────────────────
//
// Registered with rr_routine (this module already depends on it, so no cycle
// and nothing for main.c to wire). It fires for EVERY answered offer, phone
// starts included, so the id is checked: a parent starting bedtime by hand
// must not close out an unrelated scheduled occurrence.
//
// Runs on the LVGL task.
static void on_ready_answer(const char *assignment_id, bool started)
{
    if (!s_pending.valid || !s_pending.offered) return;
    if (assignment_id == NULL || strcmp(assignment_id, s_pending.assignment_id) != 0) return;

    s_answer = started ? ANSWER_START : ANSWER_DISMISS;
    rr_sched_rearm(started ? "READY started" : "READY dismissed");
}

// ── firing ──────────────────────────────────────────────────────────────────

static void pending_clear(const char *why);

// The tone, on its own. Split out of ring() because an unanswered offer
// re-sounds it every RR_SCHED_ALARM_REPEAT_S without rebuilding anything.
static void sound_alarm(void)
{
    // Screen first: the tone and the screen arrive together, and waking after
    // the sound has already started reads as a glitch rather than an alarm.
    // Repeats wake too — a repeat exists precisely for a child who was not
    // looking the first time, and one they cannot see is just noise.
    if (s_wake_hook != NULL) s_wake_hook();

    // The ONLY alerting channel on this board — there is no vibration motor
    // (§2). If this is silent the routine is invisible, so the failure is
    // logged at ERROR rather than swallowed.
    // play_alarm, not play_tone: it flushes anything queued so the alarm is
    // never third in line behind a step blip.
    if (rr_audio_play_alarm() != ESP_OK) {
        ESP_LOGE(TAG, "ALARM TONE DID NOT PLAY — the fire is visual only");
    }

    s_pending.rings++;
    s_pending.last_ring = local_now();
}

static void ring(void)
{
    char when[24];
    fmt_local(s_pending.occ_epoch, when, sizeof(when));
    ESP_LOGI(TAG, "════ ALARM: '%s' scheduled %s ════", s_pending.routine_name, when);

    s_answer = ANSWER_NONE;
    sound_alarm();

    // THE SHARED START PATH, and now the shared SCREEN too: this is the same
    // call the phone's remote start makes, and it raises READY rather than
    // running anything. The routine begins when the child taps START, so the
    // scheduled and remote paths are one behaviour with one screen instead of
    // two that had drifted apart.
    const rr_start_result_t r = rr_routine_request_start(s_pending.assignment_id);
    if (r != RR_START_OK) {
        // BUSY is handled before we get here (the deferral branch), so this is
        // a cache problem: the routine was in the schedule and is not in the
        // cache. Ringing at a child with nothing on screen to explain it is
        // worse than dropping the fire.
        ESP_LOGE(TAG, "scheduled offer of '%s' refused (%d) — dropping this fire",
                 s_pending.routine_name, (int) r);
        rr_audio_stop();
        pending_clear("could not raise READY");
        return;
    }

    s_pending.offered = true;
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
    // rr_routine has already acted on the tap — START ran the routine, NOT NOW
    // put the face back. Nothing to do here but close the books, which for
    // BOTH answers means the same thing: this occurrence is done with. NOT NOW
    // dismisses it for today; tomorrow's is a different occurrence and is not
    // affected, because it is found by the ordinary search from s_last_handled.
    if (s_pending.offered && s_answer != ANSWER_NONE) {
        const answer_t a = s_answer;
        s_answer = ANSWER_NONE;
        rr_audio_stop();
        pending_clear(a == ANSWER_START ? "started by the child"
                                        : "dismissed for today — not now");
        return 1;
    }

    // ── offered, nobody has answered yet ────────────────────────────────────
    if (s_pending.offered) {
        // Give up on the whole fire once the moment is properly past. The
        // offer outliving its grace window would leave READY on the glass all
        // day AND block every later fire, since one occurrence is in flight at
        // a time.
        if (late_by > RR_SCHED_GRACE_WINDOW_S) {
            ESP_LOGW(TAG, "READY for '%s' went unanswered for the whole %d-min grace "
                          "window — withdrawing it", s_pending.routine_name,
                     RR_SCHED_GRACE_WINDOW_S / 60);
            rr_routine_cancel_ready(s_pending.assignment_id);
            pending_clear("offer expired unanswered");
            return 1;
        }

        // Ring again, up to the cap. After that the watch goes QUIET AND THE
        // OFFER STAYS UP: the child has had ~80 s of noise, and re-ringing at
        // someone who has already seen the screen is how an alarm becomes
        // something a parent disables. Sleeping the panel does not lose it —
        // main.c's face gate re-shows READY on the next wake.
        if (s_pending.rings >= RR_SCHED_ALARM_MAX_RINGS) {
            return (int) (RR_SCHED_GRACE_WINDOW_S - late_by);
        }
        const int64_t since = now - s_pending.last_ring;
        if (since >= RR_SCHED_ALARM_REPEAT_S) {
            ESP_LOGI(TAG, "READY for '%s' still unanswered — alarm %d/%d",
                     s_pending.routine_name, s_pending.rings + 1, RR_SCHED_ALARM_MAX_RINGS);
            sound_alarm();
            return RR_SCHED_ALARM_REPEAT_S;
        }
        return (int) (RR_SCHED_ALARM_REPEAT_S - since);
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
    return RR_SCHED_ALARM_REPEAT_S;
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
        strlcpy(s_pending.assignment_id, hit.assignment_id, sizeof(s_pending.assignment_id));
        strlcpy(s_pending.routine_name, hit.routine_name, sizeof(s_pending.routine_name));
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
    // The child's answer comes back through rr_routine, which owns the READY
    // screen. Registered here rather than wired in main.c because this module
    // already depends on rr_routine — the arrow exists, so no cycle is created
    // and main.c has nothing to hold together.
    rr_routine_set_answer_hook(on_ready_answer);

    ESP_LOGI(TAG, "scheduler up (grace %d min, alarm %d x %ds)",
             RR_SCHED_GRACE_WINDOW_S / 60, RR_SCHED_ALARM_MAX_RINGS,
             RR_SCHED_ALARM_REPEAT_S);
    return ESP_OK;
}

void rr_sched_rearm(const char *reason)
{
    if (reason != NULL) strlcpy(s_rearm_reason, reason, sizeof(s_rearm_reason));
    if (s_task != NULL) xTaskNotifyGive(s_task);
}

void rr_sched_set_wake_hook(void (*fn)(void))
{
    s_wake_hook = fn;
}
