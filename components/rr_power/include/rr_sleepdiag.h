#pragma once
// rr_sleepdiag — WHO IS HOLDING THE CPU AWAKE? A bench diagnostic, not a feature.
//
// ── THE QUESTION ────────────────────────────────────────────────────────────
//
// idle-asleep measured 33.1 mA with `ls OFF` (docs/POWER.md). Light sleep is the
// only lever big enough to matter at that scale, and it is off because enabling
// it starved the IDLE and main tasks. The leading explanation was "LVGL's tick
// timer thrashes against the sleep entry/exit overhead" — never confirmed, and
// POWER.md records an observation against it.
//
// ── WHY THIS CAN RUN ON THE BENCH, PLUGGED IN, WITH LIGHT SLEEP OFF ─────────
//
// Because the thing that decides whether light sleep can engage is not a power
// measurement, it is a SCHEDULE. From IDF's own vApplicationSleep()
// (components/esp_pm/pm_impl.c):
//
//     sleep_time_us = MIN(portTICK_PERIOD_MS * 1000 * xExpectedIdleTime,
//                         esp_timer_get_next_alarm_for_wake_up() - now);
//     if (sleep_time_us >= configEXPECTED_IDLE_TIME_BEFORE_SLEEP
//                          * portTICK_PERIOD_MS * 1000) { ...sleep... }
//
// With CONFIG_FREERTOS_HZ=1000 and CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3 that
// threshold is 3000 us. So the gate is: IS THE NEXT PERIODIC WAKE MORE THAN 3 ms
// AWAY? That is a property of which timers exist and how fast they tick — and it
// is identical whether or not light sleep is enabled, and whether or not USB is
// attached.
//
// So the discriminating measurement is: enumerate every periodic wake source and
// its ACTUAL frequency, and compare each period against the 3 ms gate. No
// battery run, no unplugging, no 2-hour wait.
//
// ── HOW IT MEASURES, RATHER THAN ASSUMES ────────────────────────────────────
//
// esp_timer_dump() with CONFIG_ESP_TIMER_PROFILING lists every timer with its
// period and its trigger count. Dumping TWICE and differencing the counts gives
// each timer's real firing rate over a known interval — so a configured period
// that is not what the code actually does shows up as a rate that disagrees with
// it. That is the difference between reading `timer_period_ms = 5` in a header
// and knowing the thing fires 200 times a second on this board.
//
// Build (one line):
//   idf.py -DRR_SLEEPDIAG=1
//          -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.diag"
//          -B build-diag build
//
// sdkconfig.diag adds only CONFIG_ESP_TIMER_PROFILING and CONFIG_PM_PROFILING —
// both diagnostic-only, and deliberately NOT in the committed defaults.

#include "esp_err.h"

/**
 * Start the diagnostic. Prints a census `first_delay_s` after boot (late enough
 * that the screen has blanked and boot-time timers have settled) and again
 * `interval_s` later, then reports each timer's measured rate and verdict
 * against the 3 ms gate.
 *
 * Compiled in only under -DRR_SLEEPDIAG=1. Does nothing otherwise.
 */
esp_err_t rr_sleepdiag_start(int first_delay_s, int interval_s);
