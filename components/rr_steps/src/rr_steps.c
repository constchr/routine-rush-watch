// rr_steps — daily step count over the QMI8658 on-chip pedometer (§10.1).
// See rr_steps.h for scope and the local-midnight rationale.

#include "rr_steps.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "rr_imu.h"
#include "rr_rtc.h"

static const char *TAG = "rr_steps";

#define NVS_NAMESPACE "rr_steps"
#define NVS_KEY_DAY   "day"      /**< local days since the epoch */
#define NVS_KEY_COUNT "count"

#define TASK_STACK 3072
#define TASK_PRIO  2             /**< below the UI; a step is never urgent */

static SemaphoreHandle_t s_lock;
static bool     s_engine_ok;
static bool     s_day_known;

static uint32_t s_daily;         /**< steps since local midnight */
static int32_t  s_day;           /**< local day number the total belongs to */

static uint32_t s_persisted;     /**< s_daily as last written */
static int64_t  s_persisted_at;  /**< tick ms of that write */

// ── Liveness of the detector's INPUT, not just its output ───────────────────
//
// ⚠️ THIS EXISTS BECAUSE A DEAD SENSOR AND A STATIONARY WATCH LOOK IDENTICAL
// FROM THE STEP COUNT ALONE, and this board spent from Phase 9 to Phase 10 in
// exactly that ambiguity: arming the IMU's wake-on-motion froze its output
// registers, so the detector was fed a CONSTANT for every hour the screen was
// off and counted nothing — while "steps 128, unchanged" read as "nobody
// walked". Wake-on-motion is gone now, but the lesson is cheap to keep.
//
// The spread between the smallest and largest magnitude seen since the last
// report is the discriminator: a real accelerometer at rest still jitters by a
// few mg, and a frozen one reports EXACTLY 0.000 forever. Reported to the
// heartbeat so a regression is visible in the log rather than in a step count
// that quietly stops growing.
static float    s_mag_min = 0.0f;
static float    s_mag_max = 0.0f;
static float    s_mag_last = 0.0f;
static uint32_t s_samples;
static bool     s_mag_seen;

// ── Tuning telemetry: the band-passed SIGNAL, not the raw magnitude ─────────
//
// The first real walk test credited 16 steps for ~50 taken. That is a threshold
// problem, and the only way to fix it without another round of guessing is to
// know what amplitude a real stride actually produces ON A WRIST — the detector
// compares `signal` (fast EMA minus slow EMA) against THRESH_HIGH_G, so
// `signal` is the number to look at, and the raw magnitude says nothing about
// it.
//
// s_sig_peak is the largest signal seen since the last report. Compare it
// directly with THRESH_HIGH_G: if a brisk walk peaks at 0.08 g against a 0.11 g
// gate, the gate is simply too high and most strides never register.
// s_detects counts threshold crossings, so "peaks found" and "steps credited"
// can be told apart — a big gap between them means the rhythm/streak gates are
// throwing away real steps rather than the amplitude gate missing them.
static float    s_sig_peak;
static uint32_t s_detects;

static int64_t now_ms(void)
{
    return (int64_t) xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/**
 * Today's LOCAL day number, or INT32_MIN if the clock cannot say.
 *
 * Derived from the local wall-clock date rather than by dividing an epoch, so
 * it stays correct for the half-hour and three-quarter-hour offsets (India,
 * Nepal, Chatham) where a naive epoch/86400 lands on the wrong side of
 * midnight. Any monotonic integer per calendar day works — this one is
 * "days since 2000-01-01" via the same civil-date arithmetic rr_rtc uses.
 */
static int32_t local_day_number(void)
{
    rr_rtc_time_t t;
    if (rr_rtc_get(&t) != ESP_OK || !t.osc_ok) return INT32_MIN;
    if (rr_rtc_get_local(&t) != ESP_OK) return INT32_MIN;

    int y = t.year;
    unsigned m = t.month;
    const unsigned d = t.day;
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned) (y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int32_t) ((long) era * 146097 + (long) doe - 730425);   /* 0 = 2000-01-01 */
}

// ── persistence ─────────────────────────────────────────────────────────────

static void store_now(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "cannot open NVS — today's count will not survive a reboot");
        return;
    }
    esp_err_t a = nvs_set_i32(h, NVS_KEY_DAY, s_day);
    esp_err_t b = nvs_set_u32(h, NVS_KEY_COUNT, s_daily);
    if (a == ESP_OK && b == ESP_OK) nvs_commit(h);
    nvs_close(h);

    s_persisted = s_daily;
    s_persisted_at = now_ms();
}

static void maybe_store(void)
{
    if (s_daily == s_persisted) return;
    if (s_daily - s_persisted >= RR_STEPS_PERSIST_DELTA ||
        now_ms() - s_persisted_at >= (int64_t) RR_STEPS_PERSIST_EVERY_S * 1000) {
        store_now();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// The step detector (software — the on-chip engine is inert, see rr_imu.h)
//
// A textbook wrist pedometer, kept deliberately cheap because it runs on the
// main CPU:
//
//   magnitude -> band-pass -> hysteresis peak detect -> rhythm gate -> streak
//
//   1. |a| removes orientation: a wrist rotates constantly and per-axis
//      thresholds would fire on turning the hand over.
//   2. A fast EMA smooths sensor noise; a slow EMA tracks gravity and posture.
//      Their difference is the oscillation walking actually produces, centred
//      on zero regardless of how the arm is held.
//   3. Hysteresis (rise above HIGH, must fall below LOW before re-arming) stops
//      one step's peak being counted several times as it wobbles.
//   4. A rhythm gate rejects anything too fast or too slow to be a stride.
//   5. A streak gate credits nothing until several in-rhythm steps have
//      happened, then credits them retroactively — this is the mechanism that
//      keeps arm-waving out, because waving matches every earlier test (it has
//      the right amplitude and roughly the right rate) and only fails to
//      *continue* like walking does.
// ═════════════════════════════════════════════════════════════════════════════

// Tuned for a CHILD's gait, biased against over-counting per the brief.
//
//   THRESH_HIGH_G / LOW_G   A step must swing 0.11 g above the running
//                           baseline, and fall back under 0.045 g before
//                           another can be detected. A child's wrist swings
//                           harder than an adult's, so this is not the limiting
//                           factor; it mostly rejects fidgeting.
//                           → LOWER IF THE WALK TEST READS SHORT.
//
//   MIN_STEP_MS 260         ~3.8 steps/s ceiling. Fast enough for a running
//                           child, and it is what rejects the 4-6 Hz buzz of a
//                           shaken arm.
//   MAX_STEP_MS 1500        1.5 s floor. A longer gap ends the streak — a
//                           child ambling and stopping constantly is exactly
//                           why this is generous rather than tight.
//
//   ENTRY_STEPS 8           In-rhythm steps required before ANY are credited.
//                           The single most important anti-false-positive knob.
//                           8 at a child's cadence is ~3 seconds of continuous
//                           walking, which arm-waving and swinging rarely
//                           sustain in rhythm, while any real walk clears it.
//                           → RAISE IF WAVING STILL COUNTS.
//                           → LOWER IF SHORT WALKS GO UNCOUNTED.
// ── RETUNED FROM MEASURED WRIST DATA (Phase 10) ─────────────────────────────
//
// The first walk that ever reached this detector — step counting was dead
// before Phase 10 — credited 16 steps for ~50 taken, then 0 for a minute of
// walking on the spot. The instrumentation in rr_steps_describe_input() said
// why, and it was not the amplitude gate:
//
//     accel 0.92g sprd 2.850 n=758 pk 0.396 det 14   -> credited 0
//     accel 0.99g sprd 0.569 n=750 pk 0.210 det 18   -> credited 0
//     accel 0.98g sprd 0.506 n=751 pk 0.170 det 12   -> credited 0
//
// `pk` (0.17-0.40 g) clears THRESH_HIGH_G (0.11 g) with room to spare, so peaks
// were being FOUND. But `det` of 12-18 per 30 s is one detection every ~2 s,
// which is longer than MAX_STEP_MS was — so EVERY detection took the "too slow,
// start a fresh streak" branch, the streak never reached ENTRY_STEPS, and credit
// was structurally guaranteed to be zero. Not a threshold that was slightly off:
// a gate that could never open.
//
// ⚠️ AND THE REASON THE GAPS ARE THAT LONG IS ANATOMY, NOT TUNING. At the WRIST
// the arm swings once per STRIDE — about one peak per TWO steps — so a
// wrist-mounted detector sees roughly half the cadence a hip-mounted one does.
// The old values were implicitly written for one peak per step.
//
//   MAX_STEP_MS 3000        was 1500. Must exceed the real inter-peak gap with
//                           margin, or the streak resets forever. Measured gaps
//                           were ~2000 ms at a gentle indoor pace; a child
//                           dawdling will be slower still.
//                           → RAISE IF A STEADY WALK STILL CREDITS NOTHING.
//
//   ENTRY_STEPS 4           was 8. At roughly one detection per stride, 8 meant
//                           ~16 s of uninterrupted walking before ANY credit —
//                           which almost no real child walk sustains. 4 is ~8 s,
//                           still far more than a wave or a door-slam produces.
//                           → RAISE IF ARM WAVING REGISTERS.
//
//   MIN_STEP_MS 300         was 260. Slightly wider now that one detection means
//                           one stride: two peaks closer than 300 ms cannot be
//                           consecutive strides, so that is still vibration.
//
// ⚠️ STEPS_PER_DETECT exists because of the anatomy note above: each confirmed
// peak is credited as a stride, i.e. TWO steps. Without it the count reads
// consistently half, which is exactly what the 16-for-50 result was (16*2 = 32,
// against a streak that kept breaking). Verify against a hand-counted walk
// before trusting it — it is the one value here that is a model rather than a
// measurement.
#define THRESH_HIGH_G  0.11f
#define THRESH_LOW_G   0.045f
#define MIN_STEP_MS    300
#define MAX_STEP_MS    3000
#define ENTRY_STEPS    4
#define STEPS_PER_DETECT 2

// EMA coefficients at RR_STEPS_SAMPLE_HZ. FAST is a light smoother (~6 Hz
// corner); SLOW tracks gravity and posture over a couple of seconds so that
// lifting the arm shifts the baseline instead of registering as a step.
#define EMA_FAST 0.45f
#define EMA_SLOW 0.02f

static float s_fast, s_slow;
static bool  s_primed;
static bool  s_above;          /**< hysteresis state */
static int64_t s_last_step_ms;
static int   s_streak;         /**< consecutive in-rhythm steps so far */
static uint32_t s_pending;     /**< detected but not yet credited */

/** Feed one accelerometer sample. Returns steps to credit (usually 0). */
static uint32_t detect(float mag_g, int64_t t_ms)
{
    if (!s_primed) {
        s_fast = s_slow = mag_g;
        s_primed = true;
        return 0;
    }

    s_fast += (mag_g - s_fast) * EMA_FAST;
    s_slow += (s_fast - s_slow) * EMA_SLOW;

    const float signal = s_fast - s_slow;
    if (signal > s_sig_peak) s_sig_peak = signal;

    if (!s_above && signal > THRESH_HIGH_G) {
        s_above = true;
        s_detects++;

        const int64_t gap = t_ms - s_last_step_ms;
        s_last_step_ms = t_ms;
        ESP_LOGD(TAG, "peak %.3fg gap %lldms streak %d", (double) signal,
                 (long long) gap, s_streak);

        if (gap < MIN_STEP_MS) {
            // Too soon to be a stride: this is vibration, not walking. It also
            // breaks the streak, so a shaken arm cannot accumulate credit.
            s_streak = 0;
            s_pending = 0;
            return 0;
        }
        if (gap > MAX_STEP_MS) {
            // First step of a new burst. Start a fresh streak rather than
            // joining it to whatever happened minutes ago.
            s_streak = 1;
            s_pending = 1;
            return 0;
        }

        s_streak++;
        if (s_streak < ENTRY_STEPS) {
            s_pending++;
            return 0;
        }
        if (s_streak == ENTRY_STEPS) {
            // Streak confirmed: credit everything that led up to it, which is
            // why a 50-step walk reports ~50 and not ~42.
            const uint32_t credit = s_pending + 1;
            s_pending = 0;
            return credit * STEPS_PER_DETECT;
        }
        return STEPS_PER_DETECT;   // established rhythm: credit live
    }

    if (s_above && signal < THRESH_LOW_G) s_above = false;

    // A long silence abandons an unconfirmed streak, so pending steps from a
    // stray movement never get credited by a later, unrelated one.
    if (s_streak > 0 && s_streak < ENTRY_STEPS && t_ms - s_last_step_ms > MAX_STEP_MS) {
        s_streak = 0;
        s_pending = 0;
    }
    return 0;
}

// ── day accounting ──────────────────────────────────────────────────────────

/** Roll the day over if local midnight has passed. Call with the lock held. */
static void check_day_locked(void)
{
    const int32_t today = local_day_number();
    if (today == INT32_MIN) return;      // clock unusable; nothing to decide yet

    if (!s_day_known) {
        // The clock became usable (first TIME_SYNC of this watch's life, or of
        // this boot). Whatever day we had been provisionally counting into was a
        // guess, so adopt the real one and keep the steps: they happened, and
        // today is the only day we can honestly file them on.
        s_day = today;
        s_day_known = true;
        ESP_LOGI(TAG, "clock valid — counting into day %" PRId32 " (%" PRIu32 " steps carried)",
                 s_day, s_daily);
        store_now();
        return;
    }

    if (today != s_day) {
        ESP_LOGI(TAG, "════ LOCAL MIDNIGHT — day %" PRId32 " ended on %" PRIu32
                      " steps, resetting ════", s_day, s_daily);
        s_day = today;
        s_daily = 0;
        store_now();
    }
}

void rr_steps_refresh(void)
{
    if (s_lock == NULL) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    check_day_locked();
    maybe_store();
    xSemaphoreGive(s_lock);
}

// ── the sampling task ───────────────────────────────────────────────────────
//
// ⚠️ THIS IS THE POWER COST OF THE INERT ON-CHIP ENGINE. It wakes
// RR_STEPS_SAMPLE_HZ times a second, forever, including while the watch is
// "asleep" — see the power note in rr_steps.h. Phase 10 owns fixing it; Phase 9
// owns naming it.
static void steps_task(void *arg)
{
    (void) arg;

    const TickType_t period = pdMS_TO_TICKS(1000 / RR_STEPS_SAMPLE_HZ);
    TickType_t last_wake = xTaskGetTickCount();
    int day_check_countdown = RR_STEPS_SAMPLE_HZ * RR_STEPS_POLL_S;

    for (;;) {
        // vTaskDelayUntil, not vTaskDelay: the detector's timing gates are in
        // milliseconds, so the sample interval has to be the interval asked for
        // and not "that plus however long the I2C read took".
        vTaskDelayUntil(&last_wake, period);

        float x = 0, y = 0, z = 0;
        if (rr_imu_read_accel_g(&x, &y, &z) != ESP_OK) continue;
        const float mag = sqrtf(x * x + y * y + z * z);
        const int64_t t = now_ms();

        // Track the spread before anything else touches the sample.
        if (!s_mag_seen) { s_mag_min = s_mag_max = mag; s_mag_seen = true; }
        else if (mag < s_mag_min) s_mag_min = mag;
        else if (mag > s_mag_max) s_mag_max = mag;
        s_mag_last = mag;
        s_samples++;

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) continue;

        const uint32_t credit = detect(mag, t);
        if (credit > 0) {
            s_daily += credit;
            ESP_LOGI(TAG, "steps +%" PRIu32 " -> %" PRIu32 " today", credit, s_daily);
            maybe_store();
        }

        // The day boundary and the periodic persist do not need checking at
        // sample rate — once a minute is plenty and keeps the hot path to
        // arithmetic.
        if (--day_check_countdown <= 0) {
            day_check_countdown = RR_STEPS_SAMPLE_HZ * RR_STEPS_POLL_S;
            check_day_locked();
            maybe_store();
        }

        xSemaphoreGive(s_lock);
    }
}

// ── API ─────────────────────────────────────────────────────────────────────

esp_err_t rr_steps_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    // Restore first, so a mid-day reboot resumes rather than restarting at 0.
    int32_t stored_day = 0;
    uint32_t stored_count = 0;
    bool have_stored = false;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, NVS_KEY_DAY, &stored_day) == ESP_OK &&
            nvs_get_u32(h, NVS_KEY_COUNT, &stored_count) == ESP_OK) {
            have_stored = true;
        }
        nvs_close(h);
    }

    const int32_t today = local_day_number();
    s_day_known = (today != INT32_MIN);

    if (have_stored && s_day_known && stored_day == today) {
        s_daily = stored_count;
        s_day = today;
        ESP_LOGI(TAG, "resuming today's count: %" PRIu32 " steps (day %" PRId32 ")",
                 s_daily, s_day);
    } else if (have_stored && s_day_known) {
        // A reboot across midnight lands here: the stored total belongs to a day
        // that has ended, so it is history and today starts clean.
        s_daily = 0;
        s_day = today;
        ESP_LOGI(TAG, "stored count was for day %" PRId32 " (%" PRIu32 " steps), today is "
                      "day %" PRId32 " — starting a new day at 0",
                 stored_day, stored_count, today);
    } else if (have_stored) {
        // Clock unusable: keep the stored total rather than throwing away real
        // steps, and let the first valid clock reading decide which day it is.
        s_daily = stored_count;
        s_day = stored_day;
        ESP_LOGW(TAG, "RTC not set — carrying %" PRIu32 " stored steps with an unknown day",
                 s_daily);
    } else {
        s_daily = 0;
        s_day = s_day_known ? today : 0;
    }
    s_persisted = s_daily;
    s_persisted_at = now_ms();

    if (!rr_imu_is_ready()) {
        ESP_LOGE(TAG, "IMU not ready — no step counting; the face keeps its placeholder");
        return ESP_ERR_INVALID_STATE;
    }

    // Pin range and rate for the detector's lifetime. rr_imu owns the sensor;
    // this asks it to hold a mode, it does not configure the QMI8658 itself.
    rr_imu_step_sampling_hold(true);
    s_engine_ok = true;

    if (xTaskCreate(steps_task, "rr_steps", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the step task");
        s_engine_ok = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "step counting live — SOFTWARE detector @ %d Hz (the on-chip engine "
                  "is inert on this part, see rr_imu.h)", RR_STEPS_SAMPLE_HZ);
    ESP_LOGI(TAG, "  gait: peak>=%.3fg release<%.3fg step %d-%dms entry=%d steps",
             THRESH_HIGH_G, THRESH_LOW_G, MIN_STEP_MS, MAX_STEP_MS, ENTRY_STEPS);
    ESP_LOGW(TAG, "  ⚠ this samples the accelerometer from the CPU %d times a second, "
                  "including while asleep — Phase 10 power item", RR_STEPS_SAMPLE_HZ);
    return ESP_OK;
}

void rr_steps_describe_input(char *buf, size_t len)
{
    if (buf == NULL || len == 0) return;
    if (!s_mag_seen) {
        snprintf(buf, len, "accel NO SAMPLES");
        return;
    }
    // Spread of exactly zero over thousands of samples means the registers are
    // frozen, not that the watch is still — say so rather than print a number
    // whose significance only one person remembers.
    const float spread = s_mag_max - s_mag_min;
    snprintf(buf, len, "accel %.2fg sprd %.3f n=%" PRIu32 " pk %.3f det %" PRIu32 "%s",
             (double) s_mag_last, (double) spread, s_samples,
             (double) s_sig_peak, s_detects,
             spread < 0.001f ? " ⚠FROZEN" : "");
    s_mag_min = s_mag_max = s_mag_last;   // per-report window
    s_samples = 0;
    s_sig_peak = 0.0f;
    s_detects = 0;
}

uint32_t rr_steps_today(void) { return s_daily; }

bool rr_steps_valid(void) { return s_engine_ok && s_day_known; }

void rr_steps_flush(void)
{
    if (s_lock == NULL) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (s_daily != s_persisted) store_now();
    xSemaphoreGive(s_lock);
}
