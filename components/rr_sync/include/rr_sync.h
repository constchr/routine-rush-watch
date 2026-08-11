#pragma once
// rr_sync — sync engine. Phase 5.
//
// Drives the state machine in spec §6.1 and owns reconciliation. The risky
// relay logic lives in the PARENT APP (track 12.0 step C/E) and is proven
// against synthetic data before this module exists — so this is "wire the
// watch into a relay that already works", not a distributed-systems problem
// to solve here.
//
// Intended interface:
//
//   esp_err_t rr_sync_init(void);
//   void      rr_sync_on_connected(void);      // phone arrived → drain
//   void      rr_sync_on_routines(const uint8_t *json, size_t len);
//   void      rr_sync_on_run_ack(const rr_run_ack_t *ack);
//   rr_sync_state_t rr_sync_state(void);
//
// Provisional vs authoritative (§6.4): the watch shows provisional XP/streak
// immediately for responsiveness, then reconciles to the server's numbers on
// ack. Expect the two to disagree; the server always wins.
