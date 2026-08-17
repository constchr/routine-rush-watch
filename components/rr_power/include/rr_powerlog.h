#pragma once
// rr_powerlog — SOC-drop power measurement, with no measurement hardware.
//
// ── WHY THIS EXISTS ─────────────────────────────────────────────────────────
//
// There is no way to measure current on this board. The AXP2101 exposes battery
// VOLTAGE (reg 0x34) and a fuel-gauge PERCENTAGE (reg 0xA4) — and no battery
// current ADC at all, unlike the AXP192 it replaced. So every milliamp figure in
// the Phase 10 audit is a datasheet estimate with a ±50% honesty margin, and
// shipping power changes against estimates alone means never knowing whether they
// worked.
//
// The fuel gauge does give one usable quantity: how fast the percentage falls.
// At the board's ~400 mAh cell, 1% ≈ 4 mAh, so a state drawing 10 mA loses a
// percent every ~24 minutes. Hold a state for an hour, divide, and the answer is
// a real number rather than a guess.
//
// ── WHAT IT IS NOT ──────────────────────────────────────────────────────────
//
// Coarse and slow. The gauge reports whole percent, its own coulomb counting is
// approximate, and cell capacity is nominal. Expect ±20-30% on a one-hour
// sample, better over longer holds. That is still an order of magnitude better
// than a datasheet sum, and — crucially — it is CONSISTENT, so a before/after
// comparison of the same state on the same cell is far more trustworthy than
// either absolute figure.
//
// ⚠️ IT MUST RUN ON BATTERY. With USB attached the PMIC is charging or floating
// and the percentage will not fall; rr_powerlog refuses to report a rate while
// vbus_present, rather than printing a meaningless zero.

#include <stdbool.h>
#include "esp_err.h"

/** Nominal cell capacity (MX1.25 pack, spec §2). Used only to turn %/h into mA. */
#define RR_POWERLOG_BATTERY_MAH 400

/**
 * Start periodic battery logging.
 *
 * `interval_s` between samples; 60 is a sensible default — often enough to see a
 * trend inside an hour, rare enough that the sampling itself (one I2C read) does
 * not distort the thing being measured.
 *
 * Each line carries a monotonic timestamp, mV, SOC%, and — once at least one
 * percent has been lost — the average drop rate and the implied current. Grep
 * `POWERLOG` out of a monitor capture and the last line is the answer.
 */
esp_err_t rr_powerlog_start(int interval_s);

/** Stop logging (frees nothing; the task simply idles). */
void rr_powerlog_stop(void);

/**
 * Reset the baseline to now — so the reported rate describes the state under
 * test and not the transition into it.
 *
 * ⚠️ YOU ALMOST CERTAINLY DO NOT NEED TO CALL THIS, AND FOR TWO YEARS NOBODY
 * COULD. It had no callers at all, and on this board there is no way to reach it
 * at the moment it matters: the console has no REPL, and BOOT is already wake
 * (short press) and factory reset (10 s hold). The measurement that matters runs
 * on battery, i.e. with no host attached to ask.
 *
 * So the baseline now re-arms ITSELF on the USB->battery edge: unplugging is the
 * act that starts a run, and replugging finalises it and prints the result. This
 * remains only for a future state worth measuring that is not entered by pulling
 * the cable.
 */
void rr_powerlog_mark(const char *state_label);

/** True while a measurement is running. */
bool rr_powerlog_is_running(void);
