#pragma once
// rr_steps — QMI8658 pedometer + daily count. Phase 9.
//
// IMU confirmed live in Phase 0: I2C 0x6B, WHO_AM_I=0x05, correct gravity
// vector at rest (z ≈ +9.51 m/s²), readings track real motion.
//
// Prefer the QMI8658's onboard step engine over a software peak-detector —
// it counts while the main CPU sleeps, which is the entire point.
//
// ⚠️ Depends on the IMU interrupt lines (GPIO 16/17). Verify the console is on
// USB-Serial-JTAG, not UART — see the warning in sdkconfig.defaults.
//
// Intended interface:
//
//   esp_err_t rr_steps_init(void);
//   uint32_t  rr_steps_today(void);
//   void      rr_steps_tick(void);        // called on IMU interrupt
//
// Persist the running count to NVS across sleep so a reboot mid-day doesn't
// lose it. Reset at local midnight off the RTC, not off uptime.
