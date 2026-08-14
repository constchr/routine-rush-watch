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
#include "lvgl.h"

// ── Curved-bezel safe zone (panel is 410x502 with ~R44 physical corners) ─────
//
// Critical UI — text, numerals, buttons — must stay inside the safe box, which
// is inset RR_SAFE_INSET on every side. The glass clips anything closer to the
// edge, and it CANNOT be seen in a simulator: it only appears on the device.
//
// Screens should not hand-place near edges. Call rr_ui_begin_screen() and add
// corner/edge content to the container it returns; hero content that may
// exceed the box (avatar, QR, ring — the centre is unobstructed) goes on
// rr_ui_screen_root().
#define RR_SCREEN_W   410
#define RR_SCREEN_H   502
#define RR_SAFE_INSET 45
#define RR_SAFE_W     (RR_SCREEN_W - 2 * RR_SAFE_INSET)   /* 320 */
#define RR_SAFE_H     (RR_SCREEN_H - 2 * RR_SAFE_INSET)   /* 412 */
#define RR_CORNER_R   44

/** Clear the screen, apply the bezel clip, and return the SAFE container. */
lv_obj_t *rr_ui_begin_screen(lv_color_t bg);

/** The full screen — for hero content allowed to exceed the safe box. */
lv_obj_t *rr_ui_screen_root(void);

/** The safe container created by the last rr_ui_begin_screen(). */
lv_obj_t *rr_ui_safe_area(void);

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

/** Re-render the last status screen (paired / waiting / QR / watch face). */
esp_err_t rr_ui_show_last_status(void);

/**
 * Register how to rebuild the watch face. rr_ui cannot do it itself — the face
 * needs the clock, battery, child record and schedule that rr_idle assembles —
 * so restoring it after a transient screen goes through this callback.
 */
void rr_ui_set_watchface_repaint(void (*fn)(void));

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

// ── Phase 6: the idle watch face (§9B) ───────────────────────────────────────

#define RR_BATT_LOW_PCT 20   /**< below this the % turns red (unless charging) */

typedef struct {
    char avatar_id[16];      /**< from the cached child record */
    uint8_t batt_percent;
    bool batt_valid;
    bool charging;
    int  hour, minute;
    int  year, month, day;
    char language[4];        /**< "el" / "en" from the cached child */
    int  steps_today;
    bool steps_valid;        /**< false until Phase 9's pedometer exists */
    int  level;              /**< 0 = hide the XP line */
    int  queued_runs;        /**< unsynced completions, shown as a small badge */
    rr_next_routine_t next;
} rr_watchface_t;

/** Render the idle face. All inputs are local — no phone required (§9B.3). */
esp_err_t rr_ui_show_watchface(const rr_watchface_t *w);

/** ISO weekday (1=Mon..7=Sun) for a civil date. */
int rr_ui_iso_weekday(int y, int m, int d);

/** True if the last screen rendered was the idle watch face. */
bool rr_ui_last_screen_is_watchface(void);

// ── Phase 7: the READY screen (§7, §8 screen 1) ──────────────────────────────

/**
 * "Ready to start": routine, step count, START and NOT NOW. No countdown.
 *
 * ONE screen for BOTH start paths — the scheduler firing and the parent app's
 * RR_CONTROL start_routine. It replaced the old alarm screen rather than
 * sitting beside it: a remote start used to drop the child straight into step
 * 1 with the clock already running, so a child who was not looking at their
 * wrist lost the first step before they saw it. Starting is now a deliberate
 * tap on either path, and there is exactly one screen to get right.
 *
 * NOTHING here starts a routine. on_start goes through rr_routine_start() at
 * the far end, and the timer begins there — not on this screen. Both callbacks
 * run on the LVGL task.
 *
 * The scheduled HH:MM the alarm screen used to show is GONE. A remote start
 * has no scheduled time to print, and one screen shape for both paths is worth
 * more than that line.
 */
esp_err_t rr_ui_show_ready(const char *routine_name, const char *routine_emoji,
                           int step_count, const char *language,
                           rr_ui_step_cb_t on_start, rr_ui_step_cb_t on_not_now);

/**
 * True while a READY offer is the screen on display.
 *
 * main.c's watch-face gate uses it to stay idempotent: the gate is polled
 * twice a second, and re-rendering READY on every poll would re-read the
 * routine emoji off littlefs at 2 Hz and eat the touch it is waiting for.
 */
bool rr_ui_last_screen_is_ready(void);

/** Take READY down and put back whatever screen it covered (face / QR / paired). */
esp_err_t rr_ui_dismiss_ready(void);
