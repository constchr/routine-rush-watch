// rr_reset_button — local factory reset via a long press on BOOT (GPIO 9).
//
// WHY THIS EXISTS (and why it is not optional):
// The BLE unlink reset only works if the watch is in range when the parent
// unlinks. If it is not, nothing ever reaches it — the app has just deleted
// the device row, so it will never reconnect to that watch again. And a watch
// that still believes it is paired SKIPS THE PAIRING QR on boot, so it cannot
// be re-paired either. Without a local reset an out-of-range unlink bricks the
// watch's usefulness permanently. Same for a watch given away or whose family
// was deleted.
//
// WHY BOOT AND NOT PWR:
// PWR is wired to the AXP2101's PWRON pin, not to a GPIO, so reading it means
// polling PMIC interrupt registers over I2C. BOOT is GPIO 9 (active-low, with
// an internal pull-up) and can be read directly. BOOT is only consulted by the
// ROM bootloader at reset, so using it at runtime is free.
//
// ACCIDENT RESISTANCE (a child must not be able to wipe their own watch):
//   - the hold is LONG: 10 seconds
//   - nothing happens on a short press at all
//   - after 2 seconds an on-screen countdown appears, so the wipe is never
//     silent or surprising
//   - releasing at ANY point aborts and restores the previous screen
// A child would have to hold a recessed side button for ten seconds while
// watching a countdown that tells them what is about to happen.

#include "rr_reset_button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rr_ble.h"
#include "rr_ui.h"

static const char *TAG = "rr_reset_btn";

#define BOOT_GPIO        GPIO_NUM_9
#define POLL_MS          100
#define ARM_MS           2000    // show the countdown after this long
#define HOLD_TO_RESET_MS 10000   // total hold required

static void reset_button_task(void *arg)
{
    (void) arg;
    int held_ms = 0;
    bool countdown_shown = false;

    while (1) {
        // Active low: 0 means pressed.
        bool pressed = gpio_get_level(BOOT_GPIO) == 0;

        if (!pressed) {
            if (countdown_shown) {
                ESP_LOGI(TAG, "reset aborted — button released at %d ms", held_ms);
                rr_ui_restore_after_reset_prompt();
                countdown_shown = false;
            }
            held_ms = 0;
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }

        held_ms += POLL_MS;

        if (held_ms >= ARM_MS) {
            int remaining = (HOLD_TO_RESET_MS - held_ms + 999) / 1000;
            if (remaining < 0) remaining = 0;
            rr_ui_show_reset_countdown(remaining);
            if (!countdown_shown) {
                ESP_LOGW(TAG, "reset countdown started — release to abort");
                countdown_shown = true;
            }
        }

        if (held_ms >= HOLD_TO_RESET_MS) {
            rr_ble_factory_reset("local long-press on BOOT");
            // not reached — esp_restart()
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t rr_reset_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(reset_button_task, "rr_reset_btn", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the reset-button task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "local factory reset armed: hold BOOT (GPIO%d) for %d s",
             BOOT_GPIO, HOLD_TO_RESET_MS / 1000);
    return ESP_OK;
}
