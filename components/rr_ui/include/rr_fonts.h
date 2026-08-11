#pragma once
// Noto Sans, Latin + Greek, generated with lv_font_conv.
//
// Greek is a first-class locale (§9): step labels arrive from the cache as
// UTF-8 Greek, and LVGL's built-in Montserrat covers Latin only — every Greek
// glyph would render as a box. These fonts carry:
//
//   0x20-0x7F     Basic Latin
//   0xA0-0xFF     Latin-1 Supplement (accents, °, ×)
//   0x2010-0x205E General Punctuation (en/em dash, curly quotes, ellipsis)
//   0x370-0x3FF   Greek and Coptic — all of modern Greek, incl. tonos forms
//
// Three sizes, matching §9's "title, body, ring":
//   36  step label — the one thing a child reads across a room
//   28  countdown mm:ss in the ring centre
//   20  secondary text (step position, hints)
//
// Regenerate: tools/gen-fonts.sh

#include "lvgl.h"

LV_FONT_DECLARE(rr_font_20)
LV_FONT_DECLARE(rr_font_28)
LV_FONT_DECLARE(rr_font_36)
