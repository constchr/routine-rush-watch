// Routine Rush Watch — application entry point.
//
// PHASE 1: BLE uplink bring-up, TIME_SYNC only.
//
// The watch advertises RR_SYNC, accepts a connection from the parent app, and
// sets its PCF85063 RTC from the epoch the phone writes. That single path
// exercises advertising, connection, a characteristic write, and a verified
// hardware side effect — everything else in §6B.3 is the same transport again.
//
// Nothing else is implemented: no queue, no routines, no UI, no audio.

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"

#include "rr_ble.h"
#include "rr_rtc.h"

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)
#error "Routine Rush Watch requires ESP-IDF v5.5+ (board BSP declares idf >=5.5.0)."
#endif

static const char *TAG = "rr_watch";

void app_main(void)
{
    ESP_LOGI(TAG, "Routine Rush Watch — Phase 1 (BLE TIME_SYNC)");
    ESP_LOGI(TAG, "ESP-IDF %s", esp_get_idf_version());

    // Shared board I2C bus (GPIO 7/8) — RTC, PMIC, IMU, touch and codec all
    // hang off it. Must come before rr_rtc_init().
    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(rr_rtc_init(bsp_i2c_get_handle()));

    // Report the RTC before any sync. A factory board reads 2000-01-19 with
    // osc_ok=0 — that is the "before" the test is looking for.
    rr_rtc_time_t now;
    char buf[24];
    if (rr_rtc_get(&now) == ESP_OK) {
        rr_rtc_format(&now, buf, sizeof(buf));
        ESP_LOGI(TAG, "RTC at boot: %s (osc_ok=%d)", buf, (int) now.osc_ok);
        if (!now.osc_ok) {
            ESP_LOGW(TAG, "RTC has never been set — waiting for TIME_SYNC over BLE");
        }
    }

    // RTC must be live before the radio: a TIME_SYNC write can land the moment
    // advertising starts.
    ESP_ERROR_CHECK(rr_ble_init());

    ESP_LOGI(TAG, "free heap after BLE init: %u bytes", (unsigned) esp_get_free_heap_size());

    // Heartbeat: prints the RTC every 5 s so the jump is visible in the
    // monitor whether or not you catch the write itself.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (rr_rtc_get(&now) == ESP_OK) {
            rr_rtc_format(&now, buf, sizeof(buf));
            ESP_LOGI(TAG, "RTC %s | osc_ok=%d | ble=%s | heap %u",
                     buf, (int) now.osc_ok,
                     rr_ble_is_connected() ? "CONNECTED" : "advertising",
                     (unsigned) esp_get_free_heap_size());
        }
    }
}
