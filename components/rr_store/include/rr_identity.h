#pragma once
// rr_identity — persistent device identity + pairing nonce.
//
// Lives in rr_store because it is the persistence component; it uses NVS
// (not LittleFS) because the device_id must survive a filesystem format and is
// tiny. See rr_store.h for the cache/queue split.
//
// NVS namespace "rr_watch", key "device_id" — a canonical UUID string, minted
// once on first boot and never regenerated. It is the primary key of both
// `watch_pairing` and `device` server-side (spec §4.1), so regenerating it
// would orphan the pairing and silently create a second device row.
//
// The NONCE is different: it is ephemeral, regenerated every time the pairing
// screen opens, and never persisted. It proves possession of the QR for one
// pairing attempt.

#include <stdbool.h>
#include "esp_err.h"

#define RR_DEVICE_ID_LEN 37   // 36-char canonical UUID + NUL
#define RR_NONCE_LEN     17   // 16 hex chars + NUL

/**
 * Load the device_id from NVS, minting and persisting one on first boot.
 * Idempotent — safe to call every boot.
 */
esp_err_t rr_identity_init(void);

/** Canonical UUID string, e.g. "3f2b...-...". Valid after rr_identity_init(). */
const char *rr_identity_device_id(void);

/** True if this boot was the one that minted the device_id. */
bool rr_identity_was_first_boot(void);

/**
 * Generate a fresh pairing nonce into `out` (>= RR_NONCE_LEN bytes).
 * Call once per pairing attempt; do not persist it.
 */
void rr_identity_new_nonce(char *out, size_t out_len);
