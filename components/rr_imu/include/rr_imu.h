#pragma once
// rr_imu — QMI8658 6-axis IMU: shared bring-up.
//
// ⚠️ THIS IS THE SHARED SENSOR INIT, not wake-detection code.
//
// Two features ride this one sensor, and they must not each initialise it
// their own way:
//   • raise-to-wake  (Phase 6, here)
//   • step counting  (Phase 9, NOT built — see rr_imu_dev())
//
// Phase 0 confirmed the part on hardware: I2C 0x6B, WHO_AM_I=0x05, correct
// gravity vector, readings track real motion.
//
// ⚠️ INTERRUPT PINS. INT1 is GPIO 16 and INT2 is GPIO 17 — the exact pins the
// Waveshare examples route the console UART to. sdkconfig.defaults pins the
// console to USB-Serial-JTAG precisely so these stay free. If wake-on-motion
// ever stops firing, check that first: the failure is silent, because the IMU
// still reads perfectly over I2C while its interrupt line is owned by a UART.

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "qmi8658.h"

/** Fired from an ISR-deferred task when the IMU reports motion. */
typedef void (*rr_imu_motion_cb_t)(void);

/**
 * Bring up the QMI8658 on the shared board I2C bus and configure INT1.
 * Idempotent. Call after bsp_i2c_init().
 */
esp_err_t rr_imu_init(i2c_master_bus_handle_t bus);

/**
 * Arm hardware wake-on-motion. `threshold` is the QMI8658's own units — lower
 * is more sensitive. While armed the IMU watches for movement on its own and
 * raises INT1, so the main CPU can stay asleep (§10: the primary battery
 * saver, since polling defeats the whole point).
 */
esp_err_t rr_imu_arm_wake_on_motion(uint8_t threshold, rr_imu_motion_cb_t cb);

/** Disarm wake-on-motion (e.g. while the screen is already awake). */
esp_err_t rr_imu_disarm_wake_on_motion(void);

/** True once rr_imu_init() has succeeded. */
bool rr_imu_is_ready(void);

/**
 * The live device handle.
 *
 * PHASE 9 HOOKS HERE: the pedometer reads accelerometer samples (or enables
 * the QMI8658's on-chip step engine) through this handle rather than
 * re-initialising the sensor. Keeping one owner is why this component exists
 * separately from the wake logic in rr_power.
 */
qmi8658_dev_t *rr_imu_dev(void);
