#pragma once
// rr_power — sleep, raise-to-wake, battery. Phase 10.
//
// Verified in Phase 0: AXP2101 at I2C 0x34 (chip ID 0x4A) reports battery
// voltage and percentage — e.g. 4.10 V / 82 %, with vbus_good and
// batt_present flags. Battery telemetry is real, not assumed.
//
// ⚠️ RAISE-TO-WAKE DEPENDS ON QMI8658 INT1/INT2 = GPIO 16/17. If the console
// is left on UART (the Waveshare example default) those pins are taken and
// wake-on-motion silently degrades to CPU polling — no error, just a dead
// battery budget. See sdkconfig.defaults.
//
// Intended interface:
//
//   esp_err_t rr_power_init(void);
//   uint8_t   rr_power_battery_pct(void);
//   uint16_t  rr_power_battery_mv(void);
//   bool      rr_power_is_charging(void);
//   void      rr_power_enter_idle(void);      // screen off + light sleep
//   void      rr_power_arm_raise_to_wake(void);
//
// Wifi is never initialised (§2.2) — that is the single largest saving and it
// is a build-time decision, not a runtime one.
