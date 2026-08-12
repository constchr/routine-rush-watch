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
#define QR_SIZE 296   /* + 8px quiet-zone border fits the 320px safe box */

static lv_obj_t *s_screen;

// Last non-transient screen, so an aborted reset countdown can restore exactly
// what the watch was showing instead of guessing at pairing state.
typedef enum { RR_SCREEN_NONE, RR_SCREEN_QR, RR_SCREEN_PAIRED, RR_SCREEN_WAITING,
               RR_SCREEN_WATCHFACE, RR_SCREEN_ALARM } rr_screen_t;
static rr_screen_t s_last_screen;
static char s_last_qr_payload[128];

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "heap %s: free=%u largest_block=%u",
             when,
             (unsigned) esp_get_free_heap_size(),
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}


// ═════════════════════════════════════════════════════════════════════════════
// SAFE ZONE — the curved-bezel foundation. EVERY screen builds inside this.
//
// The 2.06" panel is 410x502 with physically ROUNDED corners (~R44). Glass
// clips anything near the edge, and the clipping is invisible in any simulator
// — it only shows on the device. So the inset is a foundation, not a per-screen
// adjustment: screens that hand-place things near an edge will look fine in
// development and be sheared in a child's hand.
//
// Two mechanisms, deliberately both:
//   1. clip_corner on the screen root, so anything that does reach the edge is
//      clipped to a clean radius instead of bleeding raggedly off the curve.
//   2. a SAFE CONTAINER inset RR_SAFE_INSET on all sides. Screens add
//      corner/edge content to this container and simply align to its edges —
//      no magic offsets to get wrong.
//
// The CENTRE is unobstructed, so hero content (avatar, QR, countdown ring) is
// added to the screen ROOT and may exceed the safe box. That is the whole
// point of separating the two: inset the corners, not the middle.
// ═════════════════════════════════════════════════════════════════════════════

static lv_obj_t *s_safe;

lv_obj_t *rr_ui_begin_screen(lv_color_t bg)
{
    lv_obj_clean(s_screen);
    s_safe = NULL;

    lv_obj_set_style_bg_color(s_screen, bg, LV_PART_MAIN);
    lv_obj_set_style_radius(s_screen, RR_CORNER_R, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(s_screen, true, 0);

    s_safe = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_safe);              // no bg, border, or padding
    lv_obj_set_size(s_safe, RR_SAFE_W, RR_SAFE_H);
    lv_obj_center(s_safe);
    lv_obj_remove_flag(s_safe, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_safe, LV_OBJ_FLAG_CLICKABLE);
    return s_safe;
}

lv_obj_t *rr_ui_screen_root(void) { return s_screen; }
lv_obj_t *rr_ui_safe_area(void)   { return s_safe; }

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

    lv_obj_t *root = rr_ui_begin_screen(lv_color_white());
    

    lv_obj_t *qr = lv_qrcode_create(root);
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

    lv_obj_align(qr, LV_ALIGN_CENTER, 0, -18);
    // A white border around the symbol IS the quiet zone. Without it the QR
    // runs to the edge of its canvas and scan reliability drops sharply.
    lv_obj_set_style_border_color(qr, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(qr, 8, LV_PART_MAIN);

    lv_obj_t *hint = lv_label_create(root);
    lv_label_set_text(hint, "Scan to pair");
    lv_obj_set_style_text_color(hint, lv_color_black(), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);

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

    lv_obj_t *root = rr_ui_begin_screen(lv_color_black());
    

    lv_obj_t *tick = lv_label_create(root);
    lv_label_set_text(tick, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(tick, lv_color_hex(0x10B981), LV_PART_MAIN);
    lv_obj_align(tick, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "Paired");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *sub = lv_label_create(root);
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

    lv_obj_t *root = rr_ui_begin_screen(lv_color_black());
    

    lv_obj_t *l = lv_label_create(root);
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
    lv_obj_t *root = rr_ui_begin_screen(lv_color_hex(0x7F1D1D));
    

    lv_obj_t *t = lv_label_create(root);
    lv_label_set_text(t, "Factory reset");
    lv_obj_set_style_text_color(t, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -70);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", seconds_remaining);
    lv_obj_t *n = lv_label_create(root);
    lv_label_set_text(n, buf);
    lv_obj_set_style_text_color(n, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(n, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_center(n);

    lv_obj_t *h = lv_label_create(root);
    lv_label_set_text(h, "Release to cancel");
    lv_obj_set_style_text_color(h, lv_color_hex(0xFCA5A5), LV_PART_MAIN);
    lv_obj_align(h, LV_ALIGN_CENTER, 0, 70);

    bsp_display_unlock();
    s_reset_prompt_active = true;
    return ESP_OK;
}

// The face is the one screen rr_ui cannot rebuild from its own state — it
// needs the clock, battery, child and schedule that rr_idle assembles. So
// rr_idle hands us a repaint callback rather than rr_ui reaching across to
// pull all of that together itself.
static void (*s_repaint_face)(void);

void rr_ui_set_watchface_repaint(void (*fn)(void))
{
    s_repaint_face = fn;
}

esp_err_t rr_ui_show_last_status(void)
{
    switch (s_last_screen) {
    case RR_SCREEN_QR:      return rr_ui_show_pairing_qr(s_last_qr_payload);
    case RR_SCREEN_PAIRED:  return rr_ui_show_paired_status();
    case RR_SCREEN_WAITING: return rr_ui_show_waiting_status();
    case RR_SCREEN_WATCHFACE:
    case RR_SCREEN_ALARM:
        // Without this an ABORTED reset hold left the red countdown on screen
        // until the next minute tick happened to repaint it. A SNOOZED alarm
        // needs the same treatment for the same reason — dismissing it has to
        // put something back, or the alarm screen simply stays up.
        if (s_repaint_face) s_repaint_face();
        return ESP_OK;
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
    lv_obj_t *root = rr_ui_begin_screen(RR_BG);
    

    // The gate: Greek and Latin in the SAME font, both legible, no boxes.
    lv_obj_t *el = lv_label_create(root);
    lv_label_set_text(el, "Πρωινή ρουτίνα");
    lv_obj_set_style_text_font(el, &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(el, RR_TEXT, LV_PART_MAIN);
    lv_obj_align(el, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *en = lv_label_create(root);
    lv_label_set_text(en, "Morning routine");
    lv_obj_set_style_text_font(en, &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(en, RR_ACCENT, LV_PART_MAIN);
    lv_obj_align(en, LV_ALIGN_CENTER, 0, 0);

    // Mixed in one string — the real case, since a bilingual family's routine
    // names and step labels are not guaranteed to be single-script.
    lv_obj_t *mix = lv_label_create(root);
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

// ═════════════════════════════════════════════════════════════════════════════
// STEP SCREEN VERTICAL LAYOUT (§8 screens 2/3)
//
// ⚠️ EVERY Y HERE IS TOP-RELATIVE TO THE SAFE AREA, DELIBERATELY.
//
// The original layout positioned the emoji, label and ring from the TOP and the
// two buttons from the BOTTOM. Both halves were individually reasonable and
// nothing in the code related them, so nobody could see that they collided —
// and on hardware the OK button was drawn straight through the countdown ring
// (the ring occupied y=224..374, the button y=296..364). Photographed on device
// in Phase 9.
//
// Two rules keep that from coming back:
//   1. one coordinate system — every element is placed from the top of the safe
//      area, so the numbers can simply be compared;
//   2. the comparisons are _Static_assert, so an overlapping layout is a BUILD
//      FAILURE rather than something to notice on a photograph.
//
// The other half of the fix is spatial: a stacked emoji AND a separate ring do
// not fit in 412 px alongside a two-line label and two buttons. The ring is now
// CONCENTRIC — the emoji sits inside it with the mm:ss below, so the countdown
// reads as a halo around the step's icon. That reclaims ~100 px, and on a 2"
// kids' watch it looks more like a watch than a stack of boxes.
// ═════════════════════════════════════════════════════════════════════════════

#define STEP_POS_Y        0     /* "1 / 2" */
#define STEP_POS_H        26

#define STEP_RING_Y       26
#define STEP_RING_SIZE    186
#define STEP_RING_STROKE  12
#define STEP_RING_EMOJI_DY (-12)  /* emoji offset from the ring centre */
#define STEP_RING_MMSS_DY  58     /* mm:ss offset from the ring centre */

// Two lines of rr_font_28 (~34 px each). The label was rr_font_36, and this is
// the one real concession the fix makes: at 36 px a two-line Greek label
// ("Καθαρίζεις το τραπέζι") needs 88 px and there is nowhere to take it from.
// 28 px is still large on a 410x502 panel, and shrinking the instruction beats
// truncating it or overlapping the button a child has to press.
#define STEP_LABEL_Y      220
#define STEP_LABEL_H      72

#define STEP_SKIP_H       38
#define STEP_SKIP_Y       (RR_SAFE_H - STEP_SKIP_H)
#define STEP_OK_H         66
#define STEP_OK_Y         (STEP_SKIP_Y - 8 - STEP_OK_H)

// The untimed step (§8 screen 3) has no ring, so its emoji and label get the
// space the ring would have used. Same buttons, same bottom anchors.
#define STEP_U_EMOJI_Y    46
#define STEP_U_EMOJI_H    96
#define STEP_U_LABEL_Y    166
#define STEP_U_LABEL_H    88    /* room for two lines of rr_font_36 here */

_Static_assert(STEP_POS_Y + STEP_POS_H <= STEP_RING_Y,
               "step screen: the position counter overlaps the countdown ring");
_Static_assert(STEP_RING_Y + STEP_RING_SIZE <= STEP_LABEL_Y,
               "step screen: the countdown ring overlaps the step label");
_Static_assert(STEP_LABEL_Y + STEP_LABEL_H <= STEP_OK_Y,
               "step screen: the step label overlaps the OK button");
_Static_assert(STEP_OK_Y + STEP_OK_H <= STEP_SKIP_Y,
               "step screen: the OK button overlaps the Skip button");
_Static_assert(STEP_SKIP_Y + STEP_SKIP_H <= RR_SAFE_H,
               "step screen: the Skip button falls outside the safe area");
_Static_assert(STEP_U_EMOJI_Y + STEP_U_EMOJI_H <= STEP_U_LABEL_Y,
               "untimed step: the emoji overlaps the label");
_Static_assert(STEP_U_LABEL_Y + STEP_U_LABEL_H <= STEP_OK_Y,
               "untimed step: the label overlaps the OK button");

// The concentric content must also fit INSIDE the ring, which is a circle and
// not a box: at the emoji's widest the inner radius has to still clear half the
// emoji. Checked at build time in integer maths (squared, to avoid sqrt).
#define STEP_RING_INNER_R  ((STEP_RING_SIZE - 2 * STEP_RING_STROKE) / 2)
#define STEP_EMOJI_HALF    48
#define STEP_EMOJI_FAR_Y   (-STEP_RING_EMOJI_DY + STEP_EMOJI_HALF)   /* |y| at the top edge */
_Static_assert(STEP_EMOJI_FAR_Y * STEP_EMOJI_FAR_Y + STEP_EMOJI_HALF * STEP_EMOJI_HALF
               <= STEP_RING_INNER_R * STEP_RING_INNER_R,
               "step screen: the emoji's corners fall outside the countdown ring");
_Static_assert(STEP_RING_MMSS_DY + 17 <= STEP_RING_INNER_R,
               "step screen: the mm:ss readout falls outside the countdown ring");

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
    lv_obj_t *root = rr_ui_begin_screen(RR_BG);
    s_arc = NULL;
    s_mmss = NULL;

    const bool timed = v->time_limit_s > 0;

    // ── position "1 / 3" ────────────────────────────────────────────────────
    char pos[24];
    snprintf(pos, sizeof(pos), "%d / %d", v->position + 1, v->step_count);
    lv_obj_t *lpos = lv_label_create(root);
    lv_label_set_text(lpos, pos);
    lv_obj_set_style_text_font(lpos, &rr_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lpos, RR_MUTED, LV_PART_MAIN);
    lv_obj_align(lpos, LV_ALIGN_TOP_MID, 0, STEP_POS_Y);

    // ── countdown ring — created FIRST so the emoji and mm:ss draw inside it ─
    // §8 screen 3: an untimed step gets no ring at all. Showing an empty or
    // full ring that never moves would imply a timer that is broken.
    if (timed) {
        s_arc = lv_arc_create(root);
        lv_obj_set_size(s_arc, STEP_RING_SIZE, STEP_RING_SIZE);
        lv_arc_set_rotation(s_arc, 270);
        lv_arc_set_bg_angles(s_arc, 0, 360);
        lv_arc_set_range(s_arc, 0, 1000);      // finer than seconds, so the
        lv_arc_set_value(s_arc, 1000);         // sweep looks continuous
        lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(s_arc, STEP_RING_STROKE, LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_arc, STEP_RING_STROKE, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_arc, RR_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_arc, RR_ACCENT, LV_PART_INDICATOR);
        lv_obj_align(s_arc, LV_ALIGN_TOP_MID, 0, STEP_RING_Y);
    }

    // ── emoji, 96px, streamed from littlefs ─────────────────────────────────
    // Inside the ring when there is one, so the countdown reads as a halo round
    // the step's icon rather than competing with it for vertical space.
    char lvpath[96];
    const char *posix_path = rr_emoji_path(v->emoji);
    bool drew_emoji = false;
    lv_obj_t *icon = NULL;

    if (posix_path != NULL && to_lv_path(posix_path, lvpath, sizeof(lvpath))) {
        icon = lv_image_create(root);
        lv_image_set_src(icon, lvpath);
        drew_emoji = true;
    } else {
        icon = lv_obj_create(root);
        lv_obj_set_size(icon, 96, 96);
        lv_obj_set_style_radius(icon, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(icon, RR_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_border_width(icon, 0, LV_PART_MAIN);
        ESP_LOGW(TAG, "emoji '%s' unmapped — fallback dot", v->emoji);
    }

    if (timed) lv_obj_align_to(icon, s_arc, LV_ALIGN_CENTER, 0, STEP_RING_EMOJI_DY);
    else       lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, STEP_U_EMOJI_Y);

    // ── mm:ss, inside the ring under the emoji ──────────────────────────────
    if (timed) {
        char buf[16];
        mmss_str(v->time_limit_s, buf, sizeof(buf));
        s_mmss = lv_label_create(root);
        lv_label_set_text(s_mmss, buf);
        lv_obj_set_style_text_font(s_mmss, &rr_font_28, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_mmss, RR_TEXT, LV_PART_MAIN);
        lv_obj_align_to(s_mmss, s_arc, LV_ALIGN_CENTER, 0, STEP_RING_MMSS_DY);
    }

    // ── step label ──────────────────────────────────────────────────────────
    lv_obj_t *llabel = lv_label_create(root);
    lv_label_set_long_mode(llabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(llabel, RR_SAFE_W);
    lv_label_set_text(llabel, v->label);
    // Keep the label at rr_font_36 when it fits on ONE line, and only drop to
    // rr_font_28 when it needs two. Measured rather than assumed: most step
    // labels are one Greek word ("Βούρτσισμα") and shrinking those to make room
    // for the long ones would be paying the cost everywhere to fix it in a few
    // places. STEP_LABEL_H (72) holds either one line of 36 or two of 28.
    lv_point_t sz;
    lv_text_get_size(&sz, v->label, &rr_font_36, 0, 0, RR_SAFE_W, LV_TEXT_FLAG_NONE);
    const lv_font_t *label_font = (sz.y <= 44) ? &rr_font_36 : &rr_font_28;
    lv_obj_set_style_text_font(llabel, timed ? label_font : &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(llabel, RR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_align(llabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(llabel, LV_ALIGN_TOP_MID, 0, timed ? STEP_LABEL_Y : STEP_U_LABEL_Y);

    // ── Done: the primary action, deliberately the biggest thing on screen ──
    lv_obj_t *bdone = lv_button_create(root);
    lv_obj_set_size(bdone, RR_SAFE_W, STEP_OK_H);
    lv_obj_align(bdone, LV_ALIGN_TOP_MID, 0, STEP_OK_Y);
    lv_obj_set_style_bg_color(bdone, RR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_radius(bdone, STEP_OK_H / 2, LV_PART_MAIN);
    lv_obj_add_event_cb(bdone, done_event_cb, LV_EVENT_CLICKED, (void *) on_done);
    lv_obj_t *ldone = lv_label_create(bdone);
    lv_label_set_text(ldone, "OK");
    lv_obj_set_style_text_font(ldone, &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(ldone, RR_BG, LV_PART_MAIN);
    lv_obj_center(ldone);

    // ── Skip: present but visually quiet, so it is not the obvious tap ──────
    lv_obj_t *bskip = lv_button_create(root);
    lv_obj_set_size(bskip, 140, STEP_SKIP_H);
    lv_obj_align(bskip, LV_ALIGN_TOP_MID, 0, STEP_SKIP_Y);
    lv_obj_set_style_bg_color(bskip, RR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(bskip, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(bskip, RR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_radius(bskip, STEP_SKIP_H / 2, LV_PART_MAIN);
    lv_obj_add_event_cb(bskip, done_event_cb, LV_EVENT_CLICKED, (void *) on_skip);
    lv_obj_t *lskip = lv_label_create(bskip);
    lv_label_set_text(lskip, "Skip");
    lv_obj_set_style_text_font(lskip, &rr_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lskip, RR_MUTED, LV_PART_MAIN);
    lv_obj_center(lskip);

    bsp_display_unlock();

    ESP_LOGI(TAG, "step [%d/%d] '%s' %s limit=%ds emoji=%s label=%s",
             v->position + 1, v->step_count, v->label, v->emoji,
             v->time_limit_s, drew_emoji ? "ok" : "FALLBACK",
             label_font == &rr_font_36 ? "36/1line" : "28/wrapped");
    // The layout the asserts guarantee, printed once per step so a future
    // regression is visible in a log and not only on a photograph.
    if (timed) {
        ESP_LOGI(TAG, "  layout y: pos 0-%d | ring %d-%d | label %d-%d | OK %d-%d | skip %d-%d (safe %d)",
                 STEP_POS_H, STEP_RING_Y, STEP_RING_Y + STEP_RING_SIZE,
                 STEP_LABEL_Y, STEP_LABEL_Y + STEP_LABEL_H,
                 STEP_OK_Y, STEP_OK_Y + STEP_OK_H,
                 STEP_SKIP_Y, STEP_SKIP_Y + STEP_SKIP_H, RR_SAFE_H);
    }
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
    lv_obj_t *root = rr_ui_begin_screen(RR_BG);
    s_arc = NULL;
    s_mmss = NULL;
    

    lv_obj_t *tick = lv_label_create(root);
    lv_label_set_text(tick, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(tick, &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(tick, RR_SUCCESS, LV_PART_MAIN);
    lv_obj_align(tick, LV_ALIGN_CENTER, 0, -110);

    // Greek, because the cached routine is Greek and this proves the font on a
    // second screen. child.language is not cached yet (Phase 5), so this is
    // not yet locale-driven — noted in rr_routine.h.
    lv_obj_t *big = lv_label_create(root);
    lv_label_set_text(big, "Τέλος!");
    lv_obj_set_style_text_font(big, &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(big, RR_TEXT, LV_PART_MAIN);
    lv_obj_align(big, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t *name = lv_label_create(root);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(name, RR_SAFE_W);
    lv_label_set_text(name, routine_name ? routine_name : "");
    lv_obj_set_style_text_font(name, &rr_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, RR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, 10);

    char summary[48];
    snprintf(summary, sizeof(summary), "%d done  ·  %d skipped", done, skipped);
    lv_obj_t *sum = lv_label_create(root);
    lv_label_set_text(sum, summary);
    lv_obj_set_style_text_font(sum, &rr_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(sum, RR_MUTED, LV_PART_MAIN);
    lv_obj_align(sum, LV_ALIGN_CENTER, 0, 70);

    bsp_display_unlock();

    ESP_LOGI(TAG, "routine complete: %d done, %d skipped", done, skipped);
    return ESP_OK;
}

// ═════════════════════════════════════════════════════════════════════════════
// Phase 6 — the idle watch face (§9B.1)
//
// Everything here is LOCAL: time from the PCF85063, date from the same, the
// next-routine hint from the littlefs cache. It renders identically whether or
// not a phone has ever connected (§9B.3) — that is the point of the design,
// not an optimisation.
//
// AMOLED-friendly: mostly black pixels, because on this panel an unlit pixel
// costs nothing. That is why the face is sparse rather than decorated.
// ═════════════════════════════════════════════════════════════════════════════

static const char *WEEKDAY_EN[7] = { "Monday", "Tuesday", "Wednesday", "Thursday",
                                     "Friday", "Saturday", "Sunday" };
static const char *WEEKDAY_EL[7] = { "Δευτέρα", "Τρίτη", "Τετάρτη", "Πέμπτη",
                                     "Παρασκευή", "Σάββατο", "Κυριακή" };
static const char *MONTH_EN[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *MONTH_EL[12] = { "Ιαν", "Φεβ", "Μαρ", "Απρ", "Μαΐ", "Ιουν",
                                    "Ιουλ", "Αυγ", "Σεπ", "Οκτ", "Νοε", "Δεκ" };
// Short forms for the corner block — a full "Παρασκευή" would crowd the avatar.
static const char *WEEKDAY_EN_SHORT[7] = { "MON","TUE","WED","THU","FRI","SAT","SUN" };
static const char *WEEKDAY_EL_SHORT[7] = { "ΔΕΥ","ΤΡΙ","ΤΕΤ","ΠΕΜ","ΠΑΡ","ΣΑΒ","ΚΥΡ" };

// ISO weekday (1=Mon..7=Sun) from a civil date — Sakamoto's method.
int rr_ui_iso_weekday(int y, int m, int d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    int dow = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;  // 0=Sunday
    return dow == 0 ? 7 : dow;
}

// avatar_id -> hero image. The child record stores an ID ('lion'), not an
// emoji, so the mapping lives here rather than going through the step-emoji
// manifest (which does not contain the avatar set at all).
static const struct { const char *id; const char *file; } AVATAR_FILES[] = {
    { "lion",   "1f981" }, { "fox",    "1f98a" }, { "bear", "1f43b" },
    { "rabbit", "1f430" }, { "panda",  "1f43c" }, { "owl",  "1f989" },
};

static bool avatar_lv_path(const char *avatar_id, char *out, size_t cap)
{
    if (avatar_id == NULL || avatar_id[0] == '\0') return false;
    for (size_t i = 0; i < sizeof(AVATAR_FILES) / sizeof(AVATAR_FILES[0]); i++) {
        if (strcmp(avatar_id, AVATAR_FILES[i].id) == 0) {
            int n = snprintf(out, cap, "%c:/lfs/avatars/%s.bin",
                             (char) CONFIG_LV_FS_POSIX_LETTER, AVATAR_FILES[i].file);
            return n > 0 && (size_t) n < cap;
        }
    }
    return false;
}


// ── Legibility scrim ────────────────────────────────────────────────────────
// White text over a full-bleed avatar is unreadable wherever the avatar is
// bright — the lion's mane was the worst case on the real panel.
//
// A vertical gradient from black to fully transparent, at the top and bottom
// edges only. It darkens exactly where the text lives and vanishes over the
// avatar's face, so the hero still reads as the hero. On AMOLED the darkened
// band also costs less power than the pixels it covers.
//
// Chosen over a per-glyph shadow/outline because it is independent of what is
// behind it: an outline still fails against a mid-tone, whereas the scrim
// guarantees a known-dark backdrop no matter what avatar the child picks.
static void add_scrim(lv_obj_t *parent, bool top, int height)
{
    lv_obj_t *sc = lv_obj_create(parent);
    lv_obj_remove_style_all(sc);
    lv_obj_set_size(sc, RR_SCREEN_W, height);
    lv_obj_align(sc, top ? LV_ALIGN_TOP_MID : LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(sc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(sc, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_bg_color(sc, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(sc, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(sc, LV_GRAD_DIR_VER, LV_PART_MAIN);
    // Opaque at the screen edge, transparent where it meets the avatar.
    lv_obj_set_style_bg_opa(sc, top ? 225 : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_opa(sc, top ? LV_OPA_TRANSP : 235, LV_PART_MAIN);
}

esp_err_t rr_ui_show_watchface(const rr_watchface_t *w)
{
    if (w == NULL) return ESP_ERR_INVALID_ARG;

    bsp_display_lock(0);
    lv_obj_t *root = rr_ui_begin_screen(lv_color_black());
    s_arc = NULL;
    s_mmss = NULL;
    // Pure black, not the app's #0D1117: on AMOLED an unlit pixel draws nothing,
    // and this screen is mostly background. The avatar supplies the colour.
    

    const bool el = (w->language[0] == 'e' && w->language[1] == 'l');
    const int wd = rr_ui_iso_weekday(w->year, w->month, w->day);

    // ── THE HERO first, so everything after it draws ON TOP ─────────────────
    // Ordering matters: the safe container is created by begin_screen(), so an
    // avatar added to the root afterwards would overdraw all of the text. The
    // avatar goes down first, then the scrims, then the container is moved to
    // the foreground — z-order guaranteed rather than incidental.
    char apath[80];
    bool drew_avatar = false;
    if (avatar_lv_path(w->avatar_id, apath, sizeof(apath))) {
        lv_obj_t *av = lv_image_create(rr_ui_screen_root());
        lv_image_set_src(av, apath);
        lv_obj_align(av, LV_ALIGN_CENTER, 0, 24);
        drew_avatar = true;
    } else {
        lv_obj_t *ph = lv_label_create(rr_ui_screen_root());
        lv_label_set_text(ph, LV_SYMBOL_IMAGE);
        lv_obj_set_style_text_font(ph, &rr_font_36, LV_PART_MAIN);
        lv_obj_set_style_text_color(ph, RR_SURFACE, LV_PART_MAIN);
        lv_obj_align(ph, LV_ALIGN_CENTER, 0, 24);
        ESP_LOGW(TAG, "no hero avatar for id '%s'", w->avatar_id);
    }

    add_scrim(rr_ui_screen_root(), true, 168);
    add_scrim(rr_ui_screen_root(), false, 190);
    lv_obj_move_foreground(root);   // all text above the avatar AND the scrims

    // ── top-LEFT: battery ───────────────────────────────────────────────────
    // Tertiary grey normally — it is ambient information, not something a child
    // should be reading. Red ONLY when low: the one case where an alert beats
    // the palette's data-colour rule, because a dead watch fails silently.
    if (w->batt_valid) {
        char b[24];
        snprintf(b, sizeof(b), "%s%d%%", w->charging ? LV_SYMBOL_CHARGE : "", w->batt_percent);
        lv_obj_t *lb = lv_label_create(root);
        lv_label_set_text(lb, b);
        lv_obj_set_style_text_font(lb, &rr_font_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(lb,
            (w->batt_percent < RR_BATT_LOW_PCT && !w->charging) ? RR_DANGER : RR_MUTED,
            LV_PART_MAIN);
        lv_obj_align(lb, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    // ── top-RIGHT: date over time, right-aligned so they form one block ─────
    char date[48];
    snprintf(date, sizeof(date), "%s %d",
             el ? WEEKDAY_EL_SHORT[wd - 1] : WEEKDAY_EN_SHORT[wd - 1], w->day);
    lv_obj_t *ldate = lv_label_create(root);
    lv_label_set_text(ldate, date);
    lv_obj_set_style_text_font(ldate, &rr_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(ldate, RR_MUTED, LV_PART_MAIN);
    lv_obj_align(ldate, LV_ALIGN_TOP_RIGHT, 0, 0);

    char hhmm[8];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", w->hour, w->minute);
    lv_obj_t *ltime = lv_label_create(root);
    lv_label_set_text(ltime, hhmm);
    // Largest text on the face: the time is what a glance is FOR. Everything
    // else on this screen is secondary to it.
    lv_obj_set_style_text_font(ltime, &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(ltime, RR_TEXT, LV_PART_MAIN);
    lv_obj_align(ltime, LV_ALIGN_TOP_RIGHT, 0, 22);

    // ── bottom: next routine, deliberately quiet ────────────────────────────
    // A thin line under the avatar. It matters, but it must not compete with
    // the character for attention.
    char hint[96];
    if (w->next.found) {
        snprintf(hint, sizeof(hint), "%02d:%02d  %s", w->next.hour, w->next.minute,
                 w->next.routine_name);
    } else {
        snprintf(hint, sizeof(hint), "%s", el ? "Όλα έτοιμα" : "All done");
    }
    lv_obj_t *lnext = lv_label_create(root);
    lv_label_set_long_mode(lnext, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lnext, RR_SAFE_W);
    lv_label_set_text(lnext, hint);
    lv_obj_set_style_text_font(lnext, &rr_font_20, LV_PART_MAIN);
    // White, not grey: this line sits over the avatar's lower edge, where grey
    // on a mid-tone was the least readable thing on the face.
    lv_obj_set_style_text_color(lnext, w->next.found ? RR_TEXT : RR_SUCCESS, LV_PART_MAIN);
    lv_obj_set_style_text_align(lnext, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(lnext, LV_ALIGN_BOTTOM_MID, 0, -28);

    // ── steps: the slot, still a placeholder until Phase 9 ──────────────────
    char sline[32];
    if (w->steps_valid) snprintf(sline, sizeof(sline), "%d %s", w->steps_today, el ? "βήματα" : "steps");
    else                snprintf(sline, sizeof(sline), "—  %s", el ? "βήματα" : "steps");
    lv_obj_t *lst = lv_label_create(root);
    lv_label_set_text(lst, sline);
    lv_obj_set_style_text_font(lst, &rr_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lst, RR_MUTED, LV_PART_MAIN);
    lv_obj_align(lst, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Unsynced completions — small, top-centre, only when non-zero.
    if (w->queued_runs > 0) {
        char q[24];
        snprintf(q, sizeof(q), LV_SYMBOL_UPLOAD " %d", w->queued_runs);
        lv_obj_t *lq = lv_label_create(root);
        lv_label_set_text(lq, q);
        lv_obj_set_style_text_font(lq, &rr_font_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(lq, RR_ACCENT, LV_PART_MAIN);
        lv_obj_align(lq, LV_ALIGN_TOP_MID, 0, 0);
    }

    bsp_display_unlock();

    s_last_screen = RR_SCREEN_WATCHFACE;
    ESP_LOGI(TAG, "watchface: %s %s | avatar=%s(%s) | batt=%d%%%s | next=%s",
             hhmm, date, w->avatar_id, drew_avatar ? "hero" : "MISSING",
             w->batt_percent, w->charging ? "+" : "",
             w->next.found ? w->next.routine_name : "(none)");
    return ESP_OK;
}

bool rr_ui_last_screen_is_watchface(void)
{
    return s_last_screen == RR_SCREEN_WATCHFACE;
}

// ── Phase 7: the alarm screen (§7, §8 screen 1) ─────────────────────────────
//
// This is §8's "Start" screen with one thing added: a snooze. It is NOT a
// second way to run a routine — nothing starts until "Πάμε"/"Let's go" is
// tapped, and that tap goes through rr_routine_request_start() like every
// other start on this watch.
//
// Why a screen at all, rather than dropping straight into step 1: a scheduled
// routine fires at a child who is not holding the watch and did not ask for
// it. Starting a 30-second countdown they are not looking at spends the first
// step before they have picked up their wrist. The alarm rings, the screen
// says what it wants, and the child decides.

esp_err_t rr_ui_show_alarm(const char *routine_name, const char *routine_emoji,
                           int hour, int minute, const char *language,
                           rr_ui_step_cb_t on_start, rr_ui_step_cb_t on_snooze)
{
    const bool el = (language != NULL && language[0] == 'e' && language[1] == 'l');

    bsp_display_lock(0);
    lv_obj_t *root = rr_ui_begin_screen(RR_BG);
    s_arc = NULL;
    s_mmss = NULL;

    // The scheduled time, in the accent colour — this is data, and it is the
    // answer to the child's first question ("why is my watch making noise").
    char hhmm[8];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", hour, minute);
    lv_obj_t *ltime = lv_label_create(root);
    lv_label_set_text(ltime, hhmm);
    lv_obj_set_style_text_font(ltime, &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(ltime, RR_ACCENT, LV_PART_MAIN);
    lv_obj_align(ltime, LV_ALIGN_TOP_MID, 0, 0);

    char lvpath[96];
    const char *posix_path = routine_emoji ? rr_emoji_path(routine_emoji) : NULL;
    if (posix_path != NULL && to_lv_path(posix_path, lvpath, sizeof(lvpath))) {
        lv_obj_t *img = lv_image_create(root);
        lv_image_set_src(img, lvpath);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 56);
    }

    lv_obj_t *lname = lv_label_create(root);
    lv_label_set_long_mode(lname, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lname, RR_SAFE_W);
    lv_label_set_text(lname, routine_name ? routine_name : "");
    lv_obj_set_style_text_font(lname, &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(lname, RR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_align(lname, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(lname, LV_ALIGN_TOP_MID, 0, 170);

    // Primary action, full width — the same shape and size as the runtime's
    // Done button, so "the big one starts things" holds across every screen.
    lv_obj_t *bgo = lv_button_create(root);
    lv_obj_set_size(bgo, RR_SAFE_W, 68);
    lv_obj_align(bgo, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_set_style_bg_color(bgo, RR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_radius(bgo, 34, LV_PART_MAIN);
    lv_obj_add_event_cb(bgo, done_event_cb, LV_EVENT_CLICKED, (void *) on_start);
    lv_obj_t *lgo = lv_label_create(bgo);
    lv_label_set_text(lgo, el ? "Πάμε!" : "Let's go!");
    lv_obj_set_style_text_font(lgo, &rr_font_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(lgo, RR_BG, LV_PART_MAIN);
    lv_obj_center(lgo);

    // Snooze: quiet, like Skip. Deliberately NOT "tap anywhere to snooze" —
    // the alarm wakes a lit screen on a moving wrist, and a whole-screen
    // snooze target would be hit by accident far more often than on purpose.
    lv_obj_t *bsn = lv_button_create(root);
    lv_obj_set_size(bsn, 180, 44);
    lv_obj_align(bsn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bsn, RR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(bsn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(bsn, RR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_radius(bsn, 22, LV_PART_MAIN);
    lv_obj_add_event_cb(bsn, done_event_cb, LV_EVENT_CLICKED, (void *) on_snooze);
    lv_obj_t *lsn = lv_label_create(bsn);
    lv_label_set_text(lsn, el ? "Σε 5′" : "Snooze 5′");
    lv_obj_set_style_text_font(lsn, &rr_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lsn, RR_MUTED, LV_PART_MAIN);
    lv_obj_center(lsn);

    bsp_display_unlock();

    // Its own screen kind, for two reasons: rr_ui_last_screen_is_watchface()
    // must say NO (or rr_idle's minute tick repaints the face straight over a
    // ringing alarm), and dismissing a snooze has to restore something, which
    // rr_ui_show_last_status() can only do if it knows what is up.
    s_last_screen = RR_SCREEN_ALARM;
    ESP_LOGI(TAG, "ALARM screen: %s @ %s (%s)", routine_name ? routine_name : "?",
             hhmm, el ? "el" : "en");
    return ESP_OK;
}
