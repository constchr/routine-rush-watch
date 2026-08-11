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

/** Start a routine from the cache at step 0. */
esp_err_t rr_routine_start(int routine_idx);

/** True while a routine is on screen. */
bool rr_routine_is_active(void);
