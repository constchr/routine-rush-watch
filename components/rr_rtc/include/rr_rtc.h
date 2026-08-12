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

/** Current time as a Unix epoch (UTC). 0 if the RTC cannot be read. */
uint32_t rr_rtc_get_epoch(void);

/** Format as an ISO-8601 UTC instant, "YYYY-MM-DDTHH:MM:SSZ" (needs >= 21). */
void rr_rtc_format_iso(const rr_rtc_time_t *t, char *buf, size_t buflen);

/** ISO-8601 for an epoch captured earlier this run. */
void rr_rtc_epoch_to_iso(uint32_t epoch, char *buf, size_t buflen);

// ── Local time (UTC offset) ─────────────────────────────────────────────────
//
// THE RTC HOLDS UTC. ALWAYS. The offset is applied on the way to the SCREEN
// and nowhere else.
//
// This is the whole design decision, so it is worth stating why:
//
//   • Run records go to Supabase as ISO-8601 "Z" instants. They are produced
//     by rr_rtc_get_epoch() / rr_rtc_epoch_to_iso(), which read the RTC
//     directly. If the RTC held local time those would silently be wrong by
//     the offset — every completed routine misfiled by three hours, and
//     invisibly, because the string still ends in "Z".
//   • DST then costs nothing: the offset changes, the RTC does not. Storing
//     local time would mean REWRITING THE CLOCK twice a year, from a phone
//     that may not be in range on the morning it matters.
//   • A stale offset is a cosmetic one-hour error on the face that the next
//     sync repairs. A wrong RTC is corrupted history that nothing repairs.
//
// The offset is PERSISTED IN NVS, so a watch that reboots on a wrist with no
// phone nearby still shows the right local time.

/**
 * Set the UTC offset — total seconds to ADD to UTC for local wall-clock,
 * signed. Persists to NVS. This is the RR_CONTROL `set_tz` payload.
 */
esp_err_t rr_rtc_set_utc_offset(int32_t offset_s);

/** Current UTC offset in seconds (0 until a phone has ever sent one). */
int32_t rr_rtc_get_utc_offset(void);

/** True once an offset has actually been received (0 is a legal offset — UTC). */
bool rr_rtc_has_utc_offset(void);

/**
 * Read the RTC as LOCAL wall-clock: UTC plus the stored offset.
 *
 * Use this for ANYTHING A CHILD READS — the watch face clock, the date, and
 * the schedule comparison (schedules are authored as local "HH:MM"). Use
 * rr_rtc_get() when you need the canonical UTC instant, which in practice
 * means run records.
 */
esp_err_t rr_rtc_get_local(rr_rtc_time_t *out);

// ── Alarm (Phase 7) ─────────────────────────────────────────────────────────
//
// ⚠️ THE ALARM WORKS. THE INTERRUPT PIN GOES NOWHERE.
//
// The PCF85063's alarm comparator is real and verified on hardware: arm it and
// the AF flag in Control_2 sets on the exact second, every time (see
// rr_rtc_alarm_selftest, run at boot). What does NOT exist on this board is a
// path from the chip's /INT pin to the ESP32-C6.
//
// From the board schematic (V1.0), net RTC_INT has exactly TWO endpoints:
//   U6 pin 4 (/INT)  →  pad P4
// and that is all. Every OTHER interrupt line on this board has a third
// endpoint at the MCU symbol — QMI_INT1→GPIO16, QMI_INT2→GPIO17,
// TP_INT→GPIO15 — and the MCU's own net list carries only RTC_SCL (GPIO7) and
// RTC_SDA (GPIO8) in its "RTC" column. There is no GPIO to attach an ISR to.
// rr_rtc_alarm_selftest() confirms this electrically as well: GPIO14, the one
// pin the board leaves unrouted, does not move when the alarm asserts.
//
// CONSEQUENCE FOR THE SCHEDULER: the RTC cannot wake the CPU on this board.
// rr_sched therefore times its own wake with the C6's LP timer and uses this
// RTC purely as the authoritative wall clock (which is what it is good at — it
// is battery-backed and survives reboots). See rr_sched.c for that design.
//
// The alarm API below is kept, armed and verified because it costs nothing and
// it is the correct mechanism the moment the /INT pad is bridged to a free
// GPIO — on a reworked board, or a future revision. It is NOT load-bearing
// today, and nothing in rr_sched depends on it firing.

/** True if this board routes the RTC /INT pin to a GPIO. It does not. */
#define RR_RTC_ALARM_INT_WIRED 0

/**
 * Arm the alarm at an absolute UTC instant, to the second.
 *
 * The PCF85063 matches on second/minute/hour/day-of-month only — there is no
 * month or year field — so the furthest a single arming reaches is ~one month.
 * Every scheduler fire is at most 7 days out, well inside that.
 */
esp_err_t rr_rtc_alarm_at(uint32_t epoch_utc);

/** Disable the alarm and clear a pending flag. */
esp_err_t rr_rtc_alarm_cancel(void);

/** Read (and optionally clear) AF — the alarm-fired flag in Control_2. */
esp_err_t rr_rtc_alarm_pending(bool *out_fired, bool clear);

/**
 * Prove the alarm actually fires, on hardware, at boot.
 *
 * Arms an alarm `seconds_ahead` out, watches AF over I2C, and reports the
 * error between the expected and observed second. Also samples GPIO14 — the
 * only pin this board leaves unrouted — across the event, so the "is /INT
 * reachable from the CPU" question is answered by measurement and not only by
 * reading the schematic.
 *
 * This exists because the QMI8658 wake-on-motion helper staged a threshold and
 * never issued the CTRL9 command that commits it: the sensor read perfectly
 * over I2C while the interrupt silently never fired. An alarm that is assumed
 * to work is the same bug waiting to happen, so it is measured instead.
 */
esp_err_t rr_rtc_alarm_selftest(int seconds_ahead);
