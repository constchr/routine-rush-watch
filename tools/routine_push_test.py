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
RR_CONTROL   = "73c4f178-0884-4db4-9624-ff443355763b"

ROUTINES = [
    {
        "assignment_id": "11111111-1111-4111-8111-111111111111",
        "name": "Πρωινή ρουτίνα",
        "emoji": "🌅",
        "steps": [
            # 15 s (not 120) purely so a full countdown-to-expiry can be
            # observed in one monitor capture without tapping.
            {"id": "a1", "position": 0, "label": "Βούρτσισμα", "emoji": "🪥",
             "time_limit_s": 15, "base_xp": 10, "is_required": True},
            {"id": "a2", "position": 1, "label": "Ντύσιμο", "emoji": "👕",
             "time_limit_s": 300, "base_xp": 15, "is_required": True},
            # UNTIMED: time_limit_s == 0 is the sentinel. No ring, no timer,
            # Done/Skip only (§8 screen 3).
            {"id": "a3", "position": 2, "label": "Πρωινό", "emoji": "🥣",
             "time_limit_s": 0, "base_xp": 20, "is_required": False},
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


# The child record now travels with the routines (§5 caches both). avatar_id is
# an ID from the app's AVATARS list, not an emoji — the watch maps it to a
# hero-size image.
CHILD = {
    "id": "33333333-3333-4333-8333-333333333333",
    "name": "Δήμητρα",
    "language": "el",
    "avatar_id": "lion",
    "total_xp": 1250,
    "streak": 3,
    "level": 4,
}


def frame(obj) -> bytes:
    """[u32 len][UTF-8 JSON] — the framing shared by ROUTINE_PUSH and RR_CONTROL."""
    payload = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    return struct.pack("<I", len(payload)) + payload


async def push(client, blob: bytes, chunk: int, char: str = ROUTINE_PUSH) -> bool:
    """Returns True if the peripheral accepted every chunk."""
    try:
        for i in range(0, len(blob), chunk):
            await client.write_gatt_char(char, blob[i:i + chunk], response=True)
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

        print("── TEST 1: ROUTINE_PUSH with NO prior auth — expect REJECTION")
        ok = await push(client, frame({"child": CHILD, "routines": ROUTINES}), chunk)
        print(f"    result: {'ACCEPTED' if ok else 'REJECTED'}\n")
        await asyncio.sleep(1.0)

        print(f"── TEST 2: RR_CONTROL nonce_auth with WRONG nonce ({bad}) — expect REJECTION")
        ok = await push(client, frame({"cmd": "nonce_auth", "nonce": bad}), chunk, RR_CONTROL)
        print(f"    result: {'ACCEPTED (BAD)' if ok else 'REJECTED (correct)'}\n")
        await asyncio.sleep(1.0)

        print(f"── TEST 3: RR_CONTROL nonce_auth with CORRECT nonce ({nonce})")
        ok = await push(client, frame({"cmd": "nonce_auth", "nonce": nonce}), chunk, RR_CONTROL)
        print(f"    result: {'ACCEPTED (correct)' if ok else 'REJECTED (BAD)'}\n")
        await asyncio.sleep(0.5)

        print("── TEST 4: ROUTINE_PUSH {child, routines} after auth — expect ACCEPTANCE")
        blob = frame({"child": CHILD, "routines": ROUTINES})
        print(f"    payload {len(blob)} bytes")
        ok = await push(client, blob, chunk)
        print(f"    result: {'ACCEPTED (correct)' if ok else 'REJECTED (BAD)'}")

        print("\n── TEST 5: RR_CONTROL unknown command — expect per-command rejection")
        ok = await push(client, frame({"cmd": "not_a_real_command"}), chunk, RR_CONTROL)
        print(f"    result: {'ACCEPTED (BAD)' if ok else 'REJECTED (correct)'}")

    print("\ndone — check the watch monitor for the rejection banner and the readback")
    return 0


sys.exit(asyncio.run(main()))
