#!/usr/bin/env python3
"""Sends { "command": "factory_reset" } over ROUTINE_PUSH.

Trust gate: the watch accepts this only from an encrypted link whose peer
identity matches the peer that completed the pairing nonce handshake.
"""
import asyncio, json, struct, sys
from bleak import BleakScanner, BleakClient

SERVICE = "fc19364a-c250-4477-928d-28c55ac1c2bd"
ROUTINE_PUSH = "8f0956d2-1818-4961-b6ed-a88844f40933"

async def main():
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SERVICE.lower() in [str(u).lower() for u in ad.service_uuids], timeout=20.0)
    if dev is None:
        print("FAIL: watch not found"); return 1
    print(f"found {dev.name!r} {dev.address}")
    async with BleakClient(dev) as c:
        mtu = getattr(c, "mtu_size", 23); chunk = max(20, mtu - 3)
        body = json.dumps({"command": "factory_reset"}).encode()
        blob = struct.pack("<I", len(body)) + body
        print(f"sending factory_reset ({len(blob)} bytes, chunk {chunk})")
        try:
            for i in range(0, len(blob), chunk):
                await c.write_gatt_char(ROUTINE_PUSH, blob[i:i+chunk], response=True)
            print("ACCEPTED — watch should be wiping and rebooting")
        except Exception as e:
            print(f"REJECTED: {type(e).__name__}: {e}")
    return 0
sys.exit(asyncio.run(main()))
