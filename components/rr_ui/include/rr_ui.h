#pragma once
// rr_ui — LVGL screens + idle watch face. Phase 4/6.
//
// ⚠️ PARTIAL RENDER ONLY. No PSRAM; ~274 KiB free at boot. A full 410x502
// 16bpp framebuffer is ~400 KiB — larger than all available RAM. Buffers are
// configured to 50-line bands in sdkconfig.defaults; do not raise that without
// a heap measurement.
//
// Greek (el) is a first-class locale — every string goes through the font set
// built for it, never a Latin-only default.
//
// Intended interface:
//
//   esp_err_t rr_ui_init(void);          // after bsp_display_start()
//   void      rr_ui_show_watchface(void);
//   void      rr_ui_show_routine_list(void);
//   void      rr_ui_show_step(const rr_step_t *step, uint32_t remaining_s);
//   void      rr_ui_show_pairing_qr(const char *payload);
//
// Watch face (§9B) is connection-independent: time from the RTC, steps from
// the IMU, next-routine from cache. It must render identically whether or not
// a phone has ever connected.

// ── Phase 2 surface (implemented) ────────────────────────────────────────────

#include "esp_err.h"

/**
 * Bring up the display (SH8601 QSPI), touch, and LVGL in partial-render mode,
 * and draw a boot label to prove the path. Logs free heap either side of LVGL
 * init — that delta is the RAM budget check.
 */
esp_err_t rr_ui_init(void);

/**
 * Replace the screen with the pairing QR encoding `payload` (the exact JSON
 * the parent app's scanner parses), plus a caption.
 */
esp_err_t rr_ui_show_pairing_qr(const char *payload);
