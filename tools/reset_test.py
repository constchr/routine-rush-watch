#!/usr/bin/env python3
"""Sends { "cmd": "factory_reset" } over RR_CONTROL (contract v2).

Trust gate: the watch accepts this only from an encrypted link whose peer
identity matches the peer that completed the pairing nonce handshake.
"""
import asyncio, json, struct, sys
from bleak import BleakScanner, BleakClient

SERVICE = "fc19364a-c250-4477-928d-28c55ac1c2bd"
RR_CONTROL = "73c4f178-0884-4db4-9624-ff443355763b"

async def main():
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SERVICE.lower() in [str(u).lower() for u in ad.service_uuids], timeout=20.0)
    if dev is None:
        print("FAIL: watch not found"); return 1
    print(f"found {dev.name!r} {dev.address}")
    async with BleakClient(dev) as c:
        mtu = getattr(c, "mtu_size", 23); chunk = max(20, mtu - 3)
        body = json.dumps({"cmd": "factory_reset"}).encode()
        blob = struct.pack("<I", len(body)) + body
        print(f"sending factory_reset ({len(blob)} bytes, chunk {chunk})")
        try:
            for i in range(0, len(blob), chunk):
                await c.write_gatt_char(RR_CONTROL, blob[i:i+chunk], response=True)
            print("ACCEPTED — watch should be wiping and rebooting")
        except Exception as e:
            print(f"REJECTED: {type(e).__name__}: {e}")
    return 0
sys.exit(asyncio.run(main()))
