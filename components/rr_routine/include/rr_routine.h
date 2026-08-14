#pragma once
// rr_routine — routine runtime. Phase 4b.
//
// The single-step-focus loop from §8: one step on screen, a 1 Hz countdown,
// and Done/Skip to advance. Runs entirely off the littlefs cache — no network,
// no phone.
//
// NOT YET BUILT (deliberately, later phases):
//   - completion RECORDS and the durable run queue (Phase 5)
//   - XP / streak (Phase 5)
//   - Back / resume-a-previous-step (the kids app has it)
//   - scheduler firing a routine by time (Phase 7), audio (Phase 8)
//   - locale: child.language is not cached yet, so the complete screen is not
//     yet locale-driven
//
// Per-step outcomes are tracked in RAM only and discarded when the routine
// ends. That is the honest scope: a durable record needs the queue, and a
// half-durable one would be worse than none.

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    RR_STEP_PENDING = 0,
    RR_STEP_DONE,
    RR_STEP_SKIPPED,
} rr_step_outcome_t;

/**
 * Start a routine from the cache at step 0.
 *
 * MUST be called from the LVGL task — it builds screens and creates an
 * lv_timer. Anything on another task (the NimBLE host, a scheduler tick) wants
 * rr_routine_request_start() instead, which defers to the LVGL task for you.
 */
esp_err_t rr_routine_start(int routine_idx);

/** True while a routine is on screen. */
bool rr_routine_is_active(void);

// ── Remote / scheduled start ────────────────────────────────────────────────
//
// THE ONE ENTRY POINT for "begin routine X now" when the caller is not already
// the LVGL task. Today that caller is the RR_CONTROL start_routine command
// from the paired phone; Phase 7's on-watch scheduler is the next one, and it
// calls exactly this — the busy rule, the cache lookup, the wake and the task
// hand-off are all here precisely so a second start path never gets written.

typedef enum {
    /**
     * Accepted — the READY screen will be up within a frame or two.
     *
     * ⚠️ OK NO LONGER MEANS "RUNNING". Since the READY screen went in, every
     * start (scheduler or phone) is an OFFER: the routine begins when the
     * child taps START and not before. The RR_CONTROL wire protocol and the
     * ATT codes are FROZEN and unchanged — this is a semantic change only, and
     * the parent app's "started on the watch" wording is handled app-side.
     */
    RR_START_OK = 0,
    /** A routine is ALREADY RUNNING and was deliberately left alone. */
    RR_START_BUSY,
    /** No routine with that assignment_id is cached — the phone must push first. */
    RR_START_UNKNOWN_ROUTINE,
    /** The cache is missing or unreadable. */
    RR_START_ERROR,
} rr_start_result_t;

/**
 * Offer `assignment_id` to the child, right now.
 *
 * Shows the READY screen (§7). It does NOT run the routine — rr_routine_start()
 * fires when START is tapped, and the countdown begins there. That is the whole
 * point of the choke point: a remote start used to drop a child who was not
 * looking at their wrist straight into a running step 1.
 *
 * SAFE FROM ANY TASK. The decision (busy? cached?) is made synchronously so
 * the caller gets a real answer to relay — the BLE handler turns it straight
 * into an ATT status on the write response — while the LVGL work is deferred
 * to the display task, where touching widgets and timers is legal.
 *
 * Refusing while a routine is active is a PRODUCT rule, not a technical one: a
 * child three steps into getting dressed must not be thrown back to step 1
 * because a parent tapped a button. The watch owns that rule because only the
 * watch knows what is on its screen.
 */
rr_start_result_t rr_routine_request_start(const char *assignment_id);

// ── The pending READY offer ─────────────────────────────────────────────────
//
// RAM ONLY, and that is a decision rather than an omission: a watch that
// rebooted at 3 a.m. and offered this morning's routine when the child put it
// on would be worse than one that quietly lost the offer. Nothing here is
// persisted, so a reboot ends the offer.

/** True while a READY offer is up and unanswered. */
bool rr_routine_ready_pending(void);

/**
 * Re-paint the pending offer. No-op when nothing is pending.
 *
 * For the case where READY was raised, the panel slept, and something else
 * (a ROUTINE_PUSH's paired screen, an aborted reset hold) painted over it.
 * MUST be called from the LVGL task. Cheap to call when READY is already the
 * screen on display — it checks before rebuilding, because its one caller is
 * a predicate polled twice a second.
 */
void rr_routine_show_ready(void);

/**
 * Withdraw the offer for `assignment_id` and put the watch face back.
 *
 * Ignored unless that id is the one currently offered, so a scheduler giving
 * up on its own occurrence cannot cancel an offer the phone has since made.
 */
void rr_routine_cancel_ready(const char *assignment_id);

/**
 * Register a callback for "the child answered a READY offer".
 *
 * `started` is true for START and false for NOT NOW. rr_sched needs both to
 * close out its pending occurrence — START means the fire did its job, NOT NOW
 * means dismiss it for today — and without this it would be guessing from the
 * outside whether an offer it raised is still up.
 *
 * Runs on the LVGL task, from the button handler. Do not block in it.
 */
void rr_routine_set_answer_hook(void (*fn)(const char *assignment_id, bool started));

/**
 * Register the "wake the screen" action, called just before a deferred start
 * paints its first step.
 *
 * A hook rather than a direct call because rr_power (which owns wake) already
 * depends on rr_ble, and rr_ble depends on this component — calling rr_idle
 * from here would close that loop into a build cycle. main.c owns both sides
 * and wires them together, the same shape rr_idle's own gate/suspend hooks use.
 */
void rr_routine_set_wake_hook(void (*fn)(void));

/**
 * Register a callback for "a routine just ended".
 *
 * Phase 7's scheduler uses it to service a fire it had to DEFER: a scheduled
 * routine that arrived while another was running waits for the watch to go
 * idle, and the moment it ends is exactly that. Without the hook the deferred
 * fire waits out a polling interval instead, which on a 30-minute grace window
 * is a visible delay for no reason.
 *
 * A hook and not a direct call, for the usual reason: rr_sched depends on this
 * component, so calling into it from here would be a cycle. Runs on the LVGL
 * task, at the end of the routine — do not block in it.
 */
void rr_routine_set_finish_hook(void (*fn)(void));
