// Routine Rush Watch — application entry point.
//
// PHASE 6: idle watch face + raise-to-wake.
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
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"

#include "rr_ble.h"
#include "rr_identity.h"
#include "rr_store.h"
#include "rr_routine.h"
#include "rr_imu.h"
#include "rr_idle.h"
#include "rr_battery.h"
#include "rr_rtc.h"
#include "rr_ui.h"
#include "rr_reset_button.h"

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
    ESP_LOGI(TAG, "Routine Rush Watch — Phase 6 (watch face + raise-to-wake)");
    ESP_LOGI(TAG, "ESP-IDF %s", esp_get_idf_version());

    // NVS first — app_main owns the NVS lifecycle for the whole firmware.
    //
    // Two consumers need it and neither may initialise it itself: rr_identity
    // stores the device_id, and NimBLE's bond store persists pairing keys.
    // Doing it here (the conventional ESP-IDF place) means module init order
    // can be reasoned about locally instead of each module racing to be the
    // one that initialised NVS.
    //
    // The erase-and-retry branch is the canonical pattern: NO_FREE_PAGES means
    // the partition is full, NEW_VERSION_FOUND means it was written by a newer
    // NVS format. Both are unrecoverable without a wipe — and wiping costs us
    // the device_id, so the watch would need re-pairing. That is the correct
    // trade: the alternative is a device that cannot boot at all.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS unusable (%s) — erasing; device_id will be regenerated "
                      "and the watch will need re-pairing", esp_err_to_name(nvs_err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_LOGI(TAG, "NVS initialised");

    // Identity next: the QR cannot be built without it, and a failure here is
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

    // Local factory reset (hold BOOT 10 s). Armed early and unconditionally:
    // it is the ONLY recovery for a watch unlinked while out of BLE range.
    ESP_ERROR_CHECK(rr_reset_button_init());

    // LittleFS: the routine cache lives here (spec §5). Mount before BLE, since
    // a ROUTINE_PUSH can arrive as soon as we advertise and its handler writes
    // straight to the cache.
    ESP_ERROR_CHECK(rr_store_init());

    // Radio up: the phone bonds over BLE as part of the same pairing moment,
    // so the peripheral must be advertising by the time the parent scans.
    ESP_ERROR_CHECK(rr_ble_init());

    if (rr_identity_is_paired()) {
        // Already in service — do NOT show the QR again. Re-pairing needs an
        // explicit reset (see rr_identity.h).
        ESP_LOGI(TAG, "already paired — skipping the pairing QR");
        if (rr_store_has_routines() == ESP_OK) {
            rr_store_log_routines();     // prove the cache survived the reboot
            rr_ui_show_paired_status();
        } else {
            ESP_LOGW(TAG, "paired but no routine cache — awaiting a ROUTINE_PUSH");
            rr_ui_show_waiting_status();
        }
    } else {
        char nonce[RR_NONCE_LEN];
        rr_identity_new_nonce(nonce, sizeof(nonce));
        // The nonce gate compares against this value (rr_ble ROUTINE_PUSH).
        rr_identity_set_active_nonce(nonce);

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
    }

    // ── Phase 6: IMU + the idle watch face ──────────────────────────────────
    // rr_imu is the SHARED sensor owner: raise-to-wake uses it now, Phase 9's
    // pedometer extends the same handle rather than re-initialising.
    ESP_ERROR_CHECK_WITHOUT_ABORT(rr_imu_init(bsp_i2c_get_handle()));
    ESP_ERROR_CHECK_WITHOUT_ABORT(rr_battery_init(bsp_i2c_get_handle()));

    // A running routine must not be blanked mid-step.
    rr_idle_set_suspend_check(rr_routine_is_active);

    ESP_ERROR_CHECK_WITHOUT_ABORT(rr_idle_init());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (rr_rtc_get(&now) == ESP_OK) {
            rr_rtc_format(&now, tbuf, sizeof(tbuf));
            ESP_LOGI(TAG, "RTC %s | ble=%s | routine=%s | screen=%s | queued=%d | heap %u",
                     tbuf,
                     rr_ble_is_connected() ? "CONNECTED" : "advertising",
                     rr_routine_is_active() ? "RUNNING" : "idle",
                     rr_idle_is_awake() ? "awake" : "asleep",
                     rr_queue_count(),
                     (unsigned) esp_get_free_heap_size());
        }
    }
}
