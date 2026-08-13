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
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "rr_ble.h"
#include "rr_idle.h"
#include "rr_ui.h"

static const char *TAG = "rr_reset_btn";

#define BOOT_GPIO        GPIO_NUM_9
#define POLL_MS          100

// ── The three press bands, and why the boundaries are where they are ─────────
//
// Phase 10 promoted this button from "fallback wake" to THE ONLY WAY TO WAKE THE
// SCREEN BY HAND — hardware wake-on-motion is gone (see rr_idle.c). So the split
// between "wake" and "start a factory reset" now matters far more than it did
// when a wrist raise was the normal path and this was a backstop.
//
//   0 .. ARM_MS            RELEASE HERE  ->  wake the screen. Nothing else can
//                          happen in this band: the countdown has not appeared,
//                          so no reset is in progress to abort.
//   ARM_MS .. HOLD_TO_RESET_MS
//                          The countdown is ON SCREEN and ticking. Releasing
//                          aborts it and does NOT wake — the screen is already
//                          lit showing the countdown, and waking here would be
//                          indistinguishable from a normal tap.
//   >= HOLD_TO_RESET_MS    Factory reset.
//
// ⚠️ THE SAFETY PROPERTY: a short press CANNOT start a reset, because a reset
// requires holding through eight further seconds of an explicit on-screen
// countdown. The bands are contiguous and mutually exclusive, so there is no
// duration that does two things or nothing.
#define ARM_MS           2000    // countdown appears; also the wake/no-wake line
#define HOLD_TO_RESET_MS 10000   // total hold required

// ── Interrupt-gated, not polled ──────────────────────────────────────────────
//
// This used to poll GPIO9 at 10 Hz forever — 864,000 wakes a day to notice a
// button that is pressed a handful of times in the device's life. Now the task
// BLOCKS on a semaphore until the ISR says the line moved, and only then polls
// at POLL_MS, which is exactly the window where a 100 ms cadence is needed (to
// time the hold and animate the countdown). Idle cost: zero wakes.
//
// The polling inside a press is deliberate rather than edge-timed: the countdown
// UI has to tick, and a release has to abort it, so there is real per-100 ms work
// while held. The saving is in not doing that work while NOT held.
static SemaphoreHandle_t s_press_sem;

static void IRAM_ATTR boot_isr(void *arg)
{
    (void) arg;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_press_sem, &hp);
    if (hp == pdTRUE) portYIELD_FROM_ISR();
}

static void reset_button_task(void *arg)
{
    (void) arg;

    while (1) {
        // Block until the line moves. ANY edge wakes us: a press starts the
        // hold timing, and the release edge during a hold is picked up by the
        // inner loop below.
        if (xSemaphoreTake(s_press_sem, portMAX_DELAY) != pdTRUE) continue;

        // Debounce, then confirm it is still down — a release edge that arrives
        // while we were not looking must not start a phantom hold.
        vTaskDelay(pdMS_TO_TICKS(20));
        if (gpio_get_level(BOOT_GPIO) != 0) {
            // Spurious or already released: drop any edge queued behind this one
            // so a bounce cannot spin the loop.
            xSemaphoreTake(s_press_sem, 0);
            continue;
        }

        int held_ms = 20;
        bool countdown_shown = false;

        // Held: poll at POLL_MS for as long as it stays down.
        while (gpio_get_level(BOOT_GPIO) == 0) {
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
            held_ms += POLL_MS;
        }

        // Released.
        //
        // A SHORT press is THE manual wake (§9B.2). The spec names the PWR
        // button, but PWR is wired to the AXP2101's PWRON pin rather than a
        // GPIO, so reading it would mean polling PMIC registers over I2C —
        // which is also why PWR could not take this job when Phase 10 removed
        // wake-on-motion. BOOT is directly readable and debounced above.
        //
        // ⚠️ This is no longer a convenience path. With WoM gone it is the only
        // way a child can look at their watch on demand, so a press that fails
        // to wake reads as a broken device. Keep it dumb and unconditional.
        if (held_ms < ARM_MS) {
            ESP_LOGI(TAG, "short press (%d ms) — manual wake", held_ms);
            rr_idle_wake_manual();
        }
        if (countdown_shown) {
            ESP_LOGI(TAG, "reset aborted — button released at %d ms", held_ms);
            rr_ui_restore_after_reset_prompt();
        }

        // The release itself raised an edge; discard it so the outer wait does
        // not immediately re-enter with the button already up.
        xSemaphoreTake(s_press_sem, 0);
    }
}

esp_err_t rr_reset_button_init(void)
{
    s_press_sem = xSemaphoreCreateBinary();
    if (s_press_sem == NULL) return ESP_ERR_NO_MEM;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        // ANY edge: a press starts the hold, a release ends it, and the task
        // needs to see both without a timer running in between.
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    // rr_imu may already have installed the ISR service; either order is fine.
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
        return err;
    }
    err = gpio_isr_handler_add(BOOT_GPIO, boot_isr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(err));
        return err;
    }

    // ── Surviving light sleep (Phase 10) ────────────────────────────────────
    //
    // Automatic light sleep POWERS THE CPU DOWN (CONFIG_PM_POWER_DOWN_CPU_IN_
    // LIGHT_SLEEP), so an ordinary GPIO interrupt does not run — only a
    // registered wake source brings the chip back. Without the two calls below
    // this button would appear to work on the bench (where the watch is awake
    // and plugged in) and be completely dead on a sleeping watch on a wrist.
    //
    // That matters more here than anywhere else in the firmware: this button is
    // the ONLY recovery path for a watch that was unlinked while out of BLE
    // range. A factory reset that silently stops working once power management
    // is switched on would be discovered by a parent, not by us.
    //
    // CONFIG_PM_SLP_DISABLE_GPIO turns every pad off during sleep to save
    // 200-300 uA; gpio_sleep_sel_dis() exempts this one so the pull-up survives
    // and a press is still a real level change. Light-sleep GPIO wakeup is
    // LEVEL-triggered only (no edges), and BOOT idles high through its pull-up,
    // so the press — a LOW level — is the wake condition.
    //
    // ⚠️ COMPILED OUT UNLESS LIGHT SLEEP IS ACTUALLY ON, AND THAT IS NOT
    // TIDINESS — IT IS A BUG FIX. On the ESP32-C6 the light-sleep wake level and
    // the ordinary edge-interrupt type are THE SAME REGISTER FIELD, so calling
    // gpio_wakeup_enable(..., GPIO_INTR_LOW_LEVEL) here overwrote the
    // GPIO_INTR_ANYEDGE configured by gpio_config() above. With WoM removed this
    // button is the only manual way to wake the screen, and the result was a
    // watch that could not be woken by hand at all.
    //
    // Light sleep is off by default (see main.c), so in the shipping build these
    // calls bought nothing and cost the wake path.
#ifdef RR_LIGHT_SLEEP
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_sleep_sel_dis(BOOT_GPIO));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_wakeup_enable(BOOT_GPIO, GPIO_INTR_LOW_LEVEL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_enable_gpio_wakeup());
    // Re-assert the edge config the ISR depends on: the call above shares the
    // INT_TYPE field with it and would otherwise leave this pin level-triggered.
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_intr_type(BOOT_GPIO, GPIO_INTR_ANYEDGE));
#endif

    if (xTaskCreate(reset_button_task, "rr_reset_btn", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the reset-button task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "local factory reset armed: hold BOOT (GPIO%d) for %d s "
                  "(wakes the chip from light sleep)",
             BOOT_GPIO, HOLD_TO_RESET_MS / 1000);
    return ESP_OK;
}
