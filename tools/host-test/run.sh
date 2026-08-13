#!/usr/bin/env bash
# Compile and run the rr_store queue tests ON THE WORKSTATION.
#
# WHY THIS EXISTS. rr_queue_read_framed() turns the on-flash NDJSON log into the
# contract's [u16 len][json] wire stream, one <=500-byte page at a time. A page
# boundary can land anywhere — including inside a frame's 2-byte length prefix —
# and getting that arithmetic wrong corrupts records only at specific
# alignments. That is precisely the shape of bug that shipped in contract v2 and
# survived a full test suite, because the suite only ever used a record size that
# happened to fit in one page.
#
# It compiles the REAL components/rr_store/src/rr_store.c, not a copy. The only
# concessions to running off-target are three header stubs (esp_log, esp_err,
# esp_littlefs) and -DMOUNT_POINT pointing at a temp directory. The arithmetic
# under test is byte-for-byte what the firmware runs.
#
#   tools/host-test/run.sh
#
# Requires: a host C compiler and IDF_PATH (for cJSON only).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CJSON="${IDF_PATH:-$HOME/esp/v5.5.5-idf}/components/json/cJSON"

if [ ! -f "$CJSON/cJSON.c" ]; then
  echo "host-test: cannot find cJSON at $CJSON" >&2
  echo "host-test: set IDF_PATH to an ESP-IDF checkout" >&2
  exit 1
fi

OUT="$(mktemp -d)"
FSROOT="$OUT/fs"
trap 'rm -rf "$OUT"' EXIT

"${CC:-cc}" -std=c11 -O1 -Wall \
  -DMOUNT_POINT="\"$FSROOT\"" -DRR_HOST_ROOT="\"$FSROOT\"" \
  -o "$OUT/test_framed" \
  "$HERE/test_framed.c" \
  "$ROOT/components/rr_store/src/rr_store.c" \
  "$CJSON/cJSON.c" \
  -I"$HERE" -I"$HERE/stubs" \
  -I"$ROOT/components/rr_store/include" \
  -I"$CJSON" \
  -Wno-unused-parameter -Wno-unused-function

"$OUT/test_framed"
