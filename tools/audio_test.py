#!/usr/bin/env python3
"""set_audio harness — speaker volume + quiet hours (RR_CONTROL, additive).

Usage:
    python3 audio_test.py <nonce> volume <pct>
    python3 audio_test.py <nonce> quiet <from HH:MM> <to HH:MM> <cap pct>
    python3 audio_test.py <nonce> quiet-now <cap pct>   # window covering right now
    python3 audio_test.py <nonce> quiet-off

The watch logs the EFFECTIVE volume as it applies each change, so the monitor
shows whether the quiet-hours cap is actually in force rather than merely stored.
"""
import asyncio
import json
import struct
import sys
import time

from bleak import BleakScanner, BleakClient

SERVICE = "fc19364a-c250-4477-928d-28c55ac1c2bd"
RR_CONTROL = "73c4f178-0884-4db4-9624-ff443355763b"


async def write(client, obj, chunk, attempts: int = 4):
    body = json.dumps(obj).encode("utf-8")
    blob = struct.pack("<I", len(body)) + body
    for attempt in range(attempts):
        try:
            for i in range(0, len(blob), chunk):
                await client.write_gatt_char(RR_CONTROL, blob[i:i + chunk], response=True)
            return
        except Exception:  # noqa: BLE001
            if attempt == attempts - 1:
                raise
            await asyncio.sleep(2.0)


def build(argv):
    mode = argv[1]
    if mode == "volume":
        return {"cmd": "set_audio", "volume_pct": int(argv[2])}
    if mode == "quiet":
        return {"cmd": "set_audio", "quiet_from": argv[2], "quiet_to": argv[3],
                "quiet_volume_pct": int(argv[4])}
    if mode == "quiet-now":
        now = time.localtime()
        start = time.localtime(time.time() - 120)
        end = time.localtime(time.time() + 1800)
        return {"cmd": "set_audio",
                "quiet_from": f"{start.tm_hour:02d}:{start.tm_min:02d}",
                "quiet_to": f"{end.tm_hour:02d}:{end.tm_min:02d}",
                "quiet_volume_pct": int(argv[2])}
    if mode == "quiet-off":
        return {"cmd": "set_audio", "quiet_from": None}
    raise SystemExit(__doc__)


async def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    nonce = sys.argv[1].strip()
    cmd = build(sys.argv[1:])
    print(f"sending: {json.dumps(cmd, ensure_ascii=False)}")

    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SERVICE.lower() in [str(u).lower() for u in ad.service_uuids],
        timeout=20.0)
    if dev is None:
        print("FAIL: watch not found")
        return 1

    async with BleakClient(dev) as c:
        mtu = getattr(c, "mtu_size", 23)
        chunk = max(20, mtu - 3)
        await asyncio.sleep(2.5)
        await write(c, {"cmd": "nonce_auth", "nonce": nonce}, chunk)
        print("nonce_auth ✓")
        await asyncio.sleep(0.3)
        await write(c, cmd, chunk)
        print("set_audio  ✓ — check the monitor for the effective volume")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
