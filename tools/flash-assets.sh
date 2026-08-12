#!/usr/bin/env bash
# Flash the littlefs ASSET partition (emoji, avatars, audio).
#
# ⚠️ THIS DESTROYS THE COMPLETION QUEUE. The same partition holds
# /lfs/queue/runs.log — completed routines that have not reached the server yet —
# and rr_store.h calls that the one non-disposable thing on the watch. Writing
# this image throws those runs away.
#
# That is why it is a separate script and not part of `idf.py flash`: an asset
# flash is rare (assets change when the emoji/audio pipeline is regenerated),
# while `idf.py flash` happens dozens of times a day.
#
# BEFORE RUNNING: drain the watch. Open the parent app so the foreground sweep
# collects anything queued, or press Sync on the child screen, and check the
# watch's console reports `queued=0`.
#
#   tools/flash-assets.sh [PORT]
set -euo pipefail

PORT="${1:-/dev/cu.usbmodem1101}"
IMG="build/littlefs.bin"
OFFSET="0x920000"   # littlefs, per partitions.csv

if [ ! -f "$IMG" ]; then
  echo "error: $IMG not found — run 'idf.py build' first (the image is built, just not flashed)." >&2
  exit 1
fi

cat <<WARN

  ⚠️  Flashing $IMG to $OFFSET on $PORT
      This ERASES the littlefs partition, including /lfs/queue/runs.log —
      any completed routines not yet synced to the server will be LOST.

      Drain the watch first (open the parent app, or press Sync) and confirm
      its console shows queued=0.

WARN
read -r -p "  Type 'yes' to continue: " reply
[ "$reply" = "yes" ] || { echo "  aborted."; exit 1; }

python -m esptool --chip esp32c6 -p "$PORT" --before usb_reset --after hard_reset \
  write_flash "$OFFSET" "$IMG"

echo "  assets flashed. The routine cache and child record were erased too —"
echo "  re-push from the parent app (open it, or press Sync)."
