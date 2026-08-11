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

#include <stdio.h>
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
#define MOUNT_POINT   "/lfs"
#define PARTITION_LBL "littlefs"
#define CACHE_DIR     MOUNT_POINT "/cache"
#define ROUTINES_PATH CACHE_DIR "/routines.json"

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
    if (!cJSON_IsArray(probe)) {
        ESP_LOGE(TAG, "refusing to cache: routines.json must be a JSON array");
        cJSON_Delete(probe);
        return ESP_ERR_INVALID_ARG;
    }
    int count = cJSON_GetArraySize(probe);
    cJSON_Delete(probe);

    FILE *f = fopen(ROUTINES_PATH, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen(%s) for write failed", ROUTINES_PATH);
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
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "cached %u bytes → %s (%d routines)", (unsigned) len, ROUTINES_PATH, count);
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

// ═════════════════════════════════════════════════════════════════════════════
// Phase 5 — the durable completion queue
// ═════════════════════════════════════════════════════════════════════════════

#define QUEUE_DIR    MOUNT_POINT "/queue"
#define RUNS_PATH    QUEUE_DIR "/runs.log"
#define CURSOR_PATH  QUEUE_DIR "/cursor.json"

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
    fseek(f, off, SEEK_SET);

    char line[1024];
    if (fgets(line, sizeof(line), f) == NULL) { fclose(f); return ESP_FAIL; }
    long next = ftell(f);
    fclose(f);

    cJSON *j = cJSON_Parse(line);
    const cJSON *lid = j ? cJSON_GetObjectItemCaseSensitive(j, "local_id") : NULL;
    bool match = cJSON_IsString(lid) && strcmp(lid->valuestring, local_id) == 0;
    cJSON_Delete(j);

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
    }
    return err;
}

uint32_t rr_queue_oldest_ts(void)
{
    char buf[1024];
    int n = rr_queue_read_unacked(buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    cJSON *j = cJSON_Parse(buf);
    uint32_t ts = 0;
    if (j) {
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "completed_epoch");
        if (cJSON_IsNumber(e)) ts = (uint32_t) e->valuedouble;
        cJSON_Delete(j);
    }
    return ts;
}
