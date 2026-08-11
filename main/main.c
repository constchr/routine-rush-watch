// Routine Rush Watch — application entry point.
//
// SCAFFOLD ONLY. No module is implemented yet; this boots the board, prints a
// heap report, and idles. Its job right now is to prove the skeleton compiles,
// links and runs on real hardware before any real code goes in.
//
// Build order is tracked in the firmware spec §12.1 (see docs/README.md for
// the pointer). Phase 0 (board bring-up) is complete and signed off.

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)
#error "Routine Rush Watch requires ESP-IDF v5.5+ (board BSP declares idf >=5.5.0)."
#endif

static const char *TAG = "rr_watch";

void app_main(void)
{
    ESP_LOGI(TAG, "Routine Rush Watch — scaffold boot");
    ESP_LOGI(TAG, "ESP-IDF %s", esp_get_idf_version());

    // RAM is the binding constraint on this board: no PSRAM, ~274 KiB free at
    // boot (measured in Phase 0). Log it every boot so a regression in memory
    // headroom is visible immediately rather than as a late-phase crash.
    ESP_LOGI(TAG, "free heap:          %u bytes", (unsigned) esp_get_free_heap_size());
    ESP_LOGI(TAG, "min free heap ever: %u bytes", (unsigned) esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "largest free block: %u bytes",
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    // Phase 1 starts here: rr_ble_init() to bring up the RR_SYNC GATT server.
    // Everything else waits on transport (spec §12.1).

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "idle — free heap %u", (unsigned) esp_get_free_heap_size());
    }
}
