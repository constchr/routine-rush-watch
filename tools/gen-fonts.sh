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

for SZ in 20 28 36; do
  npx --yes lv_font_conv --font "$TTF" \
    --range "$RANGES" --size "$SZ" --bpp 4 --no-compress \
    --format lvgl --lv-include lvgl.h \
    -o "assets/fonts/rr_font_$SZ.c"
  echo "generated assets/fonts/rr_font_$SZ.c"
done
