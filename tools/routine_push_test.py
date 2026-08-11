#!/usr/bin/env python3
"""ROUTINE_PUSH harness — exercises the nonce gate, both ways.

Usage:
    python3 routine_push_test.py <nonce-from-the-watch-QR>

Runs two pushes against the watch:
  1. a payload carrying a DELIBERATELY WRONG nonce  → must be REJECTED
  2. the same payload with the correct nonce        → must be ACCEPTED

Mirrors what the parent app does: envelope {"nonce":..., "routines":[...]}
framed as [u32 len][UTF-8 JSON] and chunked across MTU-sized writes.
"""
import asyncio
import json
import struct
import sys

from bleak import BleakScanner, BleakClient

SERVICE = "fc19364a-c250-4477-928d-28c55ac1c2bd"
ROUTINE_PUSH = "8f0956d2-1818-4961-b6ed-a88844f40933"

ROUTINES = [
    {
        "assignment_id": "11111111-1111-4111-8111-111111111111",
        "name": "Πρωινή ρουτίνα",
        "emoji": "🌅",
        "steps": [
            {"id": "a1", "position": 0, "label": "Βούρτσισμα", "emoji": "🪥",
             "time_limit_s": 120, "base_xp": 10, "is_required": True},
            {"id": "a2", "position": 1, "label": "Ντύσιμο", "emoji": "👕",
             "time_limit_s": 300, "base_xp": 15, "is_required": True},
            {"id": "a3", "position": 2, "label": "Πρωινό", "emoji": "🥣",
             "time_limit_s": 600, "base_xp": 20, "is_required": False},
        ],
        "schedules": [{"days": [1, 2, 3, 4, 5], "trigger_time": "07:15"}],
    },
    {
        "assignment_id": "22222222-2222-4222-8222-222222222222",
        "name": "Bedtime",
        "emoji": "🌙",
        "steps": [
            {"id": "b1", "position": 0, "label": "Pyjamas", "emoji": "🩳",
             "time_limit_s": 180, "base_xp": 10, "is_required": True},
            {"id": "b2", "position": 1, "label": "Brush teeth", "emoji": "🪥",
             "time_limit_s": 120, "base_xp": 10, "is_required": True},
        ],
        "schedules": [{"days": [0, 1, 2, 3, 4, 5, 6], "trigger_time": "19:30"}],
    },
]


def frame(nonce: str) -> bytes:
    payload = json.dumps({"nonce": nonce, "routines": ROUTINES},
                         ensure_ascii=False).encode("utf-8")
    return struct.pack("<I", len(payload)) + payload


async def push(client, blob: bytes, chunk: int) -> bool:
    """Returns True if the peripheral accepted every chunk."""
    try:
        for i in range(0, len(blob), chunk):
            await client.write_gatt_char(ROUTINE_PUSH, blob[i:i + chunk], response=True)
        return True
    except Exception as e:
        print(f"    peripheral rejected the write: {type(e).__name__}: {e}")
        return False


async def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    nonce = sys.argv[1].strip()

    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SERVICE.lower() in [str(u).lower() for u in ad.service_uuids],
        timeout=20.0)
    if dev is None:
        print("FAIL: no device advertising RR_SYNC")
        return 1
    print(f"found {dev.name!r} {dev.address}")

    async with BleakClient(dev) as client:
        mtu = getattr(client, "mtu_size", 23)
        chunk = max(20, mtu - 3)
        print(f"connected; ATT MTU={mtu}, chunk={chunk} bytes\n")

        bad = "deadbeefdeadbeef"
        print(f"── TEST 1: WRONG nonce ({bad}) — expect REJECTION")
        blob = frame(bad)
        print(f"    payload {len(blob)} bytes")
        ok = await push(client, blob, chunk)
        print(f"    result: {'ACCEPTED (BAD — gate is not working)' if ok else 'REJECTED (correct)'}\n")

        await asyncio.sleep(1.0)

        print(f"── TEST 2: CORRECT nonce ({nonce}) — expect ACCEPTANCE")
        blob = frame(nonce)
        print(f"    payload {len(blob)} bytes")
        ok = await push(client, blob, chunk)
        print(f"    result: {'ACCEPTED (correct)' if ok else 'REJECTED (BAD — good nonce refused)'}")

    print("\ndone — check the watch monitor for the rejection banner and the readback")
    return 0


sys.exit(asyncio.run(main()))
