#pragma once
// rr_steps — step detection + daily count. Phase 9.
//
// rr_imu owns the sensor; this module owns the pedometer ALGORITHM and the day.
//
// Four jobs:
//   1. detect steps from accelerometer samples (the detector is in rr_steps.c)
//   2. accumulate them into a DAILY total
//   3. reset that total at LOCAL midnight, not UTC midnight and not uptime
//   4. persist it so a reboot mid-day loses minutes, not the day
//
// ── ⚠️ WHY THIS IS SOFTWARE, AND WHAT IT COSTS ──────────────────────────────
//
// The plan was the QMI8658's on-chip step engine, which would have counted
// inside the sensor with the CPU asleep. IT DOES NOT WORK ON THIS PART —
// configured correctly, verified applied by register read-back, and still zero
// after 28 seconds of ~2 g shaking with the loosest thresholds it accepts. The
// evidence is written up in rr_imu.h; §10.1 explicitly allows this fallback.
//
// The cost is real and it is not hidden: this samples the accelerometer from
// the main CPU RR_STEPS_SAMPLE_HZ times a second, forever, including while the
// watch is "asleep". Phase 6's battery case rests on the CPU idling between
// wrist raises, and a 25 Hz sampler is exactly the polling loop that case was
// designed to avoid. Step counting on this board therefore has a standing power
// cost that the on-chip engine would have avoided entirely.
//
// PHASE 10, in preference order:
//   • Use the QMI8658 FIFO. It buffers samples in the sensor; the CPU drains a
//     few seconds' worth in one burst and runs the same detector over them.
//     Same accuracy, ~1/100th of the wakes. This is the real fix and the FIFO
//     is a far more basic feature than the step engine, so more likely to work.
//   • Gate sampling on movement: wake-on-motion already knows when the wrist is
//     still, and a watch on a bedside table needs no sampling at all.
//   • Drop RR_STEPS_SAMPLE_HZ. 25 Hz is comfortable for a 4 Hz gait; 15 Hz is
//     probably still enough and is 40% fewer wakes.
//
// ── Why LOCAL midnight is a trap worth naming ───────────────────────────────
//
// The RTC holds UTC (rr_rtc.h) and the offset is applied on the way out. A day
// boundary computed from UTC would roll the count over at 03:00 for a watch in
// Cyprus — a child's steps from before breakfast would land on the previous
// day, and the face would reset itself in the middle of the night for no
// visible reason. This is the same local-vs-UTC bug that made the Phase 6
// next-routine hint wrong and would have made a Phase 7 fire land three hours
// out, so it is the same fix: everything here goes through rr_rtc_get_local().
//
// ── NOT in scope (deliberately) ─────────────────────────────────────────────
//
// No backend sync. Spec §10.1 leaves that as a v1.1 item needing a
// `child_daily_steps` table, and the count is useful standalone on the wrist.
// Nothing here touches BLE or the completion queue.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * One line describing the detector's INPUT, for the heartbeat:
 * "accel 1.00g sprd 0.004 n=750".
 *
 * ⚠️ The SPREAD is the point, not the magnitude. A live accelerometer at rest
 * still jitters by a few mg; a frozen one reports an identical value forever, so
 * a spread of zero is flagged FROZEN. Without this, "steps unchanged" is
 * ambiguous between a dead sensor and a stationary watch — which is precisely
 * the ambiguity that hid a broken step counter from Phase 9 to Phase 10.
 *
 * Resets its window on each call, so it describes the interval since last asked.
 */
void rr_steps_describe_input(char *buf, size_t len);

/**
 * Accelerometer sample rate for the detector, in Hz.
 *
 * A child's gait tops out near 4 steps/s, so its peaks live below ~4 Hz and
 * 25 Hz gives ~6 samples per step — enough to place a peak without the
 * detector becoming rate-sensitive. Raising it costs battery for nothing;
 * dropping it much below ~15 Hz starts losing peaks.
 *
 * The detector's thresholds are in g and its gates in milliseconds, so they do
 * NOT need re-deriving if this changes — only the EMA coefficients do.
 */
#define RR_STEPS_SAMPLE_HZ 25

/** Seconds between day-rollover / persistence checks (not the sample rate). */
#define RR_STEPS_POLL_S 60

/**
 * Persistence thresholds — "often enough that a crash loses minutes, not
 * hours" without writing flash once per step.
 *
 * A write happens when EITHER this many steps have accumulated since the last
 * one, or this long has passed with any change at all. Worst-case loss is one
 * threshold's worth: 24 steps, or 2 minutes of walking.
 *
 * These started at 50 / 5 min, which measured badly: a reboot after a 91-step
 * test resumed at 50, because the write had fired exactly on the 50-step
 * boundary and the next 41 steps all landed inside the 5-minute window. Correct
 * per the stated worst case, but a poor showing at small counts, so both were
 * halved.
 *
 * The cost is still nowhere near the flash: a 10,000-step day is ~400 writes of
 * two small NVS entries. NVS appends entries within a page and wear-levels
 * across the partition, so that is a handful of page erases a day spread over
 * the whole nvs area — against an endurance budget in the tens of thousands of
 * cycles per sector. Three orders of magnitude of headroom, and the partition is
 * shared with pairing keys, which is the reason to keep checking that arithmetic
 * rather than lowering these further.
 */
#define RR_STEPS_PERSIST_DELTA   25
#define RR_STEPS_PERSIST_EVERY_S 120

/**
 * Start the detector and the daily counter.
 *
 * Call AFTER rr_imu_init() and rr_rtc_init(): it holds the accelerometer's
 * range and rate through rr_imu (it does not configure the QMI8658 itself) and
 * needs the clock to know which day it is resuming into.
 *
 * On failure rr_steps_valid() stays false and the face keeps its placeholder
 * rather than showing a confident zero.
 */
esp_err_t rr_steps_init(void);

/** Today's step count, local-midnight to now. */
uint32_t rr_steps_today(void);

/**
 * True when the count means something: the detector is running and the day it
 * is counting into was established from a clock that had been set.
 *
 * The face uses this for w.steps_valid, so a watch with a dead engine or an
 * unset clock shows "—" instead of a zero that looks like "you have not moved".
 */
bool rr_steps_valid(void);

/**
 * Settle the day boundary and flush if due.
 *
 * The count itself is always current — the sampling task credits steps as they
 * happen — so this is not a refresh of the number. The watch face calls it on
 * render so that a face drawn just after local midnight shows 0 rather than
 * yesterday's total for up to a minute. Safe from any task.
 */
void rr_steps_refresh(void);

/** Force a persist (before a deliberate reboot, for instance). */
void rr_steps_flush(void);

// ── Daily step target ───────────────────────────────────────────────────────
//
// One sound, once, when today's count first crosses the target. The parent owns
// the number: it arrives over BLE as RR_CONTROL `set_step_target` and persists in
// NVS, so a watch out of phone range keeps it.
//
// ⚠️ ONCE PER DAY IS THE WHOLE DESIGN, and the guard is on FLASH, not in RAM.
// The count keeps climbing after the target, and a naive `count >= target` test
// would fire the tone on every credited step for the rest of the day — a slot
// machine on a child's wrist. A RAM-only flag would also re-fire after a reboot,
// which on this watch is a routine event (button wake, a flash, a crash), so the
// day the tone fired is stored beside the count.
//
// The marker is a DAY NUMBER rather than a boolean, so local midnight clears it
// implicitly: a new day is a different number, and there is no reset to forget.

/** 0 disables the tone. Applied and persisted; the value is range-checked. */
esp_err_t rr_steps_set_target(uint32_t steps);

/** The current target; 0 when disabled. */
uint32_t rr_steps_target(void);

/** True if the target has already been reached (and announced) today. */
bool rr_steps_target_reached_today(void);
