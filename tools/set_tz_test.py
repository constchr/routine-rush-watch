#!/usr/bin/env python3
"""set_tz harness — the UTC offset for local wall-clock display.

Usage:
    python3 set_tz_test.py [offset_seconds]     # default: this machine's offset

Sends { "cmd": "set_tz", "offset_s": <int> } framed as [u32 len][UTF-8 JSON],
optionally preceded by TIME_SYNC so the pair can be observed together — which
is exactly how the phone sends them.

WHY THIS COMMAND EXISTS: TIME_SYNC is frozen at 4 bytes (a bare u32 epoch),
so there is nowhere in it to put an offset. The watch was setting its RTC to
raw UTC and rendering that verbatim — a Cyprus watch read 04:21 while the
phone read 07:21. The offset rides RR_CONTROL instead, which is additive and
leaves the frozen layout alone.

The gate here is conn_is_authorised (nonce OR paired peer), the same one
ROUTINE_PUSH uses — NOT the stricter paired-peer-only gate on factory_reset
and start_routine. So from a laptop that has merely bonded this is expected
to be REJECTED with ATT 0x05; run it from the paired phone to see it applied.
Either way the watch monitor is the real output.
"""
import asyncio
import json
import struct
import sys
import time

from bleak import BleakScanner, BleakClient

SERVICE = "fc19364a-c250-4477-928d-28c55ac1c2bd"
RR_CONTROL = "73c4f178-0884-4db4-9624-ff443355763b"
TIME_SYNC = "3c5a115d-61a0-4104-9883-ebe780044eb5"


def local_utc_offset_s() -> int:
    """Seconds to ADD to UTC for this machine's local time (signed)."""
    return -(time.altzone if time.daylight and time.localtime().tm_isdst else time.timezone)


async def main() -> int:
    offset_s = int(sys.argv[1]) if len(sys.argv) > 1 else local_utc_offset_s()

    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SERVICE.lower() in [str(u).lower() for u in ad.service_uuids],
        timeout=20.0,
    )
    if dev is None:
        print("FAIL: watch not found")
        return 1
    print(f"found {dev.name!r} {dev.address}")

    async with BleakClient(dev) as c:
        mtu = getattr(c, "mtu_size", 23)
        chunk = max(20, mtu - 3)

        epoch = int(time.time())
        print(f"TIME_SYNC epoch={epoch} (UTC)")
        try:
            await c.write_gatt_char(TIME_SYNC, struct.pack("<I", epoch), response=True)
            print("  TIME_SYNC accepted")
        except Exception as e:  # noqa: BLE001
            print(f"  TIME_SYNC rejected: {e}")

        hh, mm = divmod(abs(offset_s) // 60, 60)
        sign = "+" if offset_s >= 0 else "-"
        local = time.gmtime(epoch + offset_s)
        print(f"set_tz offset_s={offset_s} ({sign}{hh:02d}:{mm:02d}) "
              f"→ watch should show {local.tm_hour:02d}:{local.tm_min:02d}")

        body = json.dumps({"cmd": "set_tz", "offset_s": offset_s}).encode()
        blob = struct.pack("<I", len(body)) + body
        try:
            for i in range(0, len(blob), chunk):
                await c.write_gatt_char(RR_CONTROL, blob[i : i + chunk], response=True)
            print("  set_tz ACCEPTED — check the monitor for the applied local time")
        except Exception as e:  # noqa: BLE001
            print(f"  set_tz REJECTED: {e}")
            print("  (ATT 0x05 / Code=5 from a laptop is EXPECTED — not the paired peer)")
    return 0


sys.exit(asyncio.run(main()))
