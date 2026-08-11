#pragma once
// rr_store — LittleFS cache + durable completion queue. Phase 3/5.
//
// Two responsibilities, deliberately separated:
//   1. CACHE  — routines/steps/schedules mirrored from the server as JSON.
//               Disposable: if it's lost, the next sync refills it.
//   2. QUEUE  — completed runs awaiting relay. NOT disposable. A lost queue
//               entry is a child's completed routine that silently never
//               counted. Append-only log, fsync'd, survives power loss.
//
// Parse-then-free: build structs from the JSON and release the parsed tree
// immediately (§2.1 — RAM is the ceiling, ~274 KiB total).
//
// Intended interface:
//
//   esp_err_t rr_store_init(void);        // mount littlefs partition
//   esp_err_t rr_store_put_routines(const uint8_t *json, size_t len);
//   esp_err_t rr_store_load_routines(rr_routine_set_t *out);
//
//   esp_err_t rr_store_queue_push(const rr_run_record_t *run);   // durable
//   size_t    rr_store_queue_depth(void);
//   esp_err_t rr_store_queue_peek(size_t idx, rr_run_record_t *out);
//   esp_err_t rr_store_queue_ack(const char *client_local_id);   // drop one
//
// client_local_id is the idempotency key the backend dedupes on — generate it
// on the watch at completion time and never regenerate it on retry.

// ── Phase 3 surface (implemented) ────────────────────────────────────────────

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/** Mount the littlefs partition (formats it on first boot). Logs free space. */
esp_err_t rr_store_init(void);

bool rr_store_is_mounted(void);

/**
 * Validate and persist the denormalized routine set (spec §5).
 * Rejects anything that is not a JSON array — a bad cache is worse than none,
 * because the runtime would show a child the wrong steps.
 */
esp_err_t rr_store_put_routines(const char *json, size_t len);

/** ESP_OK if a non-empty routines cache exists on flash. */
esp_err_t rr_store_has_routines(void);

/**
 * Read the cache back off flash and log its contents (names, step counts,
 * schedules). Proves the round trip — "received N bytes" proves nothing.
 */
esp_err_t rr_store_log_routines(void);

/** Delete the cached routines (part of a factory reset). */
esp_err_t rr_store_clear_routines(void);

// ── Phase 4: reading one step out of the cache ───────────────────────────────

/**
 * A single step, flattened for rendering. Strings are COPIED into the struct
 * so the caller never holds a pointer into a cJSON tree that has been freed —
 * the parse-then-free rule from §2.1 (RAM is the ceiling) means the tree is
 * released as soon as the fields are extracted.
 */
typedef struct {
    char routine_name[64];
    char routine_emoji[16];
    char label[64];
    char emoji[16];
    int  time_limit_s;
    int  position;     /**< 0-based index of this step */
    int  step_count;   /**< total steps in the routine */
    int  routine_count;
} rr_step_view_t;

/** Read one step out of the cached routine set. ESP_ERR_NOT_FOUND if absent. */
esp_err_t rr_store_get_step(int routine_idx, int step_idx, rr_step_view_t *out);
