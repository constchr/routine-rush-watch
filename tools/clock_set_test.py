#!/usr/bin/env python3
"""Set the watch's clock to an arbitrary LOCAL wall-clock time.

Usage:
    python3 clock_set_test.py <nonce> <HH:MM:SS>     # set to that local time today
    python3 clock_set_test.py <nonce> now            # restore the real time

Exists to test things that otherwise need waiting for the wall clock: the
Phase 9 local-midnight step rollover, and Phase 7's day-boundary scheduling.
Sends TIME_SYNC (a UTC epoch, per the frozen contract) plus set_tz, exactly as
the phone does — so the watch is exercised through its real inputs rather than
through a test-only backdoor.

⚠️ This MOVES THE WATCH'S CLOCK. Always follow a test with `now` to restore it,
or the scheduler will be reasoning about the wrong time of day.
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
    return -(time.altzone if time.daylight and time.localtime().tm_isdst else time.timezone)


async def write(client, blob, chunk, char, attempts: int = 4):
    for attempt in range(attempts):
        try:
            for i in range(0, len(blob), chunk):
                await client.write_gatt_char(char, blob[i:i + chunk], response=True)
            return
        except Exception:  # noqa: BLE001
            if attempt == attempts - 1:
                raise
            await asyncio.sleep(2.0)


async def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    nonce, target = sys.argv[1].strip(), sys.argv[2].strip()
    offset = local_utc_offset_s()

    if target == "now":
        epoch = int(time.time())
    else:
        hh, mm, ss = (int(p) for p in target.split(":"))
        now = time.localtime()
        # Build the UTC epoch for that LOCAL time today: take local midnight,
        # add the requested time of day, then subtract the offset to get UTC.
        midnight_local = time.mktime((now.tm_year, now.tm_mon, now.tm_mday,
                                      0, 0, 0, 0, 0, now.tm_isdst))
        epoch = int(midnight_local) + hh * 3600 + mm * 60 + ss

    shown = time.gmtime(epoch + offset)
    print(f"target local time : {shown.tm_hour:02d}:{shown.tm_min:02d}:{shown.tm_sec:02d} "
          f"({shown.tm_year}-{shown.tm_mon:02d}-{shown.tm_mday:02d})")
    print(f"TIME_SYNC epoch   : {epoch} (UTC), set_tz {offset:+d}s")

    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SERVICE.lower() in [str(u).lower() for u in ad.service_uuids],
        timeout=20.0)
    if dev is None:
        print("FAIL: watch not found")
        return 1

    async with BleakClient(dev) as c:
        mtu = getattr(c, "mtu_size", 23)
        chunk = max(20, mtu - 3)
        await asyncio.sleep(2.5)          # let bonding settle before an encrypted write

        await write(c, struct.pack("<I", len(json.dumps({"cmd": "nonce_auth", "nonce": nonce}).encode())) +
                    json.dumps({"cmd": "nonce_auth", "nonce": nonce}).encode(), chunk, RR_CONTROL)
        print("nonce_auth ✓")
        await asyncio.sleep(0.3)

        await c.write_gatt_char(TIME_SYNC, struct.pack("<I", epoch), response=True)
        print("TIME_SYNC  ✓")
        await asyncio.sleep(0.3)

        body = json.dumps({"cmd": "set_tz", "offset_s": offset}).encode()
        await write(c, struct.pack("<I", len(body)) + body, chunk, RR_CONTROL)
        print("set_tz     ✓")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
