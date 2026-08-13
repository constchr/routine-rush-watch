// rr_imu — QMI8658 shared bring-up. Accelerometer sampling only.
//
// ═════════════════════════════════════════════════════════════════════════════
// THE SENSOR DOES NOT REBOOT WHEN THE MCU DOES. SOFT RESET IT AT BRING-UP.
//
// This is the most important fact about this file, and it hid a completely dead
// step counter from Phase 9 until Phase 10.
//
// The QMI8658 keeps its configuration across an ESP32 reset, a reflash and a
// watchdog reboot — it only loses it on a full power cycle. Bring-up here never
// said otherwise: it wrote range and ODR over the top of whatever was already
// there and assumed the rest was default. When the part had been left in a
// non-converting state by an earlier firmware, it stayed that way forever, and
// every reflash inherited it.
//
// THE SIGNATURE, because it is deeply misleading:
//
//     ax=-32768 ay=-32768 az=-32768        <- 0x8000 on every axis
//     -> x=-2.00 y=-2.00 z=-2.00  |a|=3.46 g
//     CTRL2=0x07 (2G, 62.5 Hz)  CTRL7=0x01 (accel on)   <- all CORRECT
//     STATUS0=0x00                                       <- never data-ready
//
// 0x8000 is the "no valid sample" sentinel, and at 2G it scales to exactly
// -2.00 g — so it reads as a *saturated accelerometer* rather than as an absent
// one. Every control register says healthy. Nothing errors. The only real tell
// is that the values NEVER CHANGE, which is indistinguishable from a watch
// nobody moved unless you are explicitly looking for variance (see
// rr_steps_describe_input()).
//
// ⚠️ A CORRECTION WORTH RECORDING. This signature was first blamed on wake-on-
// motion — "arming WoM freezes the output registers" — because it showed up
// while WoM was armed. That was wrong, or at least never demonstrated: the same
// frozen 0x8000 appears with WoM entirely absent from the firmware, and a soft
// reset fixes it either way. The confounder was that the sensor had been left in
// a bad state by a PREVIOUS boot, so the correlation with arming was incidental.
// If wake-on-motion is ever wanted again, do not treat it as ruled out on those
// grounds — re-test it against a freshly reset part.
//
// Wake-on-motion is nonetheless GONE by product decision (Phase 10): the screen
// is woken by a short press on BOOT (rr_reset_button.c) or by the scheduler when
// a routine is due (rr_sched -> rr_idle_wake_manual). That keeps the
// accelerometer in one continuous sampling mode all day, which is what rr_steps
// needs, and removes a whole class of mode-switching bugs.
// ═════════════════════════════════════════════════════════════════════════════

#include "rr_imu.h"

#include <inttypes.h>
#include <math.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rr_imu";

// GPIO 16/17 (QMI8658 INT1/INT2) are now UNUSED by this firmware. The
// sdkconfig.defaults warning about keeping the console off those pins is
// therefore no longer load-bearing — but leave it in place: routing the console
// back onto them would make re-adding any interrupt feature silently impossible,
// and that warning is cheaper than rediscovering it.

// QMI8658 register map (datasheet §8). Above its first user, because rr_imu_init
// now enables the accelerometer directly rather than leaving it to a WoM path.
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
#define REG_STATUS1   0x2F
#define REG_AX_L      0x35
#define REG_STEP_CNT_L 0x5A
#define REG_STEP_CNT_M 0x5B
#define REG_STEP_CNT_H 0x5C

#define CTRL9_CMD_ACK          0x00
#define CTRL9_CMD_CONFIG_PED   0x0D
#define CTRL9_CMD_RESET_PED    0x0F

#define CTRL8_PEDOMETER_BIT 0x10

// Soft reset. Not in the vendor driver's register list, and the reason this
// file needs it is in rr_imu_init(): the sensor survives an MCU reboot.
#define REG_RESET             0x60
#define QMI8658_SOFT_RESET_CMD 0xB0

static qmi8658_dev_t s_dev;
static bool s_ready;
static bool s_ped_active;

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

    // ── SOFT RESET FIRST. THE SENSOR DOES NOT REBOOT WHEN THE MCU DOES. ──────
    //
    // ⚠️ This is the bug that made step counting look impossible. The QMI8658
    // keeps its configuration across an ESP32 reset, a reflash, and a watchdog
    // reboot — it only loses it on a full power cycle. So whatever mode the
    // PREVIOUS firmware left it in is the mode the NEXT one starts in, and
    // bring-up here never said otherwise; it wrote range and ODR over the top
    // and assumed the rest was default.
    //
    // The symptom is brutal to diagnose because everything reads healthy:
    //
    //     CTRL2=0x07 (2G, 62.5 Hz)  CTRL7=0x01 (accel on)   <- correct
    //     STATUS0=0x00                                       <- never data-ready
    //     ax=-1181 ay=1315 az=15882, byte-identical across 500 ms
    //
    // A perfectly plausible gravity vector (|a| = 0.98 g), frozen. Registers
    // configured, sensor not converting, no error anywhere.
    //
    // Writing 0xB0 to RESET (0x60) puts the part in a known state, so bring-up
    // depends on nothing that came before it. Everything below this line is
    // applied to a freshly reset device.
    qmi8658_write_register(&s_dev, REG_RESET, QMI8658_SOFT_RESET_CMD);
    vTaskDelay(pdMS_TO_TICKS(20));   // datasheet: ~15 ms to come back

    // The reset drops the I2C-level settings the vendor init applied (address
    // auto-increment in particular), so re-run it against the clean device
    // rather than assuming the handle is still describing reality.
    err = qmi8658_init(&s_dev, bus, QMI8658_ADDRESS_HIGH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "qmi8658_init after soft reset failed: %s", esp_err_to_name(err));
        return err;
    }

    // Accelerometer only — the gyro is the expensive half of this part and
    // nothing reads it. rr_steps immediately re-pins range and rate via
    // rr_imu_step_sampling_hold(); these are just a sane state before it does.
    qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_2G);
    qmi8658_set_accel_odr(&s_dev, QMI8658_ACCEL_ODR_62_5HZ);
    qmi8658_set_accel_unit_mps2(&s_dev, true);

    // CTRL7 bit0 = accel on, sync-sample off. Set explicitly and left alone for
    // the life of the firmware: the only thing that ever took the sensor out of
    // this mode was WoM arming, and that is gone. Continuous sampling is now the
    // ONLY mode, which is what lets rr_steps count while the screen is off.
    esp_err_t ctrl7 = qmi8658_write_register(&s_dev, REG_CTRL7, 0x01);
    if (ctrl7 != ESP_OK) {
        ESP_LOGE(TAG, "could not enable the accelerometer: %s", esp_err_to_name(ctrl7));
        return ctrl7;
    }

    s_ready = true;

    // Wait for the FIRST conversion before judging anything. Straight after a
    // soft reset the output registers are genuinely all-zero for a few
    // milliseconds, and reading them there produced a |a| = 0.00 g "SAMPLES ARE
    // NOT VALID" warning on every boot of a perfectly healthy sensor. A check
    // that cries wolf is worse than no check, because the next real one gets
    // ignored.
    // ⚠️ Poll the DATA, not STATUS0. Reading STATUS0 CLEARS its data-ready bit,
    // so a wait loop built on it consumes the very flag the next reader needs —
    // which is how this check managed to still report an invalid first sample
    // after being "fixed" once already.
    //
    // Before the first conversion the registers read 0x8000 on every axis
    // (-32768), which scales to exactly -2.00 g at 2G. That is the sentinel, not
    // a measurement — see the note at the top of this file.
    float x = 0, y = 0, z = 0;
    for (int i = 0; i < 40; i++) {         // ~200 ms budget at 62.5 Hz
        if (rr_imu_read_accel_g(&x, &y, &z) == ESP_OK) {
            const float m = sqrtf(x * x + y * y + z * z);
            if (m > 0.5f && m < 1.5f) break;   // a real gravity vector
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (rr_imu_read_accel_g(&x, &y, &z) == ESP_OK) {
        const float mag = sqrtf(x * x + y * y + z * z);
        ESP_LOGI(TAG, "IMU ready — x=%.2f y=%.2f z=%.2f |a|=%.2f g%s",
                 (double) x, (double) y, (double) z, (double) mag,
                 (mag < 0.80f || mag > 1.20f)
                     ? "  ⚠ |a| IS NOT ~1 g — SAMPLES ARE NOT VALID"
                     : "");
    } else {
        ESP_LOGE(TAG, "IMU ready but the first accelerometer read FAILED");
    }

    // ── Is it actually CONVERTING? ──────────────────────────────────────────
    //
    // A plausible-looking magnitude is not proof of life: the registers can hold
    // one valid-looking sample forever. Dump the control state and take a few
    // samples spaced well apart — if the raw counts are byte-identical across
    // 500 ms, the part is not sampling, whatever the registers claim.
    {
        uint8_t c1 = 0, c2 = 0, c7 = 0, c8 = 0, st0 = 0;
        qmi8658_read_register(&s_dev, REG_CTRL1, &c1, 1);
        qmi8658_read_register(&s_dev, REG_CTRL2, &c2, 1);
        qmi8658_read_register(&s_dev, REG_CTRL7, &c7, 1);
        qmi8658_read_register(&s_dev, REG_CTRL8, &c8, 1);
        qmi8658_read_register(&s_dev, REG_STATUS0, &st0, 1);
        ESP_LOGI(TAG, "  CTRL1=0x%02X CTRL2=0x%02X CTRL7=0x%02X CTRL8=0x%02X STATUS0=0x%02X",
                 c1, c2, c7, c8, st0);

        int16_t prev_ax = 0;
        bool changed = false;
        for (int i = 0; i < 5; i++) {
            uint8_t r[6] = { 0 };
            qmi8658_read_register(&s_dev, REG_AX_L, r, sizeof(r));
            const int16_t ax = (int16_t) ((r[1] << 8) | r[0]);
            const int16_t ay = (int16_t) ((r[3] << 8) | r[2]);
            const int16_t az = (int16_t) ((r[5] << 8) | r[4]);
            uint8_t s0 = 0;
            qmi8658_read_register(&s_dev, REG_STATUS0, &s0, 1);
            ESP_LOGI(TAG, "  sample %d: ax=%6d ay=%6d az=%6d  STATUS0=0x%02X",
                     i, ax, ay, az, s0);
            if (i > 0 && ax != prev_ax) changed = true;
            prev_ax = ax;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!changed) {
            ESP_LOGE(TAG, "  ⚠ RAW COUNTS IDENTICAL ACROSS 500 ms — THE ACCELEROMETER "
                          "IS NOT CONVERTING. Step counting cannot work in this state.");
        }
    }
    return ESP_OK;
}

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
