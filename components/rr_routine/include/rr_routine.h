#pragma once
// rr_routine — routine runtime. Phase 4.
//
// The single-step-focus loop shared with the kids tablet app: one step on
// screen at a time — emoji + name + countdown ring + Done/Skip.
//
// Runs entirely offline from cache. A child with only the watch and no phone
// nearby must be able to complete a full routine; completions queue durably
// via rr_store and relay whenever a phone next appears.
//
// Intended interface:
//
//   esp_err_t rr_routine_start(uint32_t assignment_id);
//   void      rr_routine_step_done(void);
//   void      rr_routine_step_skip(void);
//   void      rr_routine_abort(void);
//   bool      rr_routine_is_active(void);
//
// On completion: build an rr_run_record_t (with a fresh client_local_id) and
// hand it to rr_store_queue_push() BEFORE showing the celebration — the record
// must be durable before the child sees success.
