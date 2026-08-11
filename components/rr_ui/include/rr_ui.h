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

/** Post-pairing confirmation: paired + routines synced. */
esp_err_t rr_ui_show_paired_status(void);

/** Already paired (from NVS) but no routine cache yet — awaiting a push. */
esp_err_t rr_ui_show_waiting_status(void);

/** Red hold-to-reset countdown. Release aborts. */
esp_err_t rr_ui_show_reset_countdown(int seconds_remaining);

/** Restore whatever status screen was showing before the countdown. */
esp_err_t rr_ui_restore_after_reset_prompt(void);

/** Re-render the last status screen (paired / waiting / QR). */
esp_err_t rr_ui_show_last_status(void);

// ── Phase 4 ──────────────────────────────────────────────────────────────────

#include "rr_store.h"

/** Gate check: Greek + Latin in one font, no boxes. */
esp_err_t rr_ui_font_selftest(void);

/** Tap handler for the Done / Skip buttons. */
typedef void (*rr_ui_step_cb_t)(void);

/**
 * §8 screen 2/3: emoji + label + (ring if timed) + Done/Skip.
 * An UNTIMED step (time_limit_s <= 0) gets no ring at all.
 */
esp_err_t rr_ui_show_step(const rr_step_view_t *v,
                          rr_ui_step_cb_t on_done,
                          rr_ui_step_cb_t on_skip);

/**
 * Update the ring + mm:ss in place. Safe to call from an lv_timer callback
 * (the LVGL port mutex is recursive). Updating these two widgets invalidates
 * only their own areas, so the emoji is NOT re-read from flash each tick.
 */
void rr_ui_set_countdown(int remaining_s, int total_s);

/** §8 screen 5, minimal: a tick, the routine name, and a done/skipped count. */
esp_err_t rr_ui_show_routine_complete(const char *routine_name, int done, int skipped);
