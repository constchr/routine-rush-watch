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

// ── Phase 3: pairing state + the BLE nonce gate ──────────────────────────────
//
// Phase 2 confirmed the bond is Just Works: ANY central can pair unprompted,
// so a bond proves encryption, not authentication. The nonce is the only
// secret that proves the peer physically saw the QR, so it — not the bond —
// is what gates privileged operations.

/**
 * Record the nonce currently displayed in the QR. Called once per pairing
 * screen; the value is deliberately NOT persisted.
 */
void rr_identity_set_active_nonce(const char *nonce);

/**
 * Constant-time-ish comparison of a presented nonce against the active one.
 * False if no nonce is active (i.e. the watch is not showing a QR).
 */
bool rr_identity_check_nonce(const char *presented);

/** True once a phone has completed the nonce handshake. Persisted in NVS. */
bool rr_identity_is_paired(void);

/**
 * Mark the watch paired (NVS key "paired"). Survives reboot so the QR does
 * not reappear on a watch that is already in service.
 *
 * FUTURE (not built): a re-pair path. Options are a long-press on the PWR
 * button, or a parent-initiated unpair over BLE that clears this flag and the
 * bond store. Until one exists, `idf.py erase-flash` (or erasing the NVS
 * region) is the only reset — which also regenerates the device_id and so
 * requires a fresh claim server-side.
 */
esp_err_t rr_identity_set_paired(bool paired);

// ── Phase 3B: reset + the paired-peer trust anchor ───────────────────────────
//
// TRUST MODEL for destructive commands (factory reset over BLE):
//   1. the link must be ENCRYPTED (characteristic is WRITE_ENC), and
//   2. the peer's BLE *identity* address must match one recorded when this
//      watch was paired.
//
// (1) alone is NOT sufficient: pairing is Just Works, so any central can bond
// unprompted — proven on hardware in Phase 2 when a Mac bonded with no prompt.
// A bond-only gate would let any passer-by wipe a child's watch. (2) is what
// makes it "the phone that actually paired me".
//
// The identity address (not the connection address) is used deliberately:
// iOS advertises a rotating resolvable private address, but because we exchange
// ID keys (BLE_SM_PAIR_KEY_DIST_ID) NimBLE resolves it to a stable identity.

/** Record the peer that completed the nonce handshake. Persisted in NVS. */
esp_err_t rr_identity_set_paired_peer(const uint8_t addr[6], uint8_t addr_type);

/** True if this peer is the one that paired us. False if none is recorded. */
bool rr_identity_is_paired_peer(const uint8_t addr[6], uint8_t addr_type);

/**
 * Factory reset: erase the whole rr_watch NVS namespace (device_id, paired
 * flag, paired peer). The caller is responsible for clearing the routine cache
 * and the BLE bond store, then rebooting.
 *
 * A reset watch is genuinely a NEW DEVICE: on the next boot it mints a fresh
 * device_id, which in turn derives a fresh BLE address, so previously-bonded
 * phones see a new peripheral rather than "same identity, different keys"
 * (the CBError 14 trap found in Phase 3).
 */
esp_err_t rr_identity_factory_reset(void);
