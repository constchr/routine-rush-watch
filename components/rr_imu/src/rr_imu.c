// rr_imu — QMI8658 shared bring-up + hardware wake-on-motion.

#include "rr_imu.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "rr_imu";

// Waveshare's pin audit: INT1 = GPIO 16, INT2 = GPIO 17. See the warning in
// rr_imu.h about the console UART fighting for these.
#define IMU_INT1_GPIO GPIO_NUM_16

static qmi8658_dev_t s_dev;
static bool s_ready;
static bool s_armed;
static rr_imu_motion_cb_t s_cb;
static SemaphoreHandle_t s_int_sem;

// The ISR does nothing but hand off. QMI8658 wake-on-motion needs an I2C read
// to clear its status, and I2C cannot be driven from interrupt context.
static void IRAM_ATTR imu_isr(void *arg)
{
    (void) arg;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_int_sem, &hp);
    if (hp == pdTRUE) portYIELD_FROM_ISR();
}

static void imu_int_task(void *arg)
{
    (void) arg;
    while (1) {
        if (xSemaphoreTake(s_int_sem, portMAX_DELAY) != pdTRUE) continue;
        if (!s_armed) continue;   // transient from arming, or a late one after disarm

        ESP_LOGI(TAG, "wake-on-motion interrupt");
        if (s_cb) s_cb();
    }
}

esp_err_t rr_imu_init(i2c_master_bus_handle_t bus)
{
    if (s_ready) return ESP_OK;
    if (bus == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t err = qmi8658_init(&s_dev, bus, QMI8658_ADDRESS_HIGH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "qmi8658_init failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t who = 0;
    qmi8658_get_who_am_i(&s_dev, &who);
    ESP_LOGI(TAG, "QMI8658 up (WHO_AM_I=0x%02X)", who);

    // Accelerometer-only config: wake-on-motion needs no gyro, and the gyro is
    // the expensive half of this part. Phase 9's pedometer also runs on accel.
    qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_8G);
    qmi8658_set_accel_odr(&s_dev, QMI8658_ACCEL_ODR_500HZ);
    qmi8658_set_accel_unit_mps2(&s_dev, true);

    s_int_sem = xSemaphoreCreateBinary();
    if (s_int_sem == NULL) return ESP_ERR_NO_MEM;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << IMU_INT1_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        // ANY edge: the QMI8658's WoM interrupt polarity is configurable and the
        // datasheet's default (CAL1_H initial-value bit) makes it active-HIGH,
        // so keying on one edge is a silent-failure trap. Both edges cost
        // nothing here — the handler is idempotent.
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&cfg));

    // The reset-button component may already have installed the ISR service.
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_isr_handler_add(IMU_INT1_GPIO, imu_isr, NULL));

    if (xTaskCreate(imu_int_task, "rr_imu_int", 3072, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the IMU interrupt task");
        return ESP_FAIL;
    }

    s_ready = true;
    ESP_LOGI(TAG, "IMU ready; INT1 on GPIO%d", IMU_INT1_GPIO);
    return ESP_OK;
}

// QMI8658 register map (datasheet §8) — only what the WoM sequence needs.
#define REG_CTRL1     0x02
#define REG_CTRL7     0x08
#define REG_CTRL9     0x0A
#define REG_CAL1_L    0x0B
#define REG_CAL1_H    0x0C
#define REG_STATUSINT 0x2D
#define CTRL9_CMD_WRITE_WOM 0x08

// ⚠️ The vendor driver's qmi8658_enable_wake_on_motion() does NOT work.
//
// It writes the threshold into CAL1_L/CAL1_H but never issues the CTRL9 host
// command that latches a WoM setting, so the registers are staged and never
// applied — the interrupt simply never fires, silently. Verified on hardware:
// armed via the vendor call, the watch never woke.
//
// This is the datasheet sequence: stage CAL1_L/CAL1_H, issue the CTRL9
// command, wait for the controller to acknowledge it, then re-enable the
// accelerometer. Written against the register map rather than the vendor
// helper so the acknowledgement step cannot be skipped again.
static esp_err_t wom_apply(uint8_t threshold_mg, bool enable)
{
    // 1. Sensors off while the setting is staged.
    esp_err_t err = qmi8658_write_register(&s_dev, REG_CTRL7, 0x00);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(2));

    // 2. Route the interrupt to a pin. CTRL1 bit3 enables INT1 as an output;
    //    without it the pin stays high-Z and nothing reaches GPIO 16.
    uint8_t ctrl1 = 0;
    qmi8658_read_register(&s_dev, REG_CTRL1, &ctrl1, 1);
    ctrl1 |= 0x08;
    qmi8658_write_register(&s_dev, REG_CTRL1, ctrl1);

    // 3. Stage the threshold. CAL1_H: bit7 = INT select (0 = INT1),
    //    bit6 = initial level, bits5..0 = blanking samples. Blanking of 2
    //    samples suppresses the ringing a single knock produces.
    qmi8658_write_register(&s_dev, REG_CAL1_L, enable ? threshold_mg : 0x00);
    qmi8658_write_register(&s_dev, REG_CAL1_H, enable ? 0x02 : 0x00);

    // 4. Issue the command and WAIT for it — the step the vendor omits.
    err = qmi8658_write_register(&s_dev, REG_CTRL9, CTRL9_CMD_WRITE_WOM);
    if (err != ESP_OK) return err;

    bool acked = false;
    for (int i = 0; i < 50; i++) {          // ~250 ms budget
        uint8_t si = 0;
        if (qmi8658_read_register(&s_dev, REG_STATUSINT, &si, 1) == ESP_OK && (si & 0x80)) {
            acked = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!acked) {
        ESP_LOGW(TAG, "CTRL9 WoM command not acknowledged — WoM may be inactive");
    }

    // 5. Accelerometer back on (low-power ODR is set by the caller's config).
    err = qmi8658_write_register(&s_dev, REG_CTRL7, enable ? 0x01 : 0x03);
    return err;
}

esp_err_t rr_imu_arm_wake_on_motion(uint8_t threshold, rr_imu_motion_cb_t cb)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    s_cb = cb;

    // Low-power sampling while watching: 21 Hz is plenty to notice an arm
    // being raised and is the part's own low-power mode.
    qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_2G);
    qmi8658_set_accel_odr(&s_dev, QMI8658_ACCEL_ODR_LOWPOWER_21HZ);

    esp_err_t err = wom_apply(threshold, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "arming wake-on-motion failed: %s", esp_err_to_name(err));
        return err;
    }
    // Arming itself moves INT1 (the pin is driven from high-Z to its configured
    // initial level), and on an any-edge handler that transient is
    // indistinguishable from a wrist raise — observed on hardware as an
    // instant wake 13 ms after arming. Let the line settle, clear any latched
    // status, then discard anything already queued so only genuine motion
    // after this point counts.
    vTaskDelay(pdMS_TO_TICKS(60));
    uint8_t status = 0;
    qmi8658_read_register(&s_dev, REG_STATUSINT, &status, 1);
    qmi8658_read_register(&s_dev, 0x2E, &status, 1);   // STATUS1: clears WoM latch
    xSemaphoreTake(s_int_sem, 0);

    s_armed = true;
    ESP_LOGI(TAG, "wake-on-motion ARMED (threshold %u mg, INT1 idle level=%d)",
             (unsigned) threshold, gpio_get_level(IMU_INT1_GPIO));
    return ESP_OK;
}

esp_err_t rr_imu_disarm_wake_on_motion(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    s_armed = false;

    // Back to normal sampling, which is also the mode Phase 9's pedometer wants.
    esp_err_t err = wom_apply(0, false);
    qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_8G);
    qmi8658_set_accel_odr(&s_dev, QMI8658_ACCEL_ODR_500HZ);
    if (err != ESP_OK) ESP_LOGW(TAG, "disarm: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "wake-on-motion disarmed");
    return err;
}

bool rr_imu_is_ready(void) { return s_ready; }

qmi8658_dev_t *rr_imu_dev(void) { return s_ready ? &s_dev : NULL; }
