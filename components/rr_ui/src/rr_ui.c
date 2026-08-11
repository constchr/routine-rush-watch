// rr_ui — LVGL bring-up + pairing QR screen.
//
// PHASE 2 SCOPE: display init and the pairing screen only. No watch face, no
// routine screens.
//
// ⚠️ PARTIAL RENDER. The BSP is configured (sdkconfig.defaults:
// CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT=50) to render in 50-line bands rather
// than a full framebuffer. A full 410x502 16bpp frame is ~400 KiB and there is
// no PSRAM — it does not fit. Nothing in this file may allocate a full-screen
// buffer.
//
// The QR widget is affordable despite its size because lv_qrcode's canvas is
// LV_COLOR_FORMAT_I1 (1 bit per pixel): a 320x320 code is ~12.8 KiB, not the
// ~205 KiB a 16bpp canvas would need.

#include "rr_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "lvgl.h"

#include "rr_fonts.h"
#include "rr_store.h"
#include "emoji_manifest.h"

static const char *TAG = "rr_ui";

// The panel is 410x502. Keep the QR comfortably inside the narrow axis so the
// quiet zone survives — scanners need the light margin around the symbol.
#define QR_SIZE 320

static lv_obj_t *s_screen;

// Last non-transient screen, so an aborted reset countdown can restore exactly
// what the watch was showing instead of guessing at pairing state.
typedef enum { RR_SCREEN_NONE, RR_SCREEN_QR, RR_SCREEN_PAIRED, RR_SCREEN_WAITING } rr_screen_t;
static rr_screen_t s_last_screen;
static char s_last_qr_payload[128];

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "heap %s: free=%u largest_block=%u",
             when,
             (unsigned) esp_get_free_heap_size(),
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

esp_err_t rr_ui_init(void)
{
    log_heap("before LVGL init");

    // bsp_display_start() brings up the SH8601 panel over QSPI, the FT3168
    // touch controller, and the LVGL port task. It also calls bsp_i2c_init()
    // internally, which is idempotent.
    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start() failed");
        return ESP_FAIL;
    }
    bsp_display_backlight_on();

    log_heap("after LVGL init");

    // Prove the render path with a trivial label before anything complex is
    // put on screen. If this does not appear, the problem is the display
    // pipeline, not the QR encoder.
    bsp_display_lock(0);
    s_screen = lv_screen_active();
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(s_screen);
    lv_label_set_text(label, "Routine Rush\nstarting...");
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(label);
    bsp_display_unlock();

    ESP_LOGI(TAG, "LVGL up — boot label rendered");
    return ESP_OK;
}

esp_err_t rr_ui_show_pairing_qr(const char *payload)
{
    if (payload == NULL) return ESP_ERR_INVALID_ARG;

    log_heap("before QR render");

    bsp_display_lock(0);

    lv_obj_clean(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_white(), LV_PART_MAIN);

    lv_obj_t *qr = lv_qrcode_create(s_screen);
    lv_qrcode_set_size(qr, QR_SIZE);
    // Dark-on-light, the orientation every scanner expects. Inverting these
    // produces a symbol most phones silently refuse to decode.
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());

    lv_result_t res = lv_qrcode_update(qr, payload, strlen(payload));
    if (res != LV_RESULT_OK) {
        bsp_display_unlock();
        ESP_LOGE(TAG, "lv_qrcode_update failed (payload %u bytes) — too long, or out of memory",
                 (unsigned) strlen(payload));
        return ESP_FAIL;
    }

    lv_obj_align(qr, LV_ALIGN_CENTER, 0, -20);
    // A white border around the symbol IS the quiet zone. Without it the QR
    // runs to the edge of its canvas and scan reliability drops sharply.
    lv_obj_set_style_border_color(qr, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(qr, 8, LV_PART_MAIN);

    lv_obj_t *hint = lv_label_create(s_screen);
    lv_label_set_text(hint, "Scan to pair");
    lv_obj_set_style_text_color(hint, lv_color_black(), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);

    bsp_display_unlock();

    log_heap("after QR render");
    ESP_LOGI(TAG, "pairing QR rendered (%u bytes, %dpx)", (unsigned) strlen(payload), QR_SIZE);
    s_last_screen = RR_SCREEN_QR;
    strlcpy(s_last_qr_payload, payload, sizeof(s_last_qr_payload));
    return ESP_OK;
}

esp_err_t rr_ui_show_paired_status(void)
{
    // Called from the NimBLE host task on a successful ROUTINE_PUSH, so it must
    // take the LVGL lock like any other non-LVGL-task caller.
    bsp_display_lock(0);

    lv_obj_clean(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);

    lv_obj_t *tick = lv_label_create(s_screen);
    lv_label_set_text(tick, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(tick, lv_color_hex(0x10B981), LV_PART_MAIN);
    lv_obj_align(tick, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "Paired");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *sub = lv_label_create(s_screen);
    lv_label_set_text(sub, "Routines synced");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x8E8E93), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 24);

    bsp_display_unlock();

    ESP_LOGI(TAG, "paired status screen shown");
    s_last_screen = RR_SCREEN_PAIRED;
    return ESP_OK;
}

esp_err_t rr_ui_show_waiting_status(void)
{
    bsp_display_lock(0);

    lv_obj_clean(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);

    lv_obj_t *l = lv_label_create(s_screen);
    lv_label_set_text(l, "Paired\nwaiting for routines");
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(l);

    bsp_display_unlock();

    ESP_LOGI(TAG, "waiting-for-routines screen shown");
    s_last_screen = RR_SCREEN_WAITING;
    return ESP_OK;
}

// ── Factory-reset countdown ──────────────────────────────────────────────────
// Remembers what was on screen so an aborted hold restores it rather than
// leaving the watch on a stale warning.
static bool s_reset_prompt_active;

esp_err_t rr_ui_show_reset_countdown(int seconds_remaining)
{
    bsp_display_lock(0);
    lv_obj_clean(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x7F1D1D), LV_PART_MAIN);

    lv_obj_t *t = lv_label_create(s_screen);
    lv_label_set_text(t, "Factory reset");
    lv_obj_set_style_text_color(t, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -70);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", seconds_remaining);
    lv_obj_t *n = lv_label_create(s_screen);
    lv_label_set_text(n, buf);
    lv_obj_set_style_text_color(n, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(n, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_center(n);

    lv_obj_t *h = lv_label_create(s_screen);
    lv_label_set_text(h, "Release to cancel");
    lv_obj_set_style_text_color(h, lv_color_hex(0xFCA5A5), LV_PART_MAIN);
    lv_obj_align(h, LV_ALIGN_CENTER, 0, 70);

    bsp_display_unlock();
    s_reset_prompt_active = true;
    return ESP_OK;
}

esp_err_t rr_ui_show_last_status(void)
{
    switch (s_last_screen) {
    case RR_SCREEN_QR:      return rr_ui_show_pairing_qr(s_last_qr_payload);
    case RR_SCREEN_PAIRED:  return rr_ui_show_paired_status();
    case RR_SCREEN_WAITING: return rr_ui_show_waiting_status();
    default:                return ESP_OK;
    }
}

esp_err_t rr_ui_restore_after_reset_prompt(void)
{
    if (!s_reset_prompt_active) return ESP_OK;
    s_reset_prompt_active = false;
    return rr_ui_show_last_status();
}

// ═════════════════════════════════════════════════════════════════════════════
// Phase 4 — routine step screen (§8 screen 2, static: no timer yet)
//
// Single-step-focus: one step on screen, large enough for a child to read at
// arm's length. Kids-app palette (§9), not the parent monochrome.
// ═════════════════════════════════════════════════════════════════════════════

#define RR_BG      lv_color_hex(0x0D1117)
#define RR_TEXT    lv_color_hex(0xF0F6FC)
#define RR_MUTED   lv_color_hex(0x8B949E)
#define RR_ACCENT  lv_color_hex(0xFF6B35)
#define RR_SURFACE lv_color_hex(0x161B22)
#define RR_SUCCESS lv_color_hex(0x3FB950)
#define RR_DANGER  lv_color_hex(0xFF453A)

// LVGL addresses files as "<LETTER>:<path>". The emoji manifest is generated in
// the app repo and emits bare POSIX paths ("/lfs/emoji/x.bin"), so we prepend
// the drive letter here rather than editing a generated file. The POSIX driver
// is registered with an EMPTY working directory (sdkconfig.defaults), so the
// remainder passes through to open() unchanged.
static bool to_lv_path(const char *posix_path, char *out, size_t cap)
{
    if (posix_path == NULL) return false;
    int n = snprintf(out, cap, "%c:%s", (char) CONFIG_LV_FS_POSIX_LETTER, posix_path);
    return n > 0 && (size_t) n < cap;
}

esp_err_t rr_ui_font_selftest(void)
{
    bsp_display_lock(0);
    lv_obj_clean(s_screen);
    lv_obj_set_style_bg_color(s_screen, RR_BG, LV_PART_MAIN);

    // The gate: Greek and Latin in the SAME font, both legible, no boxes.
    lv_obj_t *el = lv_label_create(s_screen);
    lv_label_set_text(el, "Πρωινή ρουτίνα");
    lv_obj_set_style_text_font(el, &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(el, RR_TEXT, LV_PART_MAIN);
    lv_obj_align(el, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *en = lv_label_create(s_screen);
    lv_label_set_text(en, "Morning routine");
    lv_obj_set_style_text_font(en, &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(en, RR_ACCENT, LV_PART_MAIN);
    lv_obj_align(en, LV_ALIGN_CENTER, 0, 0);

    // Mixed in one string — the real case, since a bilingual family's routine
    // names and step labels are not guaranteed to be single-script.
    lv_obj_t *mix = lv_label_create(s_screen);
    lv_label_set_text(mix, "Βούρτσισμα · brush · 1/3");
    lv_obj_set_style_text_font(mix, &rr_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(mix, RR_MUTED, LV_PART_MAIN);
    lv_obj_align(mix, LV_ALIGN_CENTER, 0, 60);

    bsp_display_unlock();
    log_heap("after font selftest");
    ESP_LOGI(TAG, "font selftest rendered (Greek + Latin, rr_font_36 / rr_font_20)");
    return ESP_OK;
}

// Handles retained so the 1 Hz tick can update the ring and clock WITHOUT
// rebuilding the screen. Rebuilding every second would re-read the emoji from
// flash and thrash the heap; updating two widgets invalidates only their own
// areas, so the emoji is never redrawn while the step runs.
static lv_obj_t *s_arc;
static lv_obj_t *s_mmss;

static void mmss_str(int secs, char *out, size_t cap)
{
    if (secs < 0) secs = 0;
    snprintf(out, cap, "%d:%02d", secs / 60, secs % 60);
}

static void done_event_cb(lv_event_t *e)
{
    rr_ui_step_cb_t cb = (rr_ui_step_cb_t) lv_event_get_user_data(e);
    if (cb) cb();
}

esp_err_t rr_ui_show_step(const rr_step_view_t *v,
                          rr_ui_step_cb_t on_done,
                          rr_ui_step_cb_t on_skip)
{
    if (v == NULL) return ESP_ERR_INVALID_ARG;

    bsp_display_lock(0);
    lv_obj_clean(s_screen);
    s_arc = NULL;
    s_mmss = NULL;
    lv_obj_set_style_bg_color(s_screen, RR_BG, LV_PART_MAIN);

    const bool timed = v->time_limit_s > 0;

    // ── position "1 / 3" ────────────────────────────────────────────────────
    char pos[24];
    snprintf(pos, sizeof(pos), "%d / %d", v->position + 1, v->step_count);
    lv_obj_t *lpos = lv_label_create(s_screen);
    lv_label_set_text(lpos, pos);
    lv_obj_set_style_text_font(lpos, &rr_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lpos, RR_MUTED, LV_PART_MAIN);
    lv_obj_align(lpos, LV_ALIGN_TOP_MID, 0, 12);

    // ── emoji, 96px, streamed from littlefs ─────────────────────────────────
    char lvpath[96];
    const char *posix_path = rr_emoji_path(v->emoji);
    bool drew_emoji = false;
    if (posix_path != NULL && to_lv_path(posix_path, lvpath, sizeof(lvpath))) {
        lv_obj_t *img = lv_image_create(s_screen);
        lv_image_set_src(img, lvpath);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 40);
        drew_emoji = true;
    } else {
        lv_obj_t *dot = lv_obj_create(s_screen);
        lv_obj_set_size(dot, 96, 96);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, RR_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 40);
        ESP_LOGW(TAG, "emoji '%s' unmapped — fallback dot", v->emoji);
    }

    // ── step label ──────────────────────────────────────────────────────────
    lv_obj_t *llabel = lv_label_create(s_screen);
    lv_label_set_long_mode(llabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(llabel, 370);
    lv_label_set_text(llabel, v->label);
    lv_obj_set_style_text_font(llabel, &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(llabel, RR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_align(llabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(llabel, LV_ALIGN_TOP_MID, 0, 148);

    // ── countdown ring — only for TIMED steps ───────────────────────────────
    // §8 screen 3: an untimed step has no ring at all. Showing an empty or
    // full ring that never moves would imply a timer that is broken.
    if (timed) {
        s_arc = lv_arc_create(s_screen);
        lv_obj_set_size(s_arc, 150, 150);
        lv_arc_set_rotation(s_arc, 270);
        lv_arc_set_bg_angles(s_arc, 0, 360);
        lv_arc_set_range(s_arc, 0, 1000);      // finer than seconds, so the
        lv_arc_set_value(s_arc, 1000);         // sweep looks continuous
        lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(s_arc, 12, LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_arc, 12, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_arc, RR_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_arc, RR_ACCENT, LV_PART_INDICATOR);
        lv_obj_align(s_arc, LV_ALIGN_TOP_MID, 0, 240);

        char buf[16];
        mmss_str(v->time_limit_s, buf, sizeof(buf));
        s_mmss = lv_label_create(s_screen);
        lv_label_set_text(s_mmss, buf);
        lv_obj_set_style_text_font(s_mmss, &rr_font_28, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_mmss, RR_TEXT, LV_PART_MAIN);
        lv_obj_align_to(s_mmss, s_arc, LV_ALIGN_CENTER, 0, 0);
    }

    // ── Done: the primary action, deliberately the biggest thing on screen ──
    lv_obj_t *bdone = lv_button_create(s_screen);
    lv_obj_set_size(bdone, 300, 68);
    lv_obj_align(bdone, LV_ALIGN_BOTTOM_MID, 0, -62);
    lv_obj_set_style_bg_color(bdone, RR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_radius(bdone, 34, LV_PART_MAIN);
    lv_obj_add_event_cb(bdone, done_event_cb, LV_EVENT_CLICKED, (void *) on_done);
    lv_obj_t *ldone = lv_label_create(bdone);
    lv_label_set_text(ldone, "OK");
    lv_obj_set_style_text_font(ldone, &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(ldone, RR_BG, LV_PART_MAIN);
    lv_obj_center(ldone);

    // ── Skip: present but visually quiet, so it is not the obvious tap ──────
    lv_obj_t *bskip = lv_button_create(s_screen);
    lv_obj_set_size(bskip, 140, 40);
    lv_obj_align(bskip, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_color(bskip, RR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(bskip, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(bskip, RR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_radius(bskip, 20, LV_PART_MAIN);
    lv_obj_add_event_cb(bskip, done_event_cb, LV_EVENT_CLICKED, (void *) on_skip);
    lv_obj_t *lskip = lv_label_create(bskip);
    lv_label_set_text(lskip, "Skip");
    lv_obj_set_style_text_font(lskip, &rr_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lskip, RR_MUTED, LV_PART_MAIN);
    lv_obj_center(lskip);

    bsp_display_unlock();

    ESP_LOGI(TAG, "step [%d/%d] '%s' %s limit=%ds emoji=%s",
             v->position + 1, v->step_count, v->label, v->emoji,
             v->time_limit_s, drew_emoji ? "ok" : "FALLBACK");
    s_last_screen = RR_SCREEN_NONE;
    return ESP_OK;
}

void rr_ui_set_countdown(int remaining_s, int total_s)
{
    if (s_arc == NULL || s_mmss == NULL || total_s <= 0) return;

    bsp_display_lock(0);   // recursive mutex: safe from an lv_timer callback

    int val = (int) ((int64_t) remaining_s * 1000 / total_s);
    if (val < 0) val = 0;
    lv_arc_set_value(s_arc, val);

    char buf[16];
    mmss_str(remaining_s, buf, sizeof(buf));
    lv_label_set_text(s_mmss, buf);

    // At zero the step STAYS ACTIVE (§8) — the colour change is the only
    // signal, deliberately: auto-advancing would take the decision away from
    // a child who is mid-task.
    if (remaining_s <= 0) {
        lv_obj_set_style_arc_color(s_arc, RR_DANGER, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_mmss, RR_DANGER, LV_PART_MAIN);
    }

    bsp_display_unlock();
}

esp_err_t rr_ui_show_routine_complete(const char *routine_name, int done, int skipped)
{
    bsp_display_lock(0);
    lv_obj_clean(s_screen);
    s_arc = NULL;
    s_mmss = NULL;
    lv_obj_set_style_bg_color(s_screen, RR_BG, LV_PART_MAIN);

    lv_obj_t *tick = lv_label_create(s_screen);
    lv_label_set_text(tick, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(tick, &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(tick, RR_SUCCESS, LV_PART_MAIN);
    lv_obj_align(tick, LV_ALIGN_CENTER, 0, -110);

    // Greek, because the cached routine is Greek and this proves the font on a
    // second screen. child.language is not cached yet (Phase 5), so this is
    // not yet locale-driven — noted in rr_routine.h.
    lv_obj_t *big = lv_label_create(s_screen);
    lv_label_set_text(big, "Τέλος!");
    lv_obj_set_style_text_font(big, &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(big, RR_TEXT, LV_PART_MAIN);
    lv_obj_align(big, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t *name = lv_label_create(s_screen);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(name, 360);
    lv_label_set_text(name, routine_name ? routine_name : "");
    lv_obj_set_style_text_font(name, &rr_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, RR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, 10);

    char summary[48];
    snprintf(summary, sizeof(summary), "%d done  ·  %d skipped", done, skipped);
    lv_obj_t *sum = lv_label_create(s_screen);
    lv_label_set_text(sum, summary);
    lv_obj_set_style_text_font(sum, &rr_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(sum, RR_MUTED, LV_PART_MAIN);
    lv_obj_align(sum, LV_ALIGN_CENTER, 0, 70);

    bsp_display_unlock();

    ESP_LOGI(TAG, "routine complete: %d done, %d skipped", done, skipped);
    return ESP_OK;
}
