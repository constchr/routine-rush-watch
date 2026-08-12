// rr_imu — QMI8658 shared bring-up + hardware wake-on-motion.

#include "rr_imu.h"

#include <inttypes.h>
#include <math.h>

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
static bool s_ped_active;
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

// QMI8658 register map (datasheet §8).
#define REG_CTRL1     0x02
#define REG_CTRL2     0x03   /* accel range + ODR */
#define REG_CTRL7     0x08   /* sensor enable; bit7 = sync-sample mode */
#define REG_CTRL8     0x09   /* bit4 = pedometer enable */
#define REG_CTRL9     0x0A   /* host command */
#define REG_CAL1_L    0x0B
#define REG_CAL1_H    0x0C
#define REG_CAL2_L    0x0D
#define REG_CAL2_H    0x0E
#define REG_CAL3_L    0x0F
#define REG_CAL3_H    0x10
#define REG_CAL4_L    0x11
#define REG_CAL4_H    0x12
#define REG_STATUSINT 0x2D   /* bit7 = CmdDone */
#define REG_STATUS0   0x2E
#define REG_STATUS1   0x2F   /* WoM / pedometer event latch; read to clear */
#define REG_AX_L      0x35
#define REG_STEP_CNT_L 0x5A
#define REG_STEP_CNT_M 0x5B
#define REG_STEP_CNT_H 0x5C

#define CTRL9_CMD_ACK          0x00
#define CTRL9_CMD_WRITE_WOM    0x08
#define CTRL9_CMD_CONFIG_PED   0x0D
#define CTRL9_CMD_RESET_PED    0x0F

#define CTRL8_PEDOMETER_BIT 0x10

// ── The CTRL9 handshake, in one place ────────────────────────────────────────
//
// The datasheet protocol is FOUR steps, not one:
//   1. write the command to CTRL9
//   2. wait for STATUSINT bit7 (CmdDone) to SET
//   3. write CTRL9_CMD_ACK to CTRL9
//   4. wait for CmdDone to CLEAR
//
// Steps 3 and 4 are the ones everyone drops. Phase 6 found the vendor helper
// omitting step 1's follow-through entirely; this file previously did 1 and 2
// and stopped, which happens to work for a single WoM write but leaves CmdDone
// latched. The pedometer issues TWO CTRL9 commands back to back, and a latched
// CmdDone makes the second one's "wait for set" pass instantly against the
// stale flag from the first — so the configuration silently half-applies.
//
// Every CTRL9 user goes through here now.
static esp_err_t ctrl9_wait_cmddone(bool want_set)
{
    for (int i = 0; i < 60; i++) {          // ~300 ms budget
        uint8_t si = 0;
        if (qmi8658_read_register(&s_dev, REG_STATUSINT, &si, 1) == ESP_OK) {
            const bool is_set = (si & 0x80) != 0;
            if (is_set == want_set) return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t ctrl9_command(uint8_t cmd)
{
    esp_err_t err = qmi8658_write_register(&s_dev, REG_CTRL9, cmd);
    if (err != ESP_OK) return err;

    if (ctrl9_wait_cmddone(true) != ESP_OK) {
        ESP_LOGW(TAG, "CTRL9 cmd 0x%02X: CmdDone never set — command not applied", cmd);
        return ESP_ERR_TIMEOUT;
    }

    err = qmi8658_write_register(&s_dev, REG_CTRL9, CTRL9_CMD_ACK);
    if (err != ESP_OK) return err;

    if (ctrl9_wait_cmddone(false) != ESP_OK) {
        // Not fatal for THIS command (it was acknowledged), but the next CTRL9
        // command would see a stale flag, so say so out loud.
        ESP_LOGW(TAG, "CTRL9 cmd 0x%02X: CmdDone did not clear after ACK", cmd);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

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

    // 4. Issue the command through the full handshake — the step the vendor
    //    omits. Phase 9 moved this into ctrl9_command() so the pedometer's
    //    back-to-back commands cannot trip over a latched CmdDone flag.
    if (ctrl9_command(CTRL9_CMD_WRITE_WOM) != ESP_OK) {
        ESP_LOGW(TAG, "CTRL9 WoM command not acknowledged — WoM may be inactive");
    }

    // 5. Accelerometer back on. CTRL7: bit0 = accel, bit1 = gyro, bit7 = sync-
    //    sample mode. 0x01 is accel-only with sync-sample OFF, which is both
    //    what wake-on-motion needs and the only mode the pedometer engine runs
    //    in ("Non-SyncSample mode", datasheet).
    //
    //    This wrote 0x03 on the disarm path, which switched the GYRO ON every
    //    time the screen woke — the expensive half of the part, contradicting
    //    this file's own accel-only comment, and nothing reads the gyro.
    err = qmi8658_write_register(&s_dev, REG_CTRL7, 0x01);
    return err;
}

esp_err_t rr_imu_arm_wake_on_motion(uint8_t threshold, rr_imu_motion_cb_t cb)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    s_cb = cb;

    // Sampling rate while watching for a wrist raise.
    //
    // 21 Hz low-power is the cheapest mode that still notices an arm being
    // raised, and it is what this used before the pedometer existed. But the
    // step engine's timing parameters are counted in SAMPLES at a fixed ODR
    // (rr_imu.h), and the watch is asleep for most of the day — which is
    // exactly when most steps happen. Dropping to 21 Hz here would either stop
    // the engine counting or silently change what every one of its thresholds
    // means, and a pedometer that only counts while you are looking at the
    // screen is not a pedometer.
    //
    // So: pedometer active wins. The power difference is real and named in the
    // Phase 9 report as a Phase 10 item.
    qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_2G);
    qmi8658_set_accel_odr(&s_dev, s_ped_active ? QMI8658_ACCEL_ODR_62_5HZ
                                               : QMI8658_ACCEL_ODR_LOWPOWER_21HZ);

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
    // STATUS1 (0x2F) is the register whose read clears the WoM latch. This line
    // used to hit 0x2E, which is STATUS0 — harmless but not the latch, so the
    // clear was relying on the settle delay alone. Both are read now.
    qmi8658_read_register(&s_dev, REG_STATUS0, &status, 1);
    qmi8658_read_register(&s_dev, REG_STATUS1, &status, 1);
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

    // Back to normal sampling. When the pedometer is running, "normal" is its
    // fixed ODR and nothing else — see the note in rr_imu_arm_wake_on_motion().
    esp_err_t err = wom_apply(0, false);
    if (s_ped_active) {
        qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_2G);
        qmi8658_set_accel_odr(&s_dev, QMI8658_ACCEL_ODR_62_5HZ);
    } else {
        qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_8G);
        qmi8658_set_accel_odr(&s_dev, QMI8658_ACCEL_ODR_500HZ);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "disarm: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "wake-on-motion disarmed");
    return err;
}

bool rr_imu_is_ready(void) { return s_ready; }

qmi8658_dev_t *rr_imu_dev(void) { return s_ready ? &s_dev : NULL; }

// ═════════════════════════════════════════════════════════════════════════════
// Pedometer — the QMI8658's on-chip step engine (§10.1)
//
// See rr_imu.h for why this uses the register map rather than a vendor helper,
// and why no interrupt is routed.
// ═════════════════════════════════════════════════════════════════════════════

// ── Gait parameters, and what to move if the count is wrong ─────────────────
//
// All timing values are in ACCELEROMETER SAMPLES at RR_IMU_PED_ODR_HZ (62.5),
// which is the ODR the datasheet's own reference values are quoted against.
// The QST reference set is for an adult wrist; the deltas below are the
// child-gait adjustments, and each says which way to move it.
//
//   PED_SAMPLE_CNT    50   calculation window (0.8 s). Reference value, kept.
//
//   PED_FIX_PEAK2PEAK 200  mg, minimum valid peak-to-peak of a step.
//   PED_FIX_PEAK      100  mg, minimum peak above the running average.
//                          KEPT AT THE ADULT REFERENCE ON PURPOSE. A shorter
//                          stride gives a weaker signal, which argues for
//                          lowering these — but a child's wrist also moves far
//                          more abruptly than an adult's, and lowering the
//                          amplitude gate is the single fastest way to turn arm
//                          waving into steps. The brief says err toward
//                          under-counting, so the amplitude gate stays strict
//                          and rejection is handled by the entry count below.
//                          → LOWER THESE (e.g. 160 / 80) IF THE WALK TEST
//                            READS SHORT.
//
//   PED_TIME_UP       200  samples (3.2 s) — a step must follow within this or
//                          the continuity streak resets. Generous on purpose:
//                          children stop and start constantly, and a short
//                          timeout would keep discarding the entry streak and
//                          lose the first several steps of every burst.
//
//   PED_TIME_LOW      20   samples (0.32 s) minimum between steps, i.e. a
//                          ceiling of ~3 steps/s. Fast enough for a running
//                          child, slow enough to reject the 4-6 Hz buzz of a
//                          shaken arm.
//
//   PED_TIME_CNT_ENTRY 12  consecutive steps before ANY are counted (the whole
//                          streak is then credited retroactively). This is the
//                          real anti-false-positive mechanism and it is raised
//                          from the reference 10: waving, clapping and swinging
//                          all produce step-like peaks at step-like rates, so
//                          amplitude and timing cannot separate them — only
//                          "did this continue like walking does" can.
//                          → RAISE IT IF ARM WAVING STILL REGISTERS.
//                          → LOWER IT IF SHORT WALKS GO UNCOUNTED.
//
//   PED_SIG_COUNT      1   update the output register every valid step. The
//                          reference uses 4 because it drives an interrupt; we
//                          poll, so per-step resolution is free and makes a
//                          50-step accuracy test meaningful instead of
//                          quantised to the nearest 4.
#define PED_SAMPLE_CNT      50
#define PED_FIX_PEAK2PEAK   200
#define PED_FIX_PEAK        100
#define PED_TIME_UP         200
#define PED_TIME_LOW        20
#define PED_TIME_CNT_ENTRY  12
#define PED_FIX_PRECISION   0
#define PED_SIG_COUNT       1

esp_err_t rr_imu_pedometer_enable(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    // The engine only runs in Non-SyncSample mode and wants the accelerometer
    // stopped while its configuration is staged — the same shape as the WoM
    // sequence above.
    esp_err_t err = qmi8658_write_register(&s_dev, REG_CTRL7, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pedometer: could not stop the accelerometer: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2));

    // Pin the ODR the parameters were computed for, BEFORE configuring: the
    // engine latches its thresholds against the sample rate in force.
    qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_2G);
    qmi8658_set_accel_odr(&s_dev, QMI8658_ACCEL_ODR_62_5HZ);

    // Round 1 — amplitude and window. CAL4 carries the "which parameter group"
    // selector (0x01) that tells the engine how to interpret CAL1..CAL3.
    qmi8658_write_register(&s_dev, REG_CAL1_L, (uint8_t) (PED_SAMPLE_CNT & 0xFF));
    qmi8658_write_register(&s_dev, REG_CAL1_H, (uint8_t) (PED_SAMPLE_CNT >> 8));
    qmi8658_write_register(&s_dev, REG_CAL2_L, (uint8_t) (PED_FIX_PEAK2PEAK & 0xFF));
    qmi8658_write_register(&s_dev, REG_CAL2_H, (uint8_t) (PED_FIX_PEAK2PEAK >> 8));
    qmi8658_write_register(&s_dev, REG_CAL3_L, (uint8_t) (PED_FIX_PEAK & 0xFF));
    qmi8658_write_register(&s_dev, REG_CAL3_H, (uint8_t) (PED_FIX_PEAK >> 8));
    qmi8658_write_register(&s_dev, REG_CAL4_H, 0x01);
    qmi8658_write_register(&s_dev, REG_CAL4_L, 0x02);

    err = ctrl9_command(CTRL9_CMD_CONFIG_PED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pedometer: parameter group 1 was NOT applied");
        qmi8658_write_register(&s_dev, REG_CTRL7, 0x01);
        return err;
    }

    // Round 2 — timing and continuity. Group selector 0x02.
    qmi8658_write_register(&s_dev, REG_CAL1_L, (uint8_t) (PED_TIME_UP & 0xFF));
    qmi8658_write_register(&s_dev, REG_CAL1_H, (uint8_t) (PED_TIME_UP >> 8));
    qmi8658_write_register(&s_dev, REG_CAL2_L, PED_TIME_LOW);
    qmi8658_write_register(&s_dev, REG_CAL2_H, PED_TIME_CNT_ENTRY);
    qmi8658_write_register(&s_dev, REG_CAL3_L, PED_FIX_PRECISION);
    qmi8658_write_register(&s_dev, REG_CAL3_H, PED_SIG_COUNT);
    qmi8658_write_register(&s_dev, REG_CAL4_H, 0x02);
    qmi8658_write_register(&s_dev, REG_CAL4_L, 0x02);

    err = ctrl9_command(CTRL9_CMD_CONFIG_PED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pedometer: parameter group 2 was NOT applied");
        qmi8658_write_register(&s_dev, REG_CTRL7, 0x01);
        return err;
    }

    // Accelerometer back on, then arm the engine.
    err = qmi8658_write_register(&s_dev, REG_CTRL7, 0x01);
    if (err != ESP_OK) return err;

    uint8_t ctrl8 = 0;
    err = qmi8658_read_register(&s_dev, REG_CTRL8, &ctrl8, 1);
    if (err != ESP_OK) return err;
    ctrl8 |= CTRL8_PEDOMETER_BIT;      // read-modify-write: CTRL8 holds other
    err = qmi8658_write_register(&s_dev, REG_CTRL8, ctrl8);   // engines' bits too
    if (err != ESP_OK) return err;

    // Read back rather than assume. This is the cheap check that would have
    // caught the WoM bug on day one.
    uint8_t verify = 0;
    qmi8658_read_register(&s_dev, REG_CTRL8, &verify, 1);
    if ((verify & CTRL8_PEDOMETER_BIT) == 0) {
        ESP_LOGE(TAG, "pedometer: CTRL8 bit4 did not stick (0x%02X) — engine is OFF", verify);
        return ESP_FAIL;
    }

    s_ped_active = true;
    ESP_LOGI(TAG, "pedometer ENABLED (on-chip engine, 2G @ %.1f Hz, CTRL8=0x%02X)",
             RR_IMU_PED_ODR_HZ, verify);
    ESP_LOGI(TAG, "  gait: p2p>=%dmg peak>=%dmg step<=%.2fs gap>=%.2fs entry=%d steps",
             PED_FIX_PEAK2PEAK, PED_FIX_PEAK,
             PED_TIME_UP / RR_IMU_PED_ODR_HZ, PED_TIME_LOW / RR_IMU_PED_ODR_HZ,
             PED_TIME_CNT_ENTRY);
    return ESP_OK;
}

esp_err_t rr_imu_pedometer_disable(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    uint8_t ctrl8 = 0;
    esp_err_t err = qmi8658_read_register(&s_dev, REG_CTRL8, &ctrl8, 1);
    if (err != ESP_OK) return err;
    ctrl8 &= (uint8_t) ~CTRL8_PEDOMETER_BIT;
    err = qmi8658_write_register(&s_dev, REG_CTRL8, ctrl8);

    s_ped_active = false;
    ESP_LOGI(TAG, "pedometer disabled");
    return err;
}

esp_err_t rr_imu_pedometer_read(uint32_t *out_steps)
{
    if (!s_ready || out_steps == NULL) return ESP_ERR_INVALID_ARG;

    // Three consecutive registers, read in one transaction so the 24-bit value
    // cannot tear across a low-byte rollover mid-read.
    uint8_t b[3] = { 0 };
    esp_err_t err = qmi8658_read_register(&s_dev, REG_STEP_CNT_L, b, sizeof(b));
    if (err != ESP_OK) return err;

    *out_steps = ((uint32_t) b[2] << 16) | ((uint32_t) b[1] << 8) | b[0];
    return ESP_OK;
}

esp_err_t rr_imu_pedometer_reset(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return ctrl9_command(CTRL9_CMD_RESET_PED);
}

bool rr_imu_pedometer_is_active(void) { return s_ped_active; }

esp_err_t rr_imu_pedometer_selftest(int seconds)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (seconds < 2) seconds = 2;

    uint32_t start = 0;
    if (rr_imu_pedometer_read(&start) != ESP_OK) {
        ESP_LOGE(TAG, "PEDOMETER SELFTEST FAILED — counter unreadable over I2C");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "pedometer selftest: %d s from count=%" PRIu32 " — MOVE THE WATCH", seconds, start);

    // The accelerometer is sampled alongside the counter so the log can tell
    // "the engine is inert" apart from "nobody moved the watch". Without this
    // the two are indistinguishable, and three test rounds were spent not
    // knowing which had happened.
    uint32_t last = start;
    float peak_overall = 0.0f;

    for (int i = 0; i < seconds; i++) {
        float peak_s = 0.0f;
        for (int k = 0; k < 20; k++) {          // ~20 Hz for one second
            // Straight off the AX..AZ registers rather than through the vendor
            // driver: qmi8658_read_accel_mps2() is DECLARED in the vendor
            // header but never implemented, so calling it fails at link time.
            uint8_t r[6];
            if (qmi8658_read_register(&s_dev, REG_AX_L, r, sizeof(r)) == ESP_OK) {
                const int16_t ax = (int16_t) ((r[1] << 8) | r[0]);
                const int16_t ay = (int16_t) ((r[3] << 8) | r[2]);
                const int16_t az = (int16_t) ((r[5] << 8) | r[4]);
                // 2G full scale => 16384 LSB per g. Deviation of the magnitude
                // from 1 g is ~0 at rest in ANY orientation and large under
                // real movement, which is exactly the discriminator needed.
                const float g = sqrtf((float) ax * ax + (float) ay * ay + (float) az * az) / 16384.0f;
                const float dev = fabsf(g - 1.0f) * 9.80665f;
                if (dev > peak_s) peak_s = dev;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (peak_s > peak_overall) peak_overall = peak_s;

        uint32_t now = 0;
        if (rr_imu_pedometer_read(&now) != ESP_OK) continue;
        if (now != last || peak_s > 2.0f) {
            ESP_LOGI(TAG, "  t=%2ds  steps=%" PRIu32 " (+%" PRIu32 ")  peak motion %.1f m/s^2",
                     i + 1, now, now - last, peak_s);
            last = now;
        }
    }

    const bool moved = peak_overall > 3.0f;   // ~0.3 g: unmistakably handled

    if (last == start) {
        ESP_LOGW(TAG, "════ PEDOMETER SELFTEST: NO STEPS COUNTED ════");
        ESP_LOGW(TAG, "  peak motion seen: %.1f m/s^2 (%s)", peak_overall,
                 moved ? "THE WATCH WAS DEFINITELY MOVED" : "watch was essentially still");
        if (moved) {
            ESP_LOGE(TAG, "  → the accelerometer sees motion and the step engine does not");
            ESP_LOGE(TAG, "    count it. The on-chip engine is INERT on this part; use the");
            ESP_LOGE(TAG, "    software fallback.");
        } else {
            ESP_LOGW(TAG, "  → inconclusive: nothing moved. Re-run while walking.");
        }
        return ESP_ERR_NOT_FINISHED;
    }

    ESP_LOGI(TAG, "════ PEDOMETER SELFTEST: engine INCREMENTS (%" PRIu32 " -> %" PRIu32
                  ", +%" PRIu32 ") ════", start, last, last - start);
    return ESP_OK;
}

// ── Raw accelerometer, for the software pedometer ───────────────────────────
//
// Straight off the AX..AZ registers. The vendor driver's qmi8658_read_accel_mps2()
// is DECLARED in its header but never implemented — calling it fails at link
// time — and qmi8658_read_accel() carries a unit-flag dependency this does not
// need. One 6-byte burst read keeps the sample atomic across all three axes.
esp_err_t rr_imu_read_accel_g(float *x, float *y, float *z)
{
    if (!s_ready || x == NULL || y == NULL || z == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t r[6];
    esp_err_t err = qmi8658_read_register(&s_dev, REG_AX_L, r, sizeof(r));
    if (err != ESP_OK) return err;

    // The step-sampling hold pins the range at 2G, so the scale is fixed at
    // 16384 LSB/g. If that hold is ever lifted while something still reads this,
    // the scale changes with it — hence rr_imu_step_sampling_hold().
    *x = (float) (int16_t) ((r[1] << 8) | r[0]) / 16384.0f;
    *y = (float) (int16_t) ((r[3] << 8) | r[2]) / 16384.0f;
    *z = (float) (int16_t) ((r[5] << 8) | r[4]) / 16384.0f;
    return ESP_OK;
}

void rr_imu_step_sampling_hold(bool hold)
{
    s_ped_active = hold;
    if (!s_ready) return;

    // Apply immediately: the caller may be starting up while the screen is
    // already awake (accel at whatever the last mode left) and the detector's
    // thresholds assume 2G at RR_IMU_PED_ODR_HZ from its first sample.
    if (hold) {
        qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_2G);
        qmi8658_set_accel_odr(&s_dev, QMI8658_ACCEL_ODR_62_5HZ);
        ESP_LOGI(TAG, "accelerometer held at 2G / %.1f Hz for step sampling", RR_IMU_PED_ODR_HZ);
    }
}
