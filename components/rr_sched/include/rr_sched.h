#pragma once
// rr_sched — the on-watch scheduler (§7). Phase 7.
//
// The watch's headline capability: a routine fires itself, on the wrist, at
// its scheduled time. No phone, no app in the foreground, no OS permission —
// iOS and Android cannot self-launch an app on a timer, and this is the whole
// reason the watch exists alongside the tablet.
//
// ── HOW THE WAKE ACTUALLY WORKS (read this before "fixing" it) ──────────────
//
// The design calls for the PCF85063's alarm interrupt to wake the CPU so
// nothing polls the clock. THAT PIN IS NOT CONNECTED ON THIS BOARD. Verified
// two ways, because assuming a vendor interrupt works is exactly how the
// QMI8658 wake-on-motion bug happened:
//
//   1. Schematic V1.0: net RTC_INT has two endpoints, U6 pin 4 and pad P4.
//      Every other interrupt on the board (QMI_INT1→GPIO16, QMI_INT2→GPIO17,
//      TP_INT→GPIO15) has a third endpoint at the MCU. This one does not.
//   2. On hardware: rr_rtc_alarm_selftest() arms a real alarm and watches it
//      fire (AF sets on the exact second — the comparator is fine) while
//      sampling GPIO14, the one pin the board leaves unrouted. It never moves.
//
// So the wake source here is the ESP32-C6's own LP timer: the scheduler task
// blocks on a notification with a computed timeout and the CPU is free to
// idle for the whole interval. That is one wake per fire, not a poll loop —
// the property the interrupt was wanted for is preserved; only the peripheral
// providing it changed.
//
// The PCF85063 is still the timebase, and it is the right one: it is battery-
// backed, it survives reboots, and it does not drift the way an MCU timer
// does. Every wake re-reads it and recomputes the interval from wall-clock
// truth rather than trusting elapsed ticks (RESYNC_CAP_S below).
//
// If a future board rev routes /INT — or the P4 pad is bridged to a free
// GPIO — rr_rtc_alarm_at() is already implemented and verified; this module
// would arm it and add an ISR. Nothing else here would change.

#include <stdbool.h>
#include "esp_err.h"

// ── Tunables (§7) ───────────────────────────────────────────────────────────

/**
 * How late a scheduled routine may still start.
 *
 * The point of the window is that a routine is about a MOMENT, not a task
 * list. If breakfast overran and bedtime's 21:15 alarm finally gets its turn
 * at 21:45, firing it is worse than dropping it: the child is already in bed
 * and the watch is nagging about something that has passed. Thirty minutes is
 * long enough to survive one overrunning routine and short enough that a fire
 * still means what it said.
 */
#define RR_SCHED_GRACE_WINDOW_S   (30 * 60)

// ── SNOOZE IS GONE. What replaced it, and why ───────────────────────────────
//
// RR_SCHED_SNOOZE_S (5 min) and RR_SCHED_ALARM_TIMEOUT_S (60 s) were REMOVED,
// not repurposed. They belonged to an alarm screen whose quiet button meant
// "ring at me again in five minutes"; the READY screen's quiet button means
// "not now", which DISMISSES this occurrence for today. Keeping the constants
// under new meanings would have left the word "snooze" in a module that no
// longer snoozes.
//
// The two behaviours they encoded are both still here, in a different shape:
//
//   • "the alarm should not give up after one beep" was RR_SCHED_ALARM_TIMEOUT_S
//     auto-snoozing an unanswered alarm. It is now REPEAT/MAX_RINGS below: the
//     tone replays while READY is unanswered, capped, and then the watch goes
//     quiet with THE OFFER STILL UP. An offer that survives in silence is
//     better than one that re-rings on a five-minute cycle at a child who has
//     already seen it.
//   • "an unanswered fire eventually expires" is still RR_SCHED_GRACE_WINDOW_S
//     above, measured from the scheduled moment exactly as before.

/** How long between repeats of the alarm tone while READY is unanswered. */
#define RR_SCHED_ALARM_REPEAT_S   20

/**
 * How many times the alarm may sound for one fire, first ring included.
 *
 * 5 × 20 s is about 80 seconds of noise. Past that the child either cannot
 * hear it or is deliberately ignoring it, and a watch that keeps beeping is
 * one a parent turns off. Ringing stops; the offer does not.
 */
#define RR_SCHED_ALARM_MAX_RINGS  5

/** How often a deferred fire re-checks whether the watch went idle. */
#define RR_SCHED_BUSY_RETRY_S     20

/**
 * Longest single sleep before re-reading the RTC.
 *
 * NOT a poll of the schedule — it is drift control. The task's timeout is an
 * MCU timer; the schedule is wall-clock. Waking every half hour to recompute
 * the remaining interval from the battery-backed RTC bounds the error to what
 * the MCU timer accumulates in 30 minutes instead of in 8 hours. A fire that
 * is 40 seconds late is a bug report; the cost of preventing it is ~48 wakes a
 * day of a single I2C read each.
 */
#define RR_SCHED_RESYNC_CAP_S     (30 * 60)

/**
 * How far back a fresh boot looks for occurrences it slept through.
 *
 * Only matters when the watch was off for a long time: it bounds how many
 * already-past fires get walked and logged before the search reaches the
 * future. Anything older than this is history, not a missed alarm.
 */
#define RR_SCHED_MAX_LOOKBACK_S   (25 * 3600)

// ── API ─────────────────────────────────────────────────────────────────────

/** Start the scheduler task. Call after rr_store/rr_rtc/rr_audio are up. */
esp_err_t rr_sched_init(void);

/**
 * Recompute the next fire now, instead of at the next scheduled wake.
 *
 * Call whenever an INPUT to the schedule changed: new routines pushed, the
 * clock set, the UTC offset changed, or a routine just finished (a deferred
 * fire may be waiting for exactly that). Safe from any task, including the
 * NimBLE host. `reason` is logged — when a fire happens at an unexpected
 * moment, the last re-arm and its cause is the first thing worth knowing.
 */
void rr_sched_rearm(const char *reason);

// rr_sched_alarm_is_showing() was REMOVED along with the alarm screen. It fed
// main.c's idle-sleep suspension so an unanswered alarm could not blank
// itself. The READY screen does not need that: it is allowed to sleep on
// rr_idle's normal timeout, because the offer is not lost when it does — the
// next wake re-shows it (rr_routine_ready_pending, wired through main.c's face
// gate). Holding the panel lit was only ever a way of not losing the alarm.

/**
 * Register how to wake the screen for a fire.
 *
 * A hook rather than a direct call to rr_idle, for the same reason
 * rr_routine_set_wake_hook() is one: rr_power depends on rr_ble, which
 * depends on this component. main.c owns both ends and wires them.
 */
void rr_sched_set_wake_hook(void (*fn)(void));
