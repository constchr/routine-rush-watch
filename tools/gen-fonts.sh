#!/usr/bin/env bash
# Regenerate the LVGL fonts in assets/fonts/ from Noto Sans.
#
# Noto Sans (latin-greek-cyrillic) is the source because the product is
# bilingual el/en and step labels mix both in the same string.
set -euo pipefail
cd "$(dirname "$0")/.."

TTF="${1:-}"
if [ -z "$TTF" ]; then
  echo "usage: tools/gen-fonts.sh /path/to/NotoSans-Regular.ttf" >&2
  echo "  get it from https://github.com/notofonts/notofonts.github.io" >&2
  exit 1
fi

# Modern Greek lives entirely in 0x370-0x3FF; Greek Extended (polytonic,
# 0x1F00+) is deliberately excluded — it would roughly double the glyph count
# for text this product never renders.
RANGES="0x20-0x7F,0xA0-0xFF,0x2010-0x205E,0x370-0x3FF"

# ── LV_SYMBOL_* glyphs ──────────────────────────────────────────────────────
#
# These are FontAwesome codepoints in LVGL's private-use block, and they exist
# ONLY in LVGL's built-in montserrat fonts. rr_font_* are Noto, so any label
# that mixes a symbol with our own font drew a MISSING-GLYPH RECTANGLE — the
# charging bolt on the watch face was showing as a box, and so were the upload
# arrow on the queued-runs badge, the tick, and the image placeholder.
#
# Merging them in is the fix rather than dropping to montserrat for those
# labels, because montserrat has no Greek: swapping the font to get a bolt
# would have turned every Greek label beside it into boxes instead.
#
# Only the four symbols actually used are pulled in, by codepoint — the whole
# FontAwesome set is ~1300 glyphs at three sizes for four we render.
FA_WOFF="managed_components/lvgl__lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff"
#   F00C LV_SYMBOL_OK      F03E LV_SYMBOL_IMAGE
#   F093 LV_SYMBOL_UPLOAD  F0E7 LV_SYMBOL_CHARGE
SYMBOLS="0xF00C,0xF03E,0xF093,0xF0E7"

if [ ! -f "$FA_WOFF" ]; then
  echo "missing $FA_WOFF — run 'idf.py reconfigure' first to fetch the LVGL component" >&2
  exit 1
fi

for SZ in 20 28 36; do
  npx --yes lv_font_conv \
    --font "$TTF" --range "$RANGES" \
    --font "$FA_WOFF" --range "$SYMBOLS" \
    --size "$SZ" --bpp 4 --no-compress \
    --format lvgl --lv-include lvgl.h \
    -o "assets/fonts/rr_font_$SZ.c"
  echo "generated assets/fonts/rr_font_$SZ.c"
done
