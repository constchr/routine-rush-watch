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

esp_err_t rr_ui_show_step(const rr_step_view_t *v)
{
    if (v == NULL) return ESP_ERR_INVALID_ARG;

    log_heap("before step screen");

    bsp_display_lock(0);
    lv_obj_clean(s_screen);
    lv_obj_set_style_bg_color(s_screen, RR_BG, LV_PART_MAIN);

    // ── position "1 / 3" ────────────────────────────────────────────────────
    char pos[24];
    snprintf(pos, sizeof(pos), "%d / %d", v->position + 1, v->step_count);
    lv_obj_t *lpos = lv_label_create(s_screen);
    lv_label_set_text(lpos, pos);
    lv_obj_set_style_text_font(lpos, &rr_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lpos, RR_MUTED, LV_PART_MAIN);
    lv_obj_align(lpos, LV_ALIGN_TOP_MID, 0, 18);

    // ── emoji, 96px, streamed from littlefs ─────────────────────────────────
    char lvpath[96];
    const char *posix_path = rr_emoji_path(v->emoji);
    bool drew_emoji = false;
    if (posix_path != NULL && to_lv_path(posix_path, lvpath, sizeof(lvpath))) {
        lv_obj_t *img = lv_image_create(s_screen);
        lv_image_set_src(img, lvpath);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 52);
        drew_emoji = true;
        ESP_LOGI(TAG, "emoji '%s' -> %s", v->emoji, lvpath);
    } else {
        // Fallback dot (§8) — an unmapped emoji must not leave a hole.
        lv_obj_t *dot = lv_obj_create(s_screen);
        lv_obj_set_size(dot, 96, 96);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, RR_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 52);
        ESP_LOGW(TAG, "emoji '%s' unmapped — drew fallback dot", v->emoji);
    }

    // ── step label, Greek font, wraps ───────────────────────────────────────
    lv_obj_t *llabel = lv_label_create(s_screen);
    lv_label_set_long_mode(llabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(llabel, 360);
    lv_label_set_text(llabel, v->label);
    lv_obj_set_style_text_font(llabel, &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(llabel, RR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_align(llabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(llabel, LV_ALIGN_TOP_MID, 0, 168);

    // ── countdown ring — STATIC at full for now (no timer this session) ─────
    lv_obj_t *arc = lv_arc_create(s_screen);
    lv_obj_set_size(arc, 190, 190);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 100);                 // full — a fresh, unstarted step
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, RR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, RR_ACCENT, LV_PART_INDICATOR);
    lv_obj_align(arc, LV_ALIGN_BOTTOM_MID, 0, -22);

    // mm:ss in the ring centre. Untimed steps show a dash rather than 0:00,
    // which would read as "already expired".
    char mmss[16];
    if (v->time_limit_s > 0) {
        snprintf(mmss, sizeof(mmss), "%d:%02d", v->time_limit_s / 60, v->time_limit_s % 60);
    } else {
        snprintf(mmss, sizeof(mmss), "—");
    }
    lv_obj_t *lring = lv_label_create(s_screen);
    lv_label_set_text(lring, mmss);
    lv_obj_set_style_text_font(lring, &rr_font_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(lring, RR_TEXT, LV_PART_MAIN);
    lv_obj_align_to(lring, arc, LV_ALIGN_CENTER, 0, 0);

    bsp_display_unlock();

    log_heap("after step screen");
    ESP_LOGI(TAG, "step screen: [%d/%d] '%s' %s  limit=%ds  emoji=%s",
             v->position + 1, v->step_count, v->label, v->emoji,
             v->time_limit_s, drew_emoji ? "rendered" : "FALLBACK");
    s_last_screen = RR_SCREEN_NONE;
    return ESP_OK;
}
