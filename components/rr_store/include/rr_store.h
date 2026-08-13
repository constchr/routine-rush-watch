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
#include <stdint.h>   // uint8_t/uint32_t below; do not rely on a transitive include
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

/** Delete the cached routines. NOT a factory reset — see below. */
esp_err_t rr_store_clear_routines(void);

/**
 * Wipe EVERYTHING on littlefs that constitutes "this watch belongs to a
 * child": routines, the child record, and the completion queue.
 *
 * This — not rr_store_clear_routines() — is the store half of a factory
 * reset. The identity half (device_id, paired flag, peer anchor) is
 * rr_identity_factory_reset(); the two are always called together from
 * rr_ble_factory_reset().
 */
esp_err_t rr_store_factory_reset(void);

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

/**
 * Resolve an assignment_id to its index in the cached routine set.
 *
 * The cache is an ordered array and everything downstream (rr_store_get_step,
 * rr_routine_start) addresses routines BY INDEX, but the phone only ever knows
 * them by id — an index is a property of one particular push and would mean
 * something different after the next one. This is the seam between the two.
 *
 * ESP_ERR_NOT_FOUND means exactly "this watch has not been sent that routine",
 * which is a real answer worth relaying, not a failure.
 */
esp_err_t rr_store_find_routine(const char *assignment_id, int *out_idx);


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
 *
 * ⚠️ NOT the BLE path. This needs the whole unacked region to fit in `cap`, and
 * a real backlog does not fit anywhere sensible. QUEUE_PULL uses the paged
 * framed-stream API below. Kept for tooling and tests.
 */
int rr_queue_read_unacked(char *out, size_t cap);

// ── The FRAMED stream (contract v3 paging) ───────────────────────────────────
//
// The frame prefix width is contract, and the contract's generated copy is
// RR_QUEUE_PULL_LEN_PREFIX_BYTES in ble_contract.h. It is restated here rather
// than included because ble_contract.h lives in rr_ble, and rr_ble already
// depends on rr_store — including it back would be a dependency cycle.
//
// It cannot drift silently: rr_ble.c sees BOTH headers and carries a
// _Static_assert that the two agree, so a change to the contract that is not
// mirrored here fails the build rather than corrupting the wire.
#define RR_QUEUE_FRAME_PREFIX_BYTES 2
//
// The wire format is not the storage format. On flash a record is `json\n`; on
// the wire it is `[u16 len][json]`. So record i occupies (len_i + 1) bytes on
// flash and (len_i + 2) bytes on the wire, and the two coordinate spaces do not
// line up. These functions work exclusively in WIRE offsets, measured from the
// head of the UNACKED region (i.e. offset 0 is the first byte of the first
// frame the phone still needs).
//
// They exist because QUEUE_PULL must serve at most 500 bytes at a time and a
// single record can be several KB: a page boundary can land anywhere, including
// in the middle of a frame's 2-byte length prefix. Neither function ever
// materialises the whole stream — they walk lines and read only the slice asked
// for, so memory is O(1) in the size of the backlog.

/** Total bytes the unacked region occupies as a framed wire stream. */
uint32_t rr_queue_framed_size(void);

/**
 * Copy up to `cap` bytes of the framed stream starting at wire offset `offset`.
 *
 * Returns bytes written (0 at or past the end — not an error), or -1.
 * A partial frame is a normal, expected result: the caller is paging.
 */
int rr_queue_read_framed(uint8_t *out, uint32_t offset, size_t cap);

/**
 * Ack the record at the head of the queue if its local_id matches, advancing
 * the cursor past it. A repeat ack for an already-advanced record is a
 * deliberate no-op, which is what makes re-flush after a BLE drop safe.
 */
esp_err_t rr_queue_ack(const char *local_id);

/** Oldest unacked record's completed_at epoch, or 0 if the queue is empty. */
uint32_t rr_queue_oldest_ts(void);

/**
 * Register a callback for "the queue depth changed" — a run was appended, or an
 * ack dropped one.
 *
 * TWO CONSUMERS, both of which were previously wrong:
 *
 *   • THE WATCH FACE. Its "N to upload" badge reads rr_queue_count() live, but
 *     only when the face is rebuilt — on wrist-raise or a minute tick. Draining
 *     the queue therefore left the badge on screen for up to a minute after the
 *     records were gone, which reads as "the sync did not work".
 *   • THE PHONE. QUEUE_STATUS notify was only sent from the RUN_ACK handler, so
 *     a connected phone was told about acks it had itself caused and nothing
 *     else. A routine finishing while connected produced no notification at all.
 *
 * A hook rather than direct calls because rr_store sits UNDER both rr_power and
 * rr_ble in the dependency graph; main.c owns both ends and fans this out.
 * Runs on whichever task changed the queue — the LVGL task for an append, the
 * NimBLE host task for an ack — so implementations must not block.
 */
void rr_store_set_queue_changed_hook(void (*fn)(void));

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
 * `iso_weekday` (1=Mon..7=Sun). If nothing remains today, rolls forward to the
 * next day that routine ACTUALLY RUNS ON and sets today=false.
 *
 * Times are LOCAL — schedules are authored as local "HH:MM" in the app, so the
 * caller must pass local wall-clock (rr_rtc_get_local), never UTC.
 *
 * Phase 7: this is now a thin wrapper over rr_store_next_schedule(), so the
 * hint on the face and the routine the scheduler actually fires are computed
 * by the same code and cannot disagree.
 */
esp_err_t rr_store_next_routine(int iso_weekday, int now_hour, int now_min,
                                rr_next_routine_t *out);

// ── Phase 7: schedule matching for the scheduler (§7) ────────────────────────

typedef struct {
    bool found;
    char assignment_id[40];   /**< what rr_routine_request_start() takes */
    char routine_name[64];
    char routine_emoji[16];
    int  days_ahead;          /**< 0 = today, up to 7 */
    int  minute_of_day;       /**< 0..1439, LOCAL wall-clock */
} rr_schedule_hit_t;

/**
 * The earliest scheduled occurrence at or after (from_iso_weekday,
 * from_minute_of_day), searching 8 days forward.
 *
 * DAY-OF-WEEK IS HONOURED. The Phase 6 hint took the earliest HH:MM of any
 * schedule that ran on some other day, so a Monday-only routine was offered on
 * a Tuesday evening. Rolling the weekday forward day by day and testing
 * membership each time is what makes "07:00 tomorrow when it is 23:00 today"
 * and "not until Monday" both come out right.
 *
 * skip_ids/skip_count exclude specific assignment_ids from the occurrence that
 * lands EXACTLY on (from_iso_weekday, from_minute_of_day) — nothing else. That
 * is how two routines sharing one trigger time get queued rather than one of
 * them being silently lost: the scheduler fires the first, then asks again
 * from the same minute with that id skipped (§7 "if multiple routines collide,
 * queue them").
 *
 * ESP_ERR_NOT_FOUND means there is genuinely nothing scheduled, which is a
 * real answer — an unscheduled watch is a supported state, not a fault.
 */
esp_err_t rr_store_next_schedule(int from_iso_weekday, int from_minute_of_day,
                                 const char *const *skip_ids, int skip_count,
                                 rr_schedule_hit_t *out);


// ── Phase 6b: the cached child (§5 /cache/child.json) ────────────────────────

typedef struct {
    bool valid;
    char name[48];
    char avatar_id[16];   /**< 'lion' | 'fox' | ... — an ID, not an emoji */
    char language[4];     /**< "el" | "en" */
    int  level;
    int  total_xp;
} rr_child_t;

/** Read the cached child record. ESP_ERR_NOT_FOUND before the first push. */
esp_err_t rr_store_get_child(rr_child_t *out);
