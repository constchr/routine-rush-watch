#pragma once
// rr_rtc — PCF85063 real-time clock (I2C 0x51).
//
// Lives in rr_power because it shares the board I2C bus and the sleep/wake
// story; it is a separate translation unit so it can move to its own component
// when the scheduler (Phase 7) needs RTC alarms.
//
// Confirmed in Phase 0: the part is present at 0x51 and ticks 1 s/s reliably.
// It ships UNSET — a factory board reads 2000-01-19 with the oscillator-stop
// flag latched. TIME_SYNC over BLE is what sets it (§6B.3); there is no other
// time source, since wifi is never initialised and there is no NTP.

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/** Broken-down time as held in the PCF85063's BCD registers. */
typedef struct {
    uint16_t year;    // full year, e.g. 2026
    uint8_t  month;   // 1..12
    uint8_t  day;     // 1..31
    uint8_t  hour;    // 0..23
    uint8_t  minute;  // 0..59
    uint8_t  second;  // 0..59
    bool     osc_ok;  // false = oscillator stopped since last set; time is suspect
} rr_rtc_time_t;

/** Attach to the shared board I2C bus. Call after bsp_i2c_init(). */
esp_err_t rr_rtc_init(i2c_master_bus_handle_t bus);

/** Read the current RTC time. */
esp_err_t rr_rtc_get(rr_rtc_time_t *out);

/** Set the RTC from a Unix epoch (UTC seconds) — the TIME_SYNC payload. */
esp_err_t rr_rtc_set_epoch(uint32_t epoch_utc);

/** Format as "YYYY-MM-DD HH:MM:SS" into buf (needs >= 20 bytes). */
void rr_rtc_format(const rr_rtc_time_t *t, char *buf, size_t buflen);
