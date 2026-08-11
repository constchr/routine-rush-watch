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

#define MOUNT_POINT   "/littlefs"
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
