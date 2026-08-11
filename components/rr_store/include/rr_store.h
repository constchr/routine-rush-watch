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
    char assignment_id[40];   /**< routine.id — the run's assignment_id */
    char step_id[40];         /**< step.id — reported per-step to the relay */
    int  base_xp;             /**< feeds the provisional-XP formula */
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


// ── Phase 5: the durable completion queue (§5, §6.3) ─────────────────────────
//
// /lfs/queue/runs.log     append-only NDJSON, one completed run per line
// /lfs/queue/cursor.json  { "offset": <bytes acked> }
//
// This is the ONE thing on the watch that is not disposable. The routine cache
// can be lost and refetched; a lost queue entry is a child's completed routine
// that silently never counted. Every append is fsync'd before it is reported
// as queued.

/** Append one run record (a single JSON line). Durable on return. */
esp_err_t rr_queue_append(const char *json, size_t len);

/** Number of runs queued and not yet acked. */
int rr_queue_count(void);

/**
 * Copy every unacked record into `out` as NDJSON. Returns bytes written, or
 * -1 on error. Pass NULL to query the required size.
 */
int rr_queue_read_unacked(char *out, size_t cap);

/**
 * Ack the record at the head of the queue if its local_id matches, advancing
 * the cursor past it. A repeat ack for an already-advanced record is a
 * deliberate no-op, which is what makes re-flush after a BLE drop safe.
 */
esp_err_t rr_queue_ack(const char *local_id);

/** Oldest unacked record's completed_at epoch, or 0 if the queue is empty. */
uint32_t rr_queue_oldest_ts(void);

// ── Phase 6: the next scheduled routine, for the idle face (§9B.1) ───────────

typedef struct {
    bool found;
    char routine_name[64];
    char routine_emoji[16];
    int  hour;
    int  minute;
    bool today;        /**< false => the soonest is tomorrow or later */
} rr_next_routine_t;

/**
 * Soonest upcoming scheduled routine at or after (now_hour:now_min) on
 * `iso_weekday` (1=Mon..7=Sun). If nothing remains today, wraps to the
 * earliest schedule on any later day and sets today=false.
 */
esp_err_t rr_store_next_routine(int iso_weekday, int now_hour, int now_min,
                                rr_next_routine_t *out);
