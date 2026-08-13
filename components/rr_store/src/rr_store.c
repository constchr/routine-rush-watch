// rr_store — LittleFS cache (spec §5).
//
// PHASE 3 SCOPE: mount the filesystem and hold the routine cache. The
// completion queue (/queue/runs.log) is Phase 5 and is not here.
//
// Layout (§5):
//   /littlefs/cache/routines.json   denormalized routine set from the phone
//
// The cache is DISPOSABLE by design: if it is lost, the next ROUTINE_PUSH
// refills it. That is why a parse failure deletes the file rather than
// leaving a half-written one — a truncated cache that parses to garbage is
// worse than no cache, because the runtime would show a child the wrong steps.

#include "rr_store.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "cJSON.h"

static const char *TAG = "rr_store";

// Mounted at /lfs, not /littlefs: the generated emoji manifest hardcodes
// "/lfs/emoji/<cp>.bin" and it is generated in the app repo, so the firmware
// matches the manifest rather than the other way round.
// Overridable so tools/host-test can point the same code at a temp directory
// and exercise it on a workstation. The firmware never defines it.
#ifndef MOUNT_POINT
#define MOUNT_POINT   "/lfs"
#endif
#define PARTITION_LBL "littlefs"
#define CACHE_DIR     MOUNT_POINT "/cache"
#define ROUTINES_PATH CACHE_DIR "/routines.json"
#define CHILD_PATH    CACHE_DIR "/child.json"

static bool s_mounted;

esp_err_t rr_store_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = MOUNT_POINT,
        .partition_label = PARTITION_LBL,
        // First boot has an unformatted partition — format rather than fail,
        // otherwise a factory-fresh watch has no cache and no way to make one.
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    err = esp_littlefs_info(PARTITION_LBL, &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "littlefs mounted at %s (partition '%s')", MOUNT_POINT, PARTITION_LBL);
        ESP_LOGI(TAG, "  total=%u bytes  used=%u bytes  free=%u bytes",
                 (unsigned) total, (unsigned) used, (unsigned) (total - used));
    } else {
        ESP_LOGW(TAG, "mounted, but esp_littlefs_info failed: %s", esp_err_to_name(err));
    }

    mkdir(CACHE_DIR, 0755);   // ok if it already exists
    s_mounted = true;
    return ESP_OK;
}

bool rr_store_is_mounted(void)
{
    return s_mounted;
}

esp_err_t rr_store_put_routines(const char *json, size_t len)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    if (json == NULL || len == 0) return ESP_ERR_INVALID_ARG;

    // Validate BEFORE persisting. Writing first and discovering the payload is
    // malformed later would leave a bad cache the next boot happily loads.
    cJSON *probe = cJSON_ParseWithLength(json, len);
    if (probe == NULL) {
        ESP_LOGE(TAG, "refusing to cache: payload is not valid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    // Two accepted shapes:
    //   [ ...routines ]              — the original document
    //   { child: {...}, routines: [] } — the same document plus the child it
    //                                    belongs to (§5 caches both)
    // Both are DATA. This is not the v2 mistake of putting commands on a data
    // characteristic, and it needs no change to watchProtocol.ts, whose
    // encodeRoutinePush() takes `unknown` and stringifies it.
    // Whether a child record arrived decides whether the watch face can ever
    // render, so every outcome here is logged. Silence used to be ambiguous:
    // no child in the payload and a failed child.json write looked identical
    // from the monitor, and both end as a watch stuck on "Paired" with no face.
    const cJSON *arr = probe;
    if (cJSON_IsObject(probe)) {
        const cJSON *child = cJSON_GetObjectItemCaseSensitive(probe, "child");
        if (cJSON_IsObject(child)) {
            char *cjson = cJSON_PrintUnformatted(child);
            if (cjson == NULL) {
                ESP_LOGE(TAG, "child record present but could not be re-serialised");
            } else {
                FILE *cf = fopen(CHILD_PATH, "wb");
                if (cf == NULL) {
                    ESP_LOGE(TAG, "fopen(%s) failed (errno %d) — NO WATCH FACE: the "
                                  "face needs a cached child record", CHILD_PATH, errno);
                } else {
                    size_t cw = fwrite(cjson, 1, strlen(cjson), cf);
                    fflush(cf); fsync(fileno(cf)); fclose(cf);
                    if (cw != strlen(cjson)) {
                        ESP_LOGE(TAG, "short write to %s (%u of %u) — deleting",
                                 CHILD_PATH, (unsigned) cw, (unsigned) strlen(cjson));
                        unlink(CHILD_PATH);
                    } else {
                        ESP_LOGI(TAG, "cached child record (%u bytes) → %s",
                                 (unsigned) cw, CHILD_PATH);
                        ESP_LOGI(TAG, "  %s", cjson);
                    }
                }
                free(cjson);
            }
        } else {
            ESP_LOGW(TAG, "envelope carries NO 'child' object — the watch face "
                          "cannot render without one; staying on the status screen");
        }
        arr = cJSON_GetObjectItemCaseSensitive(probe, "routines");
    } else {
        // The bare-array shape: a pre-child app build, or a caller that had no
        // child to send. Same end state — worth naming rather than inferring.
        ESP_LOGW(TAG, "bare routines array (no child envelope) — no watch face");
    }

    if (!cJSON_IsArray(arr)) {
        ESP_LOGE(TAG, "refusing to cache: no routines array in the payload");
        cJSON_Delete(probe);
        return ESP_ERR_INVALID_ARG;
    }
    int count = cJSON_GetArraySize(arr);

    // Persist just the routines array, so routines.json keeps its §5 shape
    // regardless of which envelope it arrived in.
    char *routines_only = cJSON_PrintUnformatted(arr);
    cJSON_Delete(probe);
    if (routines_only == NULL) return ESP_ERR_NO_MEM;
    json = routines_only;
    len = strlen(routines_only);

    FILE *f = fopen(ROUTINES_PATH, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen(%s) for write failed", ROUTINES_PATH);
        free((void *) routines_only);
        return ESP_FAIL;
    }
    size_t written = fwrite(json, 1, len, f);
    // fflush + fsync before close: a power loss between fclose and the flash
    // commit would otherwise leave a truncated file that still parses.
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "short write (%u of %u bytes) — deleting partial cache",
                 (unsigned) written, (unsigned) len);
        unlink(ROUTINES_PATH);
        free((void *) routines_only);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "cached %u bytes → %s (%d routines)", (unsigned) len, ROUTINES_PATH, count);
    free((void *) routines_only);
    return ESP_OK;
}

esp_err_t rr_store_has_routines(void)
{
    struct stat st;
    return (s_mounted && stat(ROUTINES_PATH, &st) == 0 && st.st_size > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// Reads the cache back off flash and logs what is actually in it. This exists
// to PROVE the round trip: "received N bytes" says nothing about whether the
// data survived, parsed, or is the right shape.
esp_err_t rr_store_log_routines(void)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    struct stat st;
    if (stat(ROUTINES_PATH, &st) != 0) {
        ESP_LOGW(TAG, "no cached routines at %s", ROUTINES_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    FILE *f = fopen(ROUTINES_PATH, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen(%s) for read failed", ROUTINES_PATH);
        return ESP_FAIL;
    }

    char *buf = malloc((size_t) st.st_size + 1);
    if (buf == NULL) {
        fclose(f);
        ESP_LOGE(TAG, "out of memory reading cache (%ld bytes)", (long) st.st_size);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(buf, 1, (size_t) st.st_size, f);
    fclose(f);
    buf[got] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, got);
    if (root == NULL || !cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "cached routines.json is corrupt — deleting so the next push refills it");
        cJSON_Delete(root);
        free(buf);
        unlink(ROUTINES_PATH);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "════ ROUTINES READ BACK FROM LITTLEFS (%u bytes on flash) ════", (unsigned) got);

    int n = cJSON_GetArraySize(root);
    for (int i = 0; i < n; i++) {
        cJSON *r = cJSON_GetArrayItem(root, i);
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(r, "name");
        const cJSON *emoji = cJSON_GetObjectItemCaseSensitive(r, "emoji");
        const cJSON *steps = cJSON_GetObjectItemCaseSensitive(r, "steps");
        const cJSON *scheds = cJSON_GetObjectItemCaseSensitive(r, "schedules");

        ESP_LOGI(TAG, "  [%d] %s %s — %d step(s), %d schedule(s)",
                 i,
                 cJSON_IsString(emoji) ? emoji->valuestring : "",
                 cJSON_IsString(name) ? name->valuestring : "(unnamed)",
                 cJSON_IsArray(steps) ? cJSON_GetArraySize(steps) : 0,
                 cJSON_IsArray(scheds) ? cJSON_GetArraySize(scheds) : 0);

        if (cJSON_IsArray(steps)) {
            int sn = cJSON_GetArraySize(steps);
            for (int j = 0; j < sn; j++) {
                cJSON *s = cJSON_GetArrayItem(steps, j);
                const cJSON *lbl = cJSON_GetObjectItemCaseSensitive(s, "label");
                const cJSON *se = cJSON_GetObjectItemCaseSensitive(s, "emoji");
                const cJSON *lim = cJSON_GetObjectItemCaseSensitive(s, "time_limit_s");
                ESP_LOGI(TAG, "        %d. %s %s (%ds)",
                         j + 1,
                         cJSON_IsString(se) ? se->valuestring : "",
                         cJSON_IsString(lbl) ? lbl->valuestring : "(unlabelled)",
                         cJSON_IsNumber(lim) ? lim->valueint : 0);
            }
        }
        if (cJSON_IsArray(scheds)) {
            int cn = cJSON_GetArraySize(scheds);
            for (int j = 0; j < cn; j++) {
                cJSON *c = cJSON_GetArrayItem(scheds, j);
                const cJSON *tt = cJSON_GetObjectItemCaseSensitive(c, "trigger_time");
                const cJSON *days = cJSON_GetObjectItemCaseSensitive(c, "days");
                char dbuf[32] = "";
                if (cJSON_IsArray(days)) {
                    int dn = cJSON_GetArraySize(days);
                    for (int k = 0; k < dn && strlen(dbuf) < sizeof(dbuf) - 3; k++) {
                        cJSON *d = cJSON_GetArrayItem(days, k);
                        snprintf(dbuf + strlen(dbuf), sizeof(dbuf) - strlen(dbuf),
                                 "%s%d", k ? "," : "", cJSON_IsNumber(d) ? d->valueint : 0);
                    }
                }
                ESP_LOGI(TAG, "        @ %s on days [%s]",
                         cJSON_IsString(tt) ? tt->valuestring : "??:??", dbuf);
            }
        }
    }
    ESP_LOGI(TAG, "════ %d routine(s) total ════", n);

    cJSON_Delete(root);
    free(buf);
    return ESP_OK;
}

esp_err_t rr_store_clear_routines(void)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    if (unlink(ROUTINES_PATH) == 0) {
        ESP_LOGW(TAG, "routine cache deleted (%s)", ROUTINES_PATH);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "no routine cache to delete");
    return ESP_ERR_NOT_FOUND;
}

// ── Phase 4: read one step for rendering ─────────────────────────────────────

static void copy_str(char *dst, size_t cap, const cJSON *j, const char *fallback)
{
    const char *src = cJSON_IsString(j) ? j->valuestring : fallback;
    strlcpy(dst, src, cap);
}

esp_err_t rr_store_get_step(int routine_idx, int step_idx, rr_step_view_t *out)
{
    if (!s_mounted || out == NULL) return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(ROUTINES_PATH, &st) != 0) return ESP_ERR_NOT_FOUND;

    FILE *f = fopen(ROUTINES_PATH, "rb");
    if (f == NULL) return ESP_FAIL;
    char *buf = malloc((size_t) st.st_size + 1);
    if (buf == NULL) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t got = fread(buf, 1, (size_t) st.st_size, f);
    fclose(f);
    buf[got] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, got);
    free(buf);
    if (root == NULL || !cJSON_IsArray(root)) { cJSON_Delete(root); return ESP_ERR_INVALID_STATE; }

    esp_err_t err = ESP_ERR_NOT_FOUND;
    memset(out, 0, sizeof(*out));
    out->routine_count = cJSON_GetArraySize(root);

    cJSON *r = cJSON_GetArrayItem(root, routine_idx);
    if (r != NULL) {
        copy_str(out->routine_name, sizeof(out->routine_name),
                 cJSON_GetObjectItemCaseSensitive(r, "name"), "(unnamed)");
        copy_str(out->routine_emoji, sizeof(out->routine_emoji),
                 cJSON_GetObjectItemCaseSensitive(r, "emoji"), "");
        copy_str(out->assignment_id, sizeof(out->assignment_id),
                 cJSON_GetObjectItemCaseSensitive(r, "assignment_id"), "");

        copy_str(out->assignment_id, sizeof(out->assignment_id),
                 cJSON_GetObjectItemCaseSensitive(r, "assignment_id"), "");

        const cJSON *steps = cJSON_GetObjectItemCaseSensitive(r, "steps");
        if (cJSON_IsArray(steps)) {
            out->step_count = cJSON_GetArraySize(steps);
            cJSON *s = cJSON_GetArrayItem((cJSON *) steps, step_idx);
            if (s != NULL) {
                copy_str(out->label, sizeof(out->label),
                         cJSON_GetObjectItemCaseSensitive(s, "label"), "(unlabelled)");
                copy_str(out->emoji, sizeof(out->emoji),
                         cJSON_GetObjectItemCaseSensitive(s, "emoji"), "");
                copy_str(out->step_id, sizeof(out->step_id),
                         cJSON_GetObjectItemCaseSensitive(s, "id"), "");
                const cJSON *lim = cJSON_GetObjectItemCaseSensitive(s, "time_limit_s");
                out->time_limit_s = cJSON_IsNumber(lim) ? lim->valueint : 0;
                const cJSON *bx = cJSON_GetObjectItemCaseSensitive(s, "base_xp");
                out->base_xp = cJSON_IsNumber(bx) ? bx->valueint : 0;
                out->position = step_idx;
                err = ESP_OK;
            }
        }
    }

    cJSON_Delete(root);
    return err;
}

esp_err_t rr_store_find_routine(const char *assignment_id, int *out_idx)
{
    if (!s_mounted || assignment_id == NULL || out_idx == NULL || assignment_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(ROUTINES_PATH, &st) != 0) return ESP_ERR_NOT_FOUND;

    FILE *f = fopen(ROUTINES_PATH, "rb");
    if (f == NULL) return ESP_FAIL;
    char *buf = malloc((size_t) st.st_size + 1);
    if (buf == NULL) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t got = fread(buf, 1, (size_t) st.st_size, f);
    fclose(f);
    buf[got] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, got);
    free(buf);
    if (root == NULL || !cJSON_IsArray(root)) { cJSON_Delete(root); return ESP_ERR_INVALID_STATE; }

    esp_err_t err = ESP_ERR_NOT_FOUND;
    int n = cJSON_GetArraySize(root);
    for (int i = 0; i < n; i++) {
        const cJSON *r = cJSON_GetArrayItem(root, i);
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(r, "assignment_id");
        if (cJSON_IsString(id) && strcmp(id->valuestring, assignment_id) == 0) {
            *out_idx = i;
            err = ESP_OK;
            break;
        }
    }

    if (err != ESP_OK) {
        // Names the miss precisely: "not in a cache of N" is the difference
        // between a stale phone and an empty watch, and the phone's next move
        // differs for each.
        ESP_LOGW(TAG, "routine %s not in the cache (%d cached)", assignment_id, n);
    }

    cJSON_Delete(root);
    return err;
}

// ═════════════════════════════════════════════════════════════════════════════
// Phase 5 — the durable completion queue
// ═════════════════════════════════════════════════════════════════════════════

static void (*s_queue_changed)(void);

void rr_store_set_queue_changed_hook(void (*fn)(void))
{
    s_queue_changed = fn;
}

static void queue_changed(void)
{
    if (s_queue_changed != NULL) s_queue_changed();
}

#define QUEUE_DIR    MOUNT_POINT "/queue"
#define RUNS_PATH    QUEUE_DIR "/runs.log"
#define CURSOR_PATH  QUEUE_DIR "/cursor.json"

// ⚠️ SANITY BOUND, NOT A WORKING SIZE. Nothing here reads a record into a
// buffer of this size — readers measure each line and allocate to fit (see
// line_len_at / read_line_at). This exists only so a corrupt log with no
// newline in it cannot make a scan run to the end of the filesystem.
//
// It replaced QUEUE_LINE_MAX = 1024, which WAS a working size and was wrong:
// a record costs ~260 B + ~113 B/step, so an 8-step routine exceeded it and
// fgets silently truncated. See the note above rr_queue_ack().
//
// 64 KB is also the real ceiling on a record: the wire frame length is a u16.
#define QUEUE_RECORD_MAX 65535

static long cursor_get(void)
{
    FILE *f = fopen(CURSOR_PATH, "rb");
    if (f == NULL) return 0;
    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    cJSON *j = cJSON_Parse(buf);
    long off = 0;
    if (j) {
        const cJSON *o = cJSON_GetObjectItemCaseSensitive(j, "offset");
        if (cJSON_IsNumber(o)) off = (long) o->valuedouble;
        cJSON_Delete(j);
    }
    return off < 0 ? 0 : off;
}

static esp_err_t cursor_set(long off)
{
    FILE *f = fopen(CURSOR_PATH, "wb");
    if (f == NULL) return ESP_FAIL;
    fprintf(f, "{\"offset\":%ld}", off);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return ESP_OK;
}

// Once every record has been acked, reclaim the log rather than letting it
// grow forever. Truncating only when fully drained keeps this trivially safe:
// there is nothing left that a reader could still need.
static void compact_if_drained(void)
{
    struct stat st;
    if (stat(RUNS_PATH, &st) != 0) return;
    if (cursor_get() >= st.st_size && st.st_size > 0) {
        unlink(RUNS_PATH);
        cursor_set(0);
        ESP_LOGI(TAG, "queue fully acked — log compacted");
    }
}

esp_err_t rr_queue_append(const char *json, size_t len)
{
    if (!s_mounted || json == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    mkdir(QUEUE_DIR, 0755);

    FILE *f = fopen(RUNS_PATH, "ab");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot open %s for append", RUNS_PATH);
        return ESP_FAIL;
    }
    size_t w = fwrite(json, 1, len, f);
    fputc('\n', f);
    // fsync before returning: the caller is about to tell the child their
    // routine is complete, and that claim must already be durable.
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (w != len) {
        ESP_LOGE(TAG, "queue append short write (%u/%u)", (unsigned) w, (unsigned) len);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "queued run (%u bytes) — %d now pending", (unsigned) len, rr_queue_count());
    queue_changed();
    return ESP_OK;
}

int rr_queue_count(void)
{
    if (!s_mounted) return 0;
    struct stat st;
    if (stat(RUNS_PATH, &st) != 0) return 0;

    long off = cursor_get();
    if (off >= st.st_size) return 0;

    FILE *f = fopen(RUNS_PATH, "rb");
    if (f == NULL) return 0;
    fseek(f, off, SEEK_SET);
    int lines = 0, c;
    while ((c = fgetc(f)) != EOF) if (c == '\n') lines++;
    fclose(f);
    return lines;
}

int rr_queue_read_unacked(char *out, size_t cap)
{
    if (!s_mounted) return -1;
    struct stat st;
    if (stat(RUNS_PATH, &st) != 0) return 0;

    long off = cursor_get();
    if (off >= st.st_size) return 0;
    size_t need = (size_t) (st.st_size - off);
    if (out == NULL) return (int) need;
    if (need > cap) return -1;

    FILE *f = fopen(RUNS_PATH, "rb");
    if (f == NULL) return -1;
    fseek(f, off, SEEK_SET);
    size_t got = fread(out, 1, need, f);
    fclose(f);
    return (int) got;
}

// ── Line geometry, without a fixed-size line buffer ──────────────────────────
//
// ⚠️ WHAT THIS REPLACES, AND WHY IT MATTERED. Every reader here used
// `fgets(buf, QUEUE_LINE_MAX, f)` with QUEUE_LINE_MAX = 1024. A run record
// costs ~260 B plus ~113 B per step, so an 8-step routine crosses 1024 bytes —
// and past that fgets silently returns a TRUNCATED line. In rr_queue_ack that
// was not cosmetic: the truncated JSON failed to parse, local_id never matched,
// the ack was logged as "does not match the queue head — ignoring", and the
// cursor never advanced. The record became permanently un-ackable.
//
// So paging QUEUE_PULL alone would not have fixed the drain: the phone would
// finally have been able to READ an 11-step record and still never able to
// retire it. Both ends of the pipe had the same fixed-size assumption.
//
// Nothing here assumes a line length. Lengths are measured by scanning, and
// content is read in bounded slices.

/**
 * Byte length of the line beginning at `off`, excluding the newline.
 * Returns -1 if there is no line there or it exceeds QUEUE_RECORD_MAX.
 */
static long line_len_at(FILE *f, long off)
{
    if (fseek(f, off, SEEK_SET) != 0) return -1;
    long n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') return n;
        if (++n > QUEUE_RECORD_MAX) return -1;   // corrupt or runaway
    }
    return n > 0 ? n : -1;   // final line with no trailing newline
}

/** Read the line at `off` into a fresh NUL-terminated heap buffer. */
static char *read_line_at(FILE *f, long off, long *len_out)
{
    long len = line_len_at(f, off);
    if (len < 0) return NULL;
    char *buf = malloc((size_t) len + 1);
    if (buf == NULL) return NULL;
    if (fseek(f, off, SEEK_SET) != 0 || fread(buf, 1, (size_t) len, f) != (size_t) len) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    if (len_out) *len_out = len;
    return buf;
}

esp_err_t rr_queue_ack(const char *local_id)
{
    if (!s_mounted || local_id == NULL) return ESP_ERR_INVALID_ARG;
    struct stat st;
    if (stat(RUNS_PATH, &st) != 0) return ESP_ERR_NOT_FOUND;

    long off = cursor_get();
    if (off >= st.st_size) {
        ESP_LOGI(TAG, "ack for %s but queue is already drained — no-op", local_id);
        return ESP_OK;
    }

    FILE *f = fopen(RUNS_PATH, "rb");
    if (f == NULL) return ESP_FAIL;

    // HEAP, not stack. This runs on the nimble_host task (RUN_ACK arrives as a
    // GATT write), whose stack is ~4 KB — and a 1 KB frame here nested inside
    // another 1 KB frame in rr_queue_oldest_ts() overflowed it outright:
    // "Guru Meditation Error: Core 0 panic'ed (Stack protection fault),
    //  task nimble_host", reproducibly, on every ack. Two 1 KB stack buffers in
    // a library reachable from a BLE callback is simply too much to ask. It is
    // now sized to the record instead of to a guess, which is also why the
    // guess no longer has to be big enough for the worst case.
    long len = 0;
    char *line = read_line_at(f, off, &len);
    if (line == NULL) { fclose(f); return ESP_FAIL; }
    // +1 for the newline, unless this was a final line without one.
    long next = off + len + ((off + len < st.st_size) ? 1 : 0);
    fclose(f);

    cJSON *j = cJSON_Parse(line);
    const cJSON *lid = j ? cJSON_GetObjectItemCaseSensitive(j, "local_id") : NULL;
    bool match = cJSON_IsString(lid) && strcmp(lid->valuestring, local_id) == 0;
    cJSON_Delete(j);
    free(line);

    if (!match) {
        // Not the head. Either a duplicate ack for a record already advanced
        // past, or an out-of-order ack. Both are safe to ignore: we only ever
        // send in order, and the record stays queued for the next flush.
        ESP_LOGW(TAG, "ack for %s does not match the queue head — ignoring", local_id);
        return ESP_OK;
    }

    esp_err_t err = cursor_set(next);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "acked %s — cursor %ld, %d still pending", local_id, next, rr_queue_count());
        compact_if_drained();
        queue_changed();
    }
    return err;
}

uint32_t rr_queue_oldest_ts(void)
{
    if (!s_mounted) return 0;
    struct stat st;
    if (stat(RUNS_PATH, &st) != 0) return 0;
    long off = cursor_get();
    if (off >= st.st_size) return 0;

    FILE *f = fopen(RUNS_PATH, "rb");
    if (f == NULL) return 0;
    // Only the HEAD line, sized to itself. This used to pull the whole unacked
    // region into a 1 KB buffer and return -1 (reported as 0) the moment the
    // backlog outgrew it — so a watch with real work to do reported
    // "oldest_ts = 0", i.e. "nothing queued", in QUEUE_STATUS.
    char *line = read_line_at(f, off, NULL);
    fclose(f);
    if (line == NULL) return 0;

    cJSON *j = cJSON_Parse(line);
    uint32_t ts = 0;
    if (j) {
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "completed_epoch");
        if (cJSON_IsNumber(e)) ts = (uint32_t) e->valuedouble;
        cJSON_Delete(j);
    }
    free(line);
    return ts;
}

// ═════════════════════════════════════════════════════════════════════════════
// The framed wire stream (contract v3)
//
// Storage is `json\n` per record; the wire is `[u16 len][json]`. Record i is
// therefore (len_i + 1) bytes on flash and (len_i + 2) on the wire, so the two
// coordinate spaces drift apart by one byte per record and cannot be used
// interchangeably. Everything below is in WIRE offsets from the head of the
// unacked region.
//
// Neither function materialises the stream. They walk the line geometry — which
// costs one scan per record and no heap — and read only the requested slice.
// That matters: the whole point of v3 is that a backlog may be far larger than
// anything the watch can hold in RAM at once.
// ═════════════════════════════════════════════════════════════════════════════

uint32_t rr_queue_framed_size(void)
{
    if (!s_mounted) return 0;
    struct stat st;
    if (stat(RUNS_PATH, &st) != 0) return 0;
    long off = cursor_get();
    if (off >= st.st_size) return 0;

    FILE *f = fopen(RUNS_PATH, "rb");
    if (f == NULL) return 0;

    uint32_t framed = 0;
    while (off < st.st_size) {
        long len = line_len_at(f, off);
        if (len < 0) break;
        framed += (uint32_t) len + RR_QUEUE_FRAME_PREFIX_BYTES;
        off += len + 1;
    }
    fclose(f);
    return framed;
}

int rr_queue_read_framed(uint8_t *out, uint32_t offset, size_t cap)
{
    if (!s_mounted || out == NULL || cap == 0) return -1;
    struct stat st;
    if (stat(RUNS_PATH, &st) != 0) return 0;
    long off = cursor_get();
    if (off >= st.st_size) return 0;

    FILE *f = fopen(RUNS_PATH, "rb");
    if (f == NULL) return -1;

    uint32_t framed_base = 0;   // wire offset of the frame starting at `off`
    size_t written = 0;

    while (off < st.st_size && written < cap) {
        long len = line_len_at(f, off);
        if (len < 0) break;
        const uint32_t frame_size = (uint32_t) len + RR_QUEUE_FRAME_PREFIX_BYTES;

        if (offset >= framed_base + frame_size) {
            // Entirely before the window — skip without reading its bytes.
            framed_base += frame_size;
            off += len + 1;
            continue;
        }

        // `within` is where the copy starts INSIDE this frame: 0 or 1 means it
        // starts inside the 2-byte length prefix, which is exactly the case a
        // page boundary can land on and the reason this cannot be done by
        // seeking to a file offset alone.
        uint32_t within = (offset > framed_base) ? offset - framed_base : 0;

        // 1. the u16 LE length prefix, if the window covers any of it
        while (within < RR_QUEUE_FRAME_PREFIX_BYTES && written < cap) {
            const uint16_t l16 = (uint16_t) len;
            out[written++] = (uint8_t) ((l16 >> (8 * within)) & 0xFF);
            within++;
        }
        // 2. the JSON body, read straight out of the file at the right place
        if (written < cap && within >= RR_QUEUE_FRAME_PREFIX_BYTES) {
            const long json_skip = (long) within - RR_QUEUE_FRAME_PREFIX_BYTES;
            long avail = len - json_skip;
            if (avail > 0) {
                size_t want = (size_t) avail;
                if (want > cap - written) want = cap - written;
                if (fseek(f, off + json_skip, SEEK_SET) != 0) break;
                size_t got = fread(out + written, 1, want, f);
                written += got;
                if (got != want) break;   // short read: stop, report what we have
            }
        }

        framed_base += frame_size;
        off += len + 1;
    }

    fclose(f);
    return (int) written;
}

// ── Phase 6/7: schedule matching ─────────────────────────────────────────────
//
// One implementation, two callers: the idle face's "Next:" hint and the
// scheduler that actually rings. They were separate in Phase 6 and disagreed —
// the hint offered a Monday-only routine on a Tuesday — so they are the same
// search now and a fix to one is a fix to both.

/** Read and parse the routine cache. Caller owns the tree. NULL if absent. */
static cJSON *load_routines(void)
{
    struct stat st;
    if (stat(ROUTINES_PATH, &st) != 0) return NULL;

    FILE *f = fopen(ROUTINES_PATH, "rb");
    if (f == NULL) return NULL;
    char *buf = malloc((size_t) st.st_size + 1);
    if (buf == NULL) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t) st.st_size, f);
    fclose(f);
    buf[got] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, got);
    free(buf);
    if (root != NULL && !cJSON_IsArray(root)) { cJSON_Delete(root); return NULL; }
    return root;
}

/** True if `days` (the JSON array) contains ISO weekday `wd` (1=Mon..7=Sun). */
static bool days_contains(const cJSON *days, int wd)
{
    int n = cJSON_GetArraySize(days);
    for (int k = 0; k < n; k++) {
        const cJSON *d = cJSON_GetArrayItem((cJSON *) days, k);
        if (!cJSON_IsNumber(d)) continue;
        // The app emits ISO weekdays (1=Mon..7=Sun) but the sample data also
        // uses 0 for Sunday, so accept both spellings.
        int v = d->valueint;
        if (v == 0) v = 7;
        if (v == wd) return true;
    }
    return false;
}

/** "HH:MM" -> minutes-of-day, or -1 if it is not a time. */
static int parse_hhmm(const cJSON *tt)
{
    if (!cJSON_IsString(tt)) return -1;
    int hh = 0, mm = 0;
    if (sscanf(tt->valuestring, "%d:%d", &hh, &mm) != 2) return -1;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
    return hh * 60 + mm;
}

esp_err_t rr_store_next_schedule(int from_iso_weekday, int from_minute_of_day,
                                 const char *const *skip_ids, int skip_count,
                                 rr_schedule_hit_t *out)
{
    if (!s_mounted || out == NULL) return ESP_ERR_INVALID_ARG;
    if (from_iso_weekday < 1 || from_iso_weekday > 7) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    cJSON *root = load_routines();
    if (root == NULL) return ESP_ERR_NOT_FOUND;

    // Distance from the search origin, in minutes. Anything strictly smaller
    // than the running best wins, so ties resolve to the FIRST routine in the
    // cache — a stable order, which matters when two routines share a time.
    int best = INT_MAX;

    const int n = cJSON_GetArraySize(root);
    for (int i = 0; i < n; i++) {
        const cJSON *r = cJSON_GetArrayItem(root, i);
        const cJSON *scheds = cJSON_GetObjectItemCaseSensitive(r, "schedules");
        if (!cJSON_IsArray(scheds)) continue;

        const cJSON *id = cJSON_GetObjectItemCaseSensitive(r, "assignment_id");
        if (!cJSON_IsString(id)) continue;   // unstartable: nothing to pass to request_start

        const int sn = cJSON_GetArraySize(scheds);
        for (int j = 0; j < sn; j++) {
            const cJSON *sc = cJSON_GetArrayItem((cJSON *) scheds, j);
            const cJSON *days = cJSON_GetObjectItemCaseSensitive(sc, "days");
            if (!cJSON_IsArray(days)) continue;

            const int at = parse_hhmm(cJSON_GetObjectItemCaseSensitive(sc, "trigger_time"));
            if (at < 0) continue;

            // Walk forward a day at a time. d == 7 is "the same weekday next
            // week", which is what makes a once-a-week schedule resolvable
            // rather than reported as "nothing scheduled".
            for (int d = 0; d <= 7; d++) {
                const int wd = ((from_iso_weekday - 1 + d) % 7) + 1;
                if (!days_contains(days, wd)) continue;
                if (d == 0 && at < from_minute_of_day) continue;   // already past today

                // The skip list applies ONLY to the exact origin minute: it
                // exists to say "this one already fired just now", not to hide
                // the routine from tomorrow.
                if (d == 0 && at == from_minute_of_day && skip_ids != NULL) {
                    bool skipped = false;
                    for (int s = 0; s < skip_count; s++) {
                        if (skip_ids[s] != NULL && strcmp(skip_ids[s], id->valuestring) == 0) {
                            skipped = true;
                            break;
                        }
                    }
                    if (skipped) continue;
                }

                const int distance = d * 1440 + at;
                if (distance < best) {
                    best = distance;
                    out->found = true;
                    out->days_ahead = d;
                    out->minute_of_day = at;
                    strlcpy(out->assignment_id, id->valuestring, sizeof(out->assignment_id));
                    copy_str(out->routine_name, sizeof(out->routine_name),
                             cJSON_GetObjectItemCaseSensitive(r, "name"), "");
                    copy_str(out->routine_emoji, sizeof(out->routine_emoji),
                             cJSON_GetObjectItemCaseSensitive(r, "emoji"), "");
                }
                break;   // nearest day for THIS schedule found; later ones cannot win
            }
        }
    }

    cJSON_Delete(root);
    return out->found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t rr_store_next_routine(int iso_weekday, int now_hour, int now_min,
                                rr_next_routine_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    rr_schedule_hit_t hit;
    esp_err_t err = rr_store_next_schedule(iso_weekday, now_hour * 60 + now_min,
                                           NULL, 0, &hit);
    if (err != ESP_OK) return err;

    out->found = true;
    out->today = (hit.days_ahead == 0);
    out->hour = hit.minute_of_day / 60;
    out->minute = hit.minute_of_day % 60;
    strlcpy(out->routine_name, hit.routine_name, sizeof(out->routine_name));
    strlcpy(out->routine_emoji, hit.routine_emoji, sizeof(out->routine_emoji));
    return ESP_OK;
}


// ── Phase 6b: the cached child record ────────────────────────────────────────

esp_err_t rr_store_get_child(rr_child_t *out)
{
    if (!s_mounted || out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    strlcpy(out->language, "en", sizeof(out->language));

    struct stat st;
    if (stat(CHILD_PATH, &st) != 0) return ESP_ERR_NOT_FOUND;
    FILE *f = fopen(CHILD_PATH, "rb");
    if (f == NULL) return ESP_FAIL;
    char *buf = malloc((size_t) st.st_size + 1);
    if (buf == NULL) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t got = fread(buf, 1, (size_t) st.st_size, f);
    fclose(f);
    buf[got] = '\0';

    cJSON *j = cJSON_ParseWithLength(buf, got);
    free(buf);
    if (j == NULL) return ESP_ERR_INVALID_STATE;

    copy_str(out->name, sizeof(out->name), cJSON_GetObjectItemCaseSensitive(j, "name"), "");
    copy_str(out->avatar_id, sizeof(out->avatar_id),
             cJSON_GetObjectItemCaseSensitive(j, "avatar_id"), "");
    copy_str(out->language, sizeof(out->language),
             cJSON_GetObjectItemCaseSensitive(j, "language"), "en");
    const cJSON *lv = cJSON_GetObjectItemCaseSensitive(j, "level");
    out->level = cJSON_IsNumber(lv) ? lv->valueint : 0;
    const cJSON *xp = cJSON_GetObjectItemCaseSensitive(j, "total_xp");
    out->total_xp = cJSON_IsNumber(xp) ? xp->valueint : 0;
    cJSON_Delete(j);

    out->valid = true;
    return ESP_OK;
}

// ── Factory reset: every file that makes this "someone's watch" ──────────────
//
// rr_store_clear_routines() alone is NOT a reset. It leaves child.json (name,
// avatar, language, level) and the queue, so a "reset" watch still carried the
// previous child's identity and their unrelayed completions into its next
// pairing — and the surviving avatar is what the watch face drew.
//
// The queue goes too, deliberately, even though §5 calls it the one
// non-disposable thing here. That rule protects a run from being lost while
// its watch is still the child's. A reset is the parent explicitly severing
// that link: after it there is no paired phone authorised to receive those
// records and no server-side pairing to attribute them to, so keeping them
// would mean holding a child's data on a device that is no longer theirs.
//
// Best-effort per file: a missing file is the desired end state, not a failure.
esp_err_t rr_store_factory_reset(void)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    static const char *const PATHS[] = {
        ROUTINES_PATH, CHILD_PATH, RUNS_PATH, CURSOR_PATH,
    };

    for (size_t i = 0; i < sizeof(PATHS) / sizeof(PATHS[0]); i++) {
        if (unlink(PATHS[i]) == 0) {
            ESP_LOGW(TAG, "wiped %s", PATHS[i]);
        } else if (errno != ENOENT) {
            // Report it, but keep going: leaving the REMAINING files behind
            // because one unlink failed would be strictly worse.
            ESP_LOGE(TAG, "could not wipe %s (errno %d)", PATHS[i], errno);
        }
    }

    ESP_LOGW(TAG, "littlefs wiped — no routines, no child, no queue");
    return ESP_OK;
}
