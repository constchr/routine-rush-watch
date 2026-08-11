// rr_identity — persistent device identity + pairing nonce.

#include "rr_identity.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "rr_identity";

#define NVS_NAMESPACE "rr_watch"
#define NVS_KEY_DEVICE_ID "device_id"

static char s_device_id[RR_DEVICE_ID_LEN];
static bool s_first_boot;

static void mint_uuid_v4(char *out, size_t out_len)
{
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));

    // RFC 4122 §4.4: set version (4) and variant (10xx).
    b[6] = (uint8_t) ((b[6] & 0x0F) | 0x40);
    b[8] = (uint8_t) ((b[8] & 0x3F) | 0x80);

    snprintf(out, out_len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

esp_err_t rr_identity_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    size_t len = sizeof(s_device_id);
    err = nvs_get_str(h, NVS_KEY_DEVICE_ID, s_device_id, &len);

    if (err == ESP_OK) {
        s_first_boot = false;
        ESP_LOGI(TAG, "device_id (from NVS): %s", s_device_id);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        mint_uuid_v4(s_device_id, sizeof(s_device_id));
        err = nvs_set_str(h, NVS_KEY_DEVICE_ID, s_device_id);
        if (err == ESP_OK) err = nvs_commit(h);
        if (err != ESP_OK) {
            // Refuse to run on a volatile identity: a device_id that changes
            // across boots would orphan the server-side pairing and quietly
            // create a duplicate device row on every reboot.
            ESP_LOGE(TAG, "failed to persist device_id: %s", esp_err_to_name(err));
            nvs_close(h);
            return err;
        }
        s_first_boot = true;
        ESP_LOGW(TAG, "FIRST BOOT — minted device_id: %s", s_device_id);
    } else {
        ESP_LOGE(TAG, "nvs_get_str failed: %s", esp_err_to_name(err));
        nvs_close(h);
        return err;
    }

    nvs_close(h);
    return ESP_OK;
}

const char *rr_identity_device_id(void)
{
    return s_device_id;
}

bool rr_identity_was_first_boot(void)
{
    return s_first_boot;
}

void rr_identity_new_nonce(char *out, size_t out_len)
{
    uint8_t b[8];
    esp_fill_random(b, sizeof(b));
    for (size_t i = 0; i < sizeof(b) && (i * 2 + 2) < out_len; i++) {
        snprintf(out + i * 2, out_len - i * 2, "%02x", b[i]);
    }
}
