#pragma once
// rr_imu — QMI8658 6-axis IMU: shared bring-up.
//
// ⚠️ THIS IS THE SHARED SENSOR INIT. It is now sampling-only.
//
// ONE feature rides this sensor: step counting (rr_steps). Raise-to-wake USED to
// share it and was removed in Phase 10 by product decision — the screen is now
// woken by a short press on BOOT or by the scheduler.
//
// ⚠️ THE THING MOST LIKELY TO BITE YOU HERE: the QMI8658 KEEPS ITS CONFIGURATION
// ACROSS AN MCU RESET. Left in a non-converting state by an earlier firmware, it
// stays there through every reflash, reporting 0x8000 on every axis — which
// scales to a plausible-looking saturated -2.00 g while STATUS0 never signals
// data-ready. rr_imu_init() soft-resets the part for exactly this reason. Do not
// remove that, and do not trust control registers as evidence the sensor is
// alive; only changing values are evidence.
//
// Phase 0 confirmed the part on hardware: I2C 0x6B, WHO_AM_I=0x05, correct
// gravity vector, readings track real motion. rr_imu_init() now re-checks that
// on every boot and shouts if |a| is not ~1 g.
//
// ⚠️ INTERRUPT PINS. INT1 is GPIO 16 and INT2 is GPIO 17 — the exact pins the
// Waveshare examples route the console UART to. Nothing uses those pins any
// more, but sdkconfig.defaults still keeps the console off them: if an
// interrupt feature is ever wanted again, a console sitting on those pins makes
// it silently impossible, and that warning is cheaper than rediscovering it.

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "qmi8658.h"

/**
 * Bring up the QMI8658 on the shared board I2C bus and start the accelerometer.
 * Idempotent. Call after bsp_i2c_init().
 */
esp_err_t rr_imu_init(i2c_master_bus_handle_t bus);

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

// ── The ON-CHIP step engine: PRESENT, CONFIGURABLE, AND INERT ───────────────
//
// ⚠️ DO NOT BUILD ON THIS. It does not count. Measured, not assumed.
//
// The QMI8658 documents an embedded pedometer, and everything about bringing it
// up succeeds: both CTRL9 CONFIGURE_PEDOMETER commands complete the full
// handshake, CTRL8 bit4 reads back set, and the accelerometer streams valid
// data the whole time. The step counter at 0x5A..0x5C never leaves zero.
//
// Evidence (Phase 9, on this board, WHO_AM_I=0x05 REVISION=0x7C):
//   • ~2 g of sustained shaking for 28 s, logged sample by sample from the
//     AX..AZ registers so the motion is not in question — step count 0.
//   • Repeated with the LOOSEST parameters the engine accepts (peak-to-peak
//     80 mg, peak 40 mg, entry count 1, i.e. a single peak is a step) — 0.
//   • Config verified applied by register read-back, not by return codes.
//
// So this is the THIRD vendor feature on this board that reads healthy over I2C
// and does nothing: the wake-on-motion helper that never issued CTRL9 (Phase 6),
// the RTC alarm pin that is not routed to the MCU (Phase 7), and now this. The
// pattern is worth internalising — on this hardware, a feature is not real until
// it has been observed working.
//
// The code below is KEPT deliberately, unused, because it is a correct
// implementation of a documented feature: it costs nothing, it records what was
// tried, and it is what would run on a part where the engine works. What
// actually counts steps is the software detector in rr_steps, over
// rr_imu_read_accel_g() — see the power note in rr_steps.h.
//
// ⚠️ ODR COUPLING (applies to the software detector too). Step thresholds are
// meaningless without a known range and sample rate, so the accelerometer is
// pinned for as long as anything is counting — see rr_imu_step_sampling_hold().

/** The ODR the pedometer parameters are computed against. */
#define RR_IMU_PED_ODR_HZ 62.5f

/**
 * Configure and start the on-chip step engine. INERT ON THIS PART (see above) —
 * retained as a documented, working-if-the-silicon-cooperated implementation.
 */
esp_err_t rr_imu_pedometer_enable(void);

esp_err_t rr_imu_pedometer_disable(void);

/** Read the engine's 24-bit cumulative counter (volatile — resets on power-up). */
esp_err_t rr_imu_pedometer_read(uint32_t *out_steps);

/** Zero the engine's counter. */
esp_err_t rr_imu_pedometer_reset(void);

bool rr_imu_pedometer_is_active(void);

/**
 * Prove (or disprove) the on-chip engine on hardware.
 *
 * Samples the counter once a second alongside the accelerometer, and reports
 * BOTH — because "count is 0" and "nobody moved the watch" are otherwise
 * indistinguishable, which cost three inconclusive test rounds before the log
 * started carrying the motion alongside the count. This is what established
 * that the engine is inert.
 */
esp_err_t rr_imu_pedometer_selftest(int seconds);

// ── Raw accelerometer + the sampling hold (software pedometer) ──────────────

/** One atomic 3-axis sample, in g. 2G scale — see rr_imu_step_sampling_hold(). */
esp_err_t rr_imu_read_accel_g(float *x, float *y, float *z);

/**
 * Pin the accelerometer to 2G / RR_IMU_PED_ODR_HZ for as long as something is
 * counting steps from it.
 *
 * Historically the range and rate followed the WAKE STATE — 2G/21 Hz low-power
 * while wake-on-motion was armed, 8G/500 Hz once the screen was up — so a step
 * detector whose thresholds are in g silently changed meaning every time the
 * screen slept. Wake-on-motion is gone and nothing changes the mode any more,
 * so this is now belt-and-braces rather than load-bearing. rr_steps still holds
 * it for the lifetime of the firmware, because the alternative is a detector
 * whose correctness depends on nobody ever touching CTRL2 again.
 */
void rr_imu_step_sampling_hold(bool hold);
