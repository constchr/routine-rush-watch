#!/usr/bin/env python3
"""Host-side TIME_SYNC test — does exactly what the parent app's syncTime() does.

Scans for the RR_SYNC service, connects, and writes a 4-byte little-endian
epoch to the TIME_SYNC characteristic (write-with-response).

Purpose: prove the FIRMWARE works independently of the React Native app, so a
later failure on the phone can be attributed to the app, not the watch.
"""
import asyncio
import struct
import sys
import time

from bleak import BleakScanner, BleakClient

SERVICE = "fc19364a-c250-4477-928d-28c55ac1c2bd"
TIME_SYNC = "3c5a115d-61a0-4104-9883-ebe780044eb5"


async def main():
    print(f"scanning for RR_SYNC ({SERVICE}) ...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SERVICE.lower() in [str(u).lower() for u in ad.service_uuids],
        timeout=20.0,
    )
    if dev is None:
        print("FAIL: no device advertising RR_SYNC found")
        return 1
    print(f"found: {dev.name!r}  {dev.address}")

    async with BleakClient(dev) as client:
        print(f"connected: {client.is_connected}")

        svcs = client.services
        chars = [c.uuid.lower() for s in svcs for c in s.characteristics]
        print("characteristics discovered:")
        for s in svcs:
            for c in s.characteristics:
                print(f"  {c.uuid}  props={c.properties}")

        if TIME_SYNC not in chars:
            print(f"FAIL: TIME_SYNC {TIME_SYNC} not found")
            return 1

        epoch = int(time.time())
        payload = struct.pack("<I", epoch)   # little-endian u32, per the contract
        print(f"writing TIME_SYNC epoch={epoch} bytes={payload.hex()}")
        await client.write_gatt_char(TIME_SYNC, payload, response=True)
        print("write acknowledged by the peripheral")

        # Negative check: the firmware must reject a wrong-length payload.
        try:
            await client.write_gatt_char(TIME_SYNC, b"\x01\x02", response=True)
            print("WARN: 2-byte write was ACCEPTED — length validation is not working")
        except Exception as e:
            print(f"good: short write rejected ({type(e).__name__})")

    print("done")
    return 0


sys.exit(asyncio.run(main()))
