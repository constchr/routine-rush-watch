#pragma once
// rr_ble — RR_SYNC GATT server. Phase 1. THE FIRST THING TO BUILD.
//
// The watch is the BLE *peripheral*; the parent phone is the central (§6B.1).
// This is the sole uplink — wifi is never initialised — so everything from
// pairing to OTA rides on this module.
//
// The wire format is FROZEN (spec §6B.3) and shared with the phone. Do not
// hand-write UUIDs or struct offsets here: include the generated header, which
// is produced from the app repo's watchProtocol.ts by tools/gen-ble-contract.mjs.
//
//     #include "ble_contract.h"
//
// Intended interface:
//
//   esp_err_t rr_ble_init(void);         // NimBLE up, register RR_SYNC service
//   esp_err_t rr_ble_start_advertising(void);
//   esp_err_t rr_ble_stop_advertising(void);
//   bool      rr_ble_is_connected(void);
//
//   // QUEUE_STATUS is read|notify — push a fresh 9-byte status block when the
//   // queue depth changes so the phone doesn't have to poll.
//   esp_err_t rr_ble_notify_queue_status(void);
//
//   // Callbacks the transport raises into rr_sync (never the reverse):
//   typedef void (*rr_ble_time_sync_cb_t)(uint32_t epoch);
//   typedef void (*rr_ble_routine_push_cb_t)(const uint8_t *json, size_t len);
//   typedef void (*rr_ble_run_ack_cb_t)(const rr_run_ack_t *ack);
//   void rr_ble_set_callbacks(...);
//
// Phase 1 exit criteria: phone connects, TIME_SYNC write lands, and the RTC is
// set from it. Nothing else needs to work yet.

// ── Phase 1 surface (implemented) ────────────────────────────────────────────

#include <stdbool.h>
#include "esp_err.h"

/**
 * Bring up NimBLE as a peripheral, register RR_SYNC, and start advertising.
 *
 * Requires rr_rtc_init() to have run first — a TIME_SYNC write can arrive as
 * soon as advertising starts, and its handler writes the RTC.
 */
esp_err_t rr_ble_init(void);

/** True while a central is connected. */
bool rr_ble_is_connected(void);
