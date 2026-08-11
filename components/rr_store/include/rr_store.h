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
