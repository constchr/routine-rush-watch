// Routine Rush Watch — application entry point.
//
// PHASE 2: QR pairing.
//
// The watch mints a persistent device_id, generates an ephemeral pairing
// nonce, renders both as a QR on the AMOLED, and waits. The parent app scans
// it and performs the Supabase register+claim under its own authenticated
// session — the watch itself never talks to Supabase, because wifi is off and
// BLE is the only uplink (spec §2.2, §6B).
//
// Phase 1's TIME_SYNC path is still here and is now bond-gated (WRITE_ENC).
//
// Not implemented: routine pull, sync engine, queue, routine UI, watch face,
// audio.

#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"

#include "rr_ble.h"
#include "rr_identity.h"
#include "rr_rtc.h"
#include "rr_ui.h"

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)
#error "Routine Rush Watch requires ESP-IDF v5.5+ (board BSP declares idf >=5.5.0)."
#endif

static const char *TAG = "rr_watch";

// The exact shape apps/parent/app/watch/scan.tsx parses. It accepts either
// this JSON object or a routinerushparent:// URL with the same two query
// params; JSON is shorter, so the QR stays a lower version and scans faster.
//
// Both fields are strings and both are REQUIRED — the scanner rejects the
// payload outright if either is missing.
#define PAIRING_PAYLOAD_FMT "{\"device_id\":\"%s\",\"nonce\":\"%s\"}"

void app_main(void)
{
    ESP_LOGI(TAG, "Routine Rush Watch — Phase 2 (QR pairing)");
    ESP_LOGI(TAG, "ESP-IDF %s", esp_get_idf_version());

    // Identity first: the QR cannot be built without it, and a failure here is
    // fatal to pairing (see rr_identity.c on why we refuse a volatile id).
    ESP_ERROR_CHECK(rr_identity_init());

    // Display + LVGL. Also brings up the shared I2C bus via the BSP.
    ESP_ERROR_CHECK(rr_ui_init());

    ESP_ERROR_CHECK(rr_rtc_init(bsp_i2c_get_handle()));

    rr_rtc_time_t now;
    char tbuf[24];
    if (rr_rtc_get(&now) == ESP_OK) {
        rr_rtc_format(&now, tbuf, sizeof(tbuf));
        ESP_LOGI(TAG, "RTC at boot: %s (osc_ok=%d)", tbuf, (int) now.osc_ok);
    }

    // Radio up before the QR is shown: the phone bonds over BLE as part of the
    // same pairing moment, so the peripheral must already be advertising by
    // the time the parent scans the code.
    ESP_ERROR_CHECK(rr_ble_init());

    char nonce[RR_NONCE_LEN];
    rr_identity_new_nonce(nonce, sizeof(nonce));

    char payload[128];
    int n = snprintf(payload, sizeof(payload), PAIRING_PAYLOAD_FMT,
                     rr_identity_device_id(), nonce);
    if (n < 0 || n >= (int) sizeof(payload)) {
        ESP_LOGE(TAG, "pairing payload truncated (%d bytes) — refusing to show a bad QR", n);
        return;
    }

    ESP_LOGI(TAG, "──────── PAIRING ────────");
    ESP_LOGI(TAG, "device_id: %s", rr_identity_device_id());
    ESP_LOGI(TAG, "nonce:     %s", nonce);
    ESP_LOGI(TAG, "payload:   %s", payload);
    ESP_LOGI(TAG, "─────────────────────────");

    ESP_ERROR_CHECK(rr_ui_show_pairing_qr(payload));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (rr_rtc_get(&now) == ESP_OK) {
            rr_rtc_format(&now, tbuf, sizeof(tbuf));
            ESP_LOGI(TAG, "RTC %s | ble=%s | heap %u",
                     tbuf,
                     rr_ble_is_connected() ? "CONNECTED" : "advertising",
                     (unsigned) esp_get_free_heap_size());
        }
    }
}
