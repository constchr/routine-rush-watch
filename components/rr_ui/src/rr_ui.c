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
