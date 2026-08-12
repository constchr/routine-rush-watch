#!/usr/bin/env python3
"""start_routine harness — remote start over RR_CONTROL.

Usage:
    python3 start_routine_test.py <routine-assignment-id>

Sends { "cmd": "start_routine", "routine_id": "<id>" } framed as
[u32 len][UTF-8 JSON], the same envelope as every other RR_CONTROL command.

── WHAT THIS MACHINE CAN AND CANNOT PROVE ──────────────────────────────────

The watch accepts start_routine ONLY from the peer it paired with — the
parent's phone. A laptop is not that peer, so from here the EXPECTED result
is a rejection with ATT 0x05, and that rejection is the point: it is the
proof that a stray central which has merely bonded (Just Works lets anything
bond — confirmed on hardware in Phase 2) cannot start a routine on a child's
wrist.

The started / busy outcomes need the paired phone, because they are what the
gate exists to let through. Run those from the parent app.

Reading the result: bleak surfaces the ATT status in the exception text.
    0x05  not the paired peer                (expected here)
    0x06  firmware predates start_routine    (flash it)
    0x80  busy — a routine is already running
    0x81  that routine_id is not cached — push routines first
    none  accepted; the watch is starting it now
"""
import asyncio
import json
import struct
import sys

from bleak import BleakScanner, BleakClient

SERVICE = "fc19364a-c250-4477-928d-28c55ac1c2bd"
RR_CONTROL = "73c4f178-0884-4db4-9624-ff443355763b"

ATT_MEANING = {
    0x05: "not the paired peer (or link unencrypted)",
    0x06: "unknown command — firmware predates start_routine",
    0x80: "BUSY — a routine is already running and was not interrupted",
    0x81: "UNKNOWN ROUTINE — not in the watch's cache; push routines first",
}


def explain(err: Exception) -> str:
    text = str(err).lower()
    for code, meaning in ATT_MEANING.items():
        # CoreBluetooth reports the status as a DECIMAL "Code=5" inside a
        # CBATTErrorDomain string, not as hex — observed against this watch.
        # BlueZ words it differently again, so match several renderings.
        if (f"code={code}" in text
                or f"0x{code:02x}" in text
                or f"error {code}" in text):
            return f"ATT 0x{code:02x} — {meaning}"
    return "unrecognised ATT status"


async def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    routine_id = sys.argv[1]

    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SERVICE.lower() in [str(u).lower() for u in ad.service_uuids],
        timeout=20.0,
    )
    if dev is None:
        print("FAIL: watch not found (out of range, or asleep and not advertising)")
        return 1
    print(f"found {dev.name!r} {dev.address}")

    async with BleakClient(dev) as c:
        mtu = getattr(c, "mtu_size", 23)
        chunk = max(20, mtu - 3)
        body = json.dumps({"cmd": "start_routine", "routine_id": routine_id}).encode()
        blob = struct.pack("<I", len(body)) + body
        print(f"sending start_routine {routine_id} ({len(blob)} bytes, chunk {chunk})")
        try:
            for i in range(0, len(blob), chunk):
                await c.write_gatt_char(RR_CONTROL, blob[i : i + chunk], response=True)
            print("ACCEPTED — the watch should be waking and showing step 1")
            print("NOTE: accepted from a laptop means the paired-peer gate is NOT "
                  "holding. That is a bug worth chasing.")
        except Exception as e:  # noqa: BLE001 — the ATT status is the result
            print(f"REJECTED: {type(e).__name__}: {e}")
            print(f"  → {explain(e)}")
    return 0


sys.exit(asyncio.run(main()))
