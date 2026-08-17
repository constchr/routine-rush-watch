// rr_powerlog — SOC-drop power measurement. See rr_powerlog.h for why this is
// the only measurement available on this board.
//
// ⚠️ WHY THIS FILE GAINED NVS AND TWO VBUS EDGES (2026-08-14)
//
// The original version could not deliver a result, and the failure was total
// rather than partial. Three things combined:
//
//   1. It only ever called ESP_LOGI. The console is USB-Serial-JTAG and nothing
//      else (CONFIG_ESP_CONSOLE_SECONDARY_NONE=y, UART_NUM=-1), so with the
//      cable out every POWERLOG line went into a FIFO no host was draining and
//      was discarded. docs/POWER.md claimed "the lines are retained in the log";
//      there was no ring buffer, no NVS and no littlefs write. Nothing was.
//   2. The one recoverable path was blocked. The baseline lives in RAM and would
//      survive a replug, so the first sample afterwards could have computed the
//      whole cumulative answer — but the vbus_present branch came FIRST and
//      continue'd, so it printed "ON USB, NOT MEASURING" and the answer was
//      never computed. It also set charged_since_mark, which would have stamped
//      SAMPLE INVALID on it if it had been.
//   3. rr_powerlog_mark() had no callers at all. The documented procedure said
//      to call it once the state was established, and there was no way to: no
//      console REPL, and BOOT is wake (short) or factory reset (10 s hold).
//
// So a 2-hour run on battery produced a silent console, an unprinted answer and
// a baseline taken on the charger. Hence:
//
//   - Every whole-percent change is persisted to NVS, so the answer survives
//     the unplug AND a reboot.
//   - The baseline re-arms ITSELF on the USB->battery edge, so unplugging is the
//     act that starts the measurement. No manual mark is needed or possible.
//   - The battery->USB edge FINALISES and prints the result before the ON-USB
//     guard runs, so replugging ends the window and hands back the number.
//
// The lesson worth keeping: a measurement harness whose only output path is the
// console cannot measure the one state that requires the console to be absent.

#include "rr_powerlog.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "rr_battery.h"
#include "rr_pm.h"

static const char *TAG = "rr_powerlog";

#define TASK_STACK 3072
#define TASK_PRIO  1          /* lowest: measuring must not perturb the thing measured */

#define PLOG_NS    "rr_plog"
#define PLOG_KEY   "run"
// Bumped on every layout change (PLG2 = light-sleep counters, PLG3 = scheduling
// lateness). An old blob is a different size AND a different magic, so it is
// rejected loudly as "no previous run" rather than read as garbage.
#define PLOG_MAGIC 0x504C4733u  /* "PLG3" */

static volatile bool s_running;
static int  s_interval_s = 60;

// Baseline for the rate calculation.
static int64_t  s_base_us;
static uint8_t  s_base_pct;
static bool     s_base_valid;
static char     s_label[32] = "unlabelled";

// Set when USB has been seen at any point since the baseline: a charge event
// invalidates the whole sample, and silently reporting the surviving numbers
// would be worse than saying so.
//
// NOTE the asymmetry with the closing edge: USB appearing is what ENDS a window,
// and a window that ran baseline->now entirely on battery is valid. This flag is
// for the other case — USB appearing and then going away again while the same
// baseline is still in force, i.e. a charge in the middle of the run.
static bool s_charged_since_mark;

// Tracks vbus across samples so the two transitions can be acted on.
// -1 = not yet known (first sample establishes it without firing an edge).
static int s_prev_vbus = -1;

// Light-sleep counters as they stood when the window opened. The counters
// themselves are cumulative since boot, and the window starts at unplug, so only
// the DELTA describes the state under test.
static uint32_t s_base_ls_entries;
static uint64_t s_base_ls_slept_us;

// Scheduling-lateness accumulators, reset with the baseline.
static uint32_t s_samples;
static uint32_t s_late_max_ms;
static uint32_t s_late_over_1s;

// ── Persistence ─────────────────────────────────────────────────────────────
// Written on EVERY sample (~one NVS write a minute) and on finalise. It used to
// be only on whole-percent changes, which meant a replug-reset truncated the
// recovered window back to the last percent step and understated the rate; see
// the note at the save site.
typedef struct {
    uint32_t magic;
    uint8_t  base_pct;
    uint8_t  last_pct;
    uint16_t last_mv;
    int64_t  elapsed_us;        // last sample - baseline, so it survives a reboot
    uint8_t  charged_since_mark;
    uint8_t  finalized;         // the window was closed by USB appearing
    char     label[32];
    // ── Light-sleep counters, folded in 2026-08-14 ──────────────────────────
    // These belong here and not in rr_pm because they have exactly the same
    // problem as the SOC drop: they are only meaningful while USB is detached
    // (the usb_serial_jtag NO_LIGHT_SLEEP lock pins them to ~0 on the bench),
    // so they are produced when no console can read them. Same window, same
    // persistence, same replay-at-boot — one record, one truth.
    uint8_t  ls_enabled;        // was RR_LIGHT_SLEEP compiled in at all
    uint32_t ls_entries;        // light sleeps actually entered
    uint64_t ls_slept_us;       // total time spent in them
    int64_t  ls_window_us;      // wall clock the two above accumulated over
    // ── Scheduling lateness, added 2026-08-17 ───────────────────────────────
    // Does light sleep starve the tasks? The standing warning is that a starved
    // main task can make rr_sched miss a scheduled routine, and that risk has
    // never been measured — only inferred from a task_wdt signature that also
    // appeared with light sleep OFF.
    //
    // This task wakes on a fixed vTaskDelay every interval and is at the same
    // priority as the main task, so how LATE it actually wakes is a direct proxy
    // for whether a periodic task can be scheduled on time under light sleep.
    // Cheap, needs no other component to cooperate, and it accumulates while
    // unplugged where the console cannot see anything.
    uint32_t samples;           // wake-ups observed in the window
    uint32_t late_max_ms;       // worst single lateness vs the nominal interval
    uint32_t late_over_1s;      // wake-ups more than 1 s late
} plog_rec_t;

static void plog_save(const rr_battery_t *b, int64_t elapsed_us, bool finalized)
{
    plog_rec_t r;
    memset(&r, 0, sizeof(r));
    r.magic       = PLOG_MAGIC;
    r.base_pct    = s_base_pct;
    r.last_pct    = b->percent;
    r.last_mv     = b->millivolts;
    r.elapsed_us  = elapsed_us;
    r.charged_since_mark = s_charged_since_mark ? 1 : 0;
    r.finalized   = finalized ? 1 : 0;
    strlcpy(r.label, s_label, sizeof(r.label));

    uint32_t ent = 0;
    uint64_t slept = 0;
    rr_pm_stats(&ent, &slept);
    r.ls_enabled   = rr_pm_light_sleep_enabled() ? 1 : 0;
    r.ls_entries   = ent - s_base_ls_entries;
    r.ls_slept_us  = slept - s_base_ls_slept_us;
    r.ls_window_us = elapsed_us;
    r.samples      = s_samples;
    r.late_max_ms  = s_late_max_ms;
    r.late_over_1s = s_late_over_1s;

    nvs_handle_t h;
    esp_err_t err = nvs_open(PLOG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) failed: %s — this sample is console-only",
                 PLOG_NS, esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(h, PLOG_KEY, &r, sizeof(r));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "persisting the sample failed: %s", esp_err_to_name(err));
    }
}

// Report whatever the last run left behind. Called at start, which is the one
// moment a host is guaranteed to be attached and reading.
static void plog_report_previous(void)
{
    plog_rec_t r;
    size_t len = sizeof(r);
    nvs_handle_t h;

    // Whether USB is present RIGHT NOW disambiguates the two reasons a record can
    // be unfinalised, which matter very differently:
    //
    //   on USB  -> the board reset when the cable went in, before the 60 s sample
    //              that would have finalised. The window is TRUNCATED at the last
    //              save. Numerator and denominator still describe the same instant,
    //              so the rate is sound — it just covers less than the time the
    //              watch was actually unplugged. Harmless.
    //
    //   on batt -> we are running on battery with an unfinalised record, i.e. the
    //              board really did reboot DURING a measurement window. The
    //              baseline was re-taken at boot, so the stored record is stale and
    //              anything after the reboot is lost. Suspect.
    rr_battery_t now_b;
    const bool on_usb_now = (rr_battery_read(&now_b) == ESP_OK) && now_b.vbus_present;

    if (nvs_open(PLOG_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "POWERLOG no previous run stored");
        return;
    }
    esp_err_t err = nvs_get_blob(h, PLOG_KEY, &r, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(r) || r.magic != PLOG_MAGIC) {
        ESP_LOGI(TAG, "POWERLOG no previous run stored");
        return;
    }

    const double elapsed_h = (double) r.elapsed_us / 3600e6;
    const int dropped = (int) r.base_pct - (int) r.last_pct;

    // The light-sleep half of the answer, and the ONLY place it is readable: the
    // counters accumulate while USB is out and would otherwise die on the reset
    // that attaching a host can cause.
    if (r.ls_enabled) {
        const double slp_pct = r.ls_window_us > 0
            ? 100.0 * (double) r.ls_slept_us / (double) r.ls_window_us : 0.0;
        ESP_LOGW(TAG, "POWERLOG ── PREVIOUS RUN, LIGHT SLEEP ── entered %" PRIu32
                      " times, slept %.1f s of %.1f s = %.1f%% residency%s",
                 r.ls_entries, (double) r.ls_slept_us / 1e6,
                 (double) r.ls_window_us / 1e6, slp_pct,
                 r.ls_entries == 0 ? "  ⚠ NEVER ENTERED" : "");
    } else {
        ESP_LOGW(TAG, "POWERLOG ── PREVIOUS RUN, LIGHT SLEEP ── not compiled in "
                      "(no RR_LIGHT_SLEEP) — the CPU was awake for the whole window");
    }

    // The starvation question, measured rather than inferred.
    ESP_LOGW(TAG, "POWERLOG ── PREVIOUS RUN, SCHEDULING ── %" PRIu32 " wake-ups, "
                  "worst lateness %" PRIu32 " ms, %" PRIu32 " over 1 s%s",
             r.samples, r.late_max_ms, r.late_over_1s,
             r.late_over_1s > 0
                 ? "  ⚠ A PERIODIC TASK WAS DELAYED >1 s — rr_sched could miss a fire"
                 : (r.samples > 0 ? "  ✅ no task starvation seen" : ""));

    if (dropped > 0 && elapsed_h > 0.0) {
        const double pct_per_h = (double) dropped / elapsed_h;
        const double ma = pct_per_h * RR_POWERLOG_BATTERY_MAH / 100.0;
        ESP_LOGW(TAG, "POWERLOG ── PREVIOUS RUN ── state=\"%s\" %u%%->%u%% over "
                      "%.2fh | %.2f %%/h ≈ %.1f mA%s%s",
                 r.label, r.base_pct, r.last_pct, elapsed_h, pct_per_h, ma,
                 r.charged_since_mark ? "  ⚠ USB SEEN MID-WINDOW — INVALID" : "",
                 r.finalized ? ""
                     : (on_usb_now
                        ? "  ⓘ TRUNCATED, NOT INVALID — the board reset when USB was "
                          "attached, so the finalise never ran. The rate is sound; the "
                          "window is just shorter than the time you were unplugged."
                        : "  ⚠ REBOOTED DURING THE WINDOW — baseline was re-taken at "
                          "boot, so this record is stale. Re-run."));
    } else {
        ESP_LOGW(TAG, "POWERLOG ── PREVIOUS RUN ── state=\"%s\" %u%%->%u%% over "
                      "%.2fh — no whole percent lost, no rate available%s",
                 r.label, r.base_pct, r.last_pct, elapsed_h,
                 r.finalized ? "" : "  ⚠ NOT FINALISED");
    }
}

static void mark_now(const rr_battery_t *b, const char *label)
{
    s_base_us = esp_timer_get_time();
    s_base_pct = b->percent;
    s_base_valid = true;
    s_charged_since_mark = false;
    rr_pm_stats(&s_base_ls_entries, &s_base_ls_slept_us);
    s_samples = 0;
    s_late_max_ms = 0;
    s_late_over_1s = 0;
    if (label != NULL) {
        strlcpy(s_label, label, sizeof(s_label));
    }
    ESP_LOGI(TAG, "POWERLOG baseline: state=\"%s\" soc=%u%% mv=%u",
             s_label, b->percent, b->millivolts);
}

// The computed result for the window baseline..b. Returns false when the gauge
// has not moved a whole percent yet, in which case there is no rate to report.
static bool compute(const rr_battery_t *b, int64_t now_us,
                    double *elapsed_h, int *dropped, double *pct_per_h, double *ma)
{
    *elapsed_h = (double) (now_us - s_base_us) / 3600e6;
    *dropped = (int) s_base_pct - (int) b->percent;
    if (*dropped <= 0 || *elapsed_h <= 0.0) return false;
    *pct_per_h = (double) *dropped / *elapsed_h;
    *ma = *pct_per_h * RR_POWERLOG_BATTERY_MAH / 100.0;
    return true;
}

static void powerlog_task(void *arg)
{
    (void) arg;

    rr_battery_t b;
    if (rr_battery_read(&b) == ESP_OK) {
        s_prev_vbus = b.vbus_present ? 1 : 0;
        mark_now(&b, NULL);
        if (b.vbus_present) {
            ESP_LOGW(TAG, "POWERLOG started ON USB — this baseline is on the "
                          "charger and is NOT the measurement. Unplug: the "
                          "baseline re-arms itself on the USB->battery edge.");
        }
    }

    uint8_t last_saved_pct = 0xFF;

    while (s_running) {
        const int64_t before_delay = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS((uint32_t) s_interval_s * 1000));
        if (!s_running) break;

        // How much later than the nominal interval did we actually get the CPU?
        // Under-sleeping is impossible here, so anything positive is scheduling
        // delay: light-sleep exit cost, or a task that would not yield.
        {
            const int64_t actual_ms = (esp_timer_get_time() - before_delay) / 1000;
            const int64_t late_ms = actual_ms - (int64_t) s_interval_s * 1000;
            s_samples++;
            if (late_ms > 0) {
                if ((uint32_t) late_ms > s_late_max_ms) s_late_max_ms = (uint32_t) late_ms;
                if (late_ms > 1000) s_late_over_1s++;
            }
        }

        if (rr_battery_read(&b) != ESP_OK) {
            ESP_LOGW(TAG, "POWERLOG battery unreadable");
            continue;
        }

        const int64_t now_us = esp_timer_get_time();
        const int vbus = b.vbus_present ? 1 : 0;

        // ── USB -> battery: the run starts HERE ─────────────────────────────
        // Unplugging is the only signal available on a device with no console
        // and no buttons to spare, so it is the one that arms the baseline.
        // Without this the baseline stayed at whatever the charger left, and
        // every rate was computed across the transition into the state rather
        // than across the state.
        if (s_prev_vbus == 1 && vbus == 0) {
            mark_now(&b, "idle-asleep (auto: unplugged)");
            last_saved_pct = b.percent;
            plog_save(&b, 0, false);
            s_prev_vbus = vbus;
            ESP_LOGI(TAG, "POWERLOG measuring from here — leave it alone");
            continue;
        }

        // ── battery -> USB: the run ENDS here, and must hand back the number ─
        // This is deliberately BEFORE the ON-USB guard below. The window ran
        // from the baseline to this moment entirely on battery, so it is a
        // valid measurement and printing it is the whole point of the exercise.
        // The original code's guard ran first and threw the answer away.
        if (s_prev_vbus == 0 && vbus == 1) {
            double elapsed_h, pct_per_h, ma;
            int dropped;
            if (s_base_valid && compute(&b, now_us, &elapsed_h, &dropped, &pct_per_h, &ma)) {
                ESP_LOGW(TAG, "POWERLOG ── FINAL ── state=\"%s\" %u%%->%u%% over "
                              "%.2fh | %.2f %%/h ≈ %.1f mA | full charge ≈ %.1f h%s",
                         s_label, s_base_pct, b.percent, elapsed_h, pct_per_h, ma,
                         100.0 / pct_per_h,
                         s_charged_since_mark ? "  ⚠ USB SEEN MID-WINDOW — INVALID" : "");
            } else {
                ESP_LOGW(TAG, "POWERLOG ── FINAL ── state=\"%s\" %u%%->%u%% over "
                              "%.2fh — NO WHOLE PERCENT LOST, so there is no rate. "
                              "The run was too short, or the draw is lower than "
                              "the gauge can see in that time.",
                         s_label, s_base_pct, b.percent,
                         (double) (now_us - s_base_us) / 3600e6);
            }
            // Report the light-sleep side of the same window immediately too —
            // this is the moment a host is attached and reading.
            {
                uint32_t ent = 0;
                uint64_t slept = 0;
                rr_pm_stats(&ent, &slept);
                const int64_t win = now_us - s_base_us;
                const uint32_t d_ent = ent - s_base_ls_entries;
                const uint64_t d_slept = slept - s_base_ls_slept_us;
                if (rr_pm_light_sleep_enabled()) {
                    ESP_LOGW(TAG, "POWERLOG ── FINAL, LIGHT SLEEP ── entered %" PRIu32
                                  " times, slept %.1f s of %.1f s = %.1f%% residency%s",
                             d_ent, (double) d_slept / 1e6, (double) win / 1e6,
                             win > 0 ? 100.0 * (double) d_slept / (double) win : 0.0,
                             d_ent == 0 ? "  ⚠ NEVER ENTERED" : "");
                } else {
                    ESP_LOGW(TAG, "POWERLOG ── FINAL, LIGHT SLEEP ── not compiled in "
                                  "(no RR_LIGHT_SLEEP)");
                }
                // Print the lateness here too. It was originally only in the
                // PREVIOUS RUN path, so a run that finalised CLEANLY — i.e. the
                // good case, where the board did not reset on attach — reported
                // everything except the starvation answer, and recovering it
                // needed a gratuitous extra reset.
                ESP_LOGW(TAG, "POWERLOG ── FINAL, SCHEDULING ── %" PRIu32 " wake-ups, "
                              "worst lateness %" PRIu32 " ms, %" PRIu32 " over 1 s%s",
                         s_samples, s_late_max_ms, s_late_over_1s,
                         s_late_over_1s > 0
                             ? "  ⚠ A PERIODIC TASK WAS DELAYED >1 s — rr_sched could miss a fire"
                             : (s_samples > 0 ? "  ✅ no task starvation seen" : ""));
            }
            plog_save(&b, now_us - s_base_us, true);
            s_prev_vbus = vbus;
            continue;
        }
        s_prev_vbus = vbus;

        if (vbus) {
            // Charging or floating on USB: the gauge will not fall, so any rate
            // computed from here is meaningless. Say it every sample — a run
            // accidentally left plugged in is the likeliest way to waste an hour.
            s_charged_since_mark = true;
            ESP_LOGW(TAG, "POWERLOG t=%.2fh soc=%u%% mv=%u — ON USB, NOT MEASURING "
                          "(unplug; the baseline re-arms itself)",
                     (double) (now_us - s_base_us) / 3600e6, b.percent, b.millivolts);
            continue;
        }

        if (!s_base_valid) { mark_now(&b, NULL); continue; }

        double elapsed_h, pct_per_h, ma;
        int dropped;
        const bool have_rate = compute(&b, now_us, &elapsed_h, &dropped, &pct_per_h, &ma);

        // Persist EVERY sample, not just whole-percent changes.
        //
        // ⚠️ WHY THIS CHANGED. Replugging almost always resets the board through
        // the USB-JTAG bridge's DTR/RTS lines, and that happens within a second or
        // two — far sooner than the next 60 s sample. So the battery->USB finalise
        // usually never runs, and what gets read back is the last SAVE. Saving only
        // on percent changes meant the recovered window ended at the last percent
        // step, discarding up to a whole percent's worth of time (~24 min at 10 mA)
        // and understating the rate. Saving every sample bounds that truncation at
        // one sample interval.
        //
        // Cost: one NVS write a minute, ~180 across a 3-hour run. Immaterial next
        // to a 400 mAh cell, and NVS wear at that rate is irrelevant.
        (void) last_saved_pct;
        plog_save(&b, now_us - s_base_us, false);

        if (!have_rate) {
            // Normal early on: a 10 mA state takes ~24 min to move one percent.
            ESP_LOGI(TAG, "POWERLOG t=%.2fh soc=%u%% mv=%u state=\"%s\" — "
                          "no whole percent lost yet",
                     elapsed_h, b.percent, b.millivolts, s_label);
            continue;
        }

        ESP_LOGI(TAG, "POWERLOG t=%.2fh soc=%u%% mv=%u state=\"%s\" | "
                      "dropped %d%% -> %.2f %%/h ≈ %.1f mA | full charge ≈ %.1f h%s",
                 elapsed_h, b.percent, b.millivolts, s_label,
                 dropped, pct_per_h, ma, 100.0 / pct_per_h,
                 s_charged_since_mark ? "  ⚠ USB SEEN SINCE BASELINE — SAMPLE INVALID" : "");
    }

    ESP_LOGI(TAG, "POWERLOG stopped");
    vTaskDelete(NULL);
}

esp_err_t rr_powerlog_start(int interval_s)
{
    if (s_running) return ESP_OK;
    s_interval_s = interval_s > 0 ? interval_s : 60;

    // FIRST, before anything can overwrite it: a host is attached right now
    // (that is what flashing means), and this is the only moment the last run's
    // result is guaranteed to be readable.
    plog_report_previous();

    s_running = true;

    if (xTaskCreate(powerlog_task, "rr_powerlog", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "could not start the power-log task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "POWERLOG started — sampling every %d s (%d mAh cell, 1%% = %.1f mAh)",
             s_interval_s, RR_POWERLOG_BATTERY_MAH, RR_POWERLOG_BATTERY_MAH / 100.0);
    return ESP_OK;
}

void rr_powerlog_stop(void) { s_running = false; }

void rr_powerlog_mark(const char *state_label)
{
    rr_battery_t b;
    if (rr_battery_read(&b) == ESP_OK) mark_now(&b, state_label);
}

bool rr_powerlog_is_running(void) { return s_running; }
