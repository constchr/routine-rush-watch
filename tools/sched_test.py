#!/usr/bin/env python3
"""Phase 7 scheduler harness — pushes routines whose schedules are RELATIVE TO NOW.

Usage:
    python3 sched_test.py <nonce> <case> [minutes]

Cases:
    soon       one routine <minutes> (default 2) from now, on TODAY's weekday
    wrongday   one routine <minutes> from now, on every weekday EXCEPT today
    collide    two routines at the same minute, <minutes> from now
    pair       two routines <minutes> and <minutes>+1 from now
    none       no schedules at all

Everything is computed in the WATCH'S LOCAL time, which is this machine's local
time — the harness sends TIME_SYNC (UTC epoch) and set_tz (offset) first, so
both clocks agree before the schedule is pushed. That is the whole point: a
scheduler bug and a timezone bug look identical from the outside, so the test
pins the timezone down instead of assuming it.
"""
import asyncio
import json
import struct
import sys
import time

from bleak import BleakScanner, BleakClient

SERVICE = "fc19364a-c250-4477-928d-28c55ac1c2bd"
ROUTINE_PUSH = "8f0956d2-1818-4961-b6ed-a88844f40933"
RR_CONTROL = "73c4f178-0884-4db4-9624-ff443355763b"
TIME_SYNC = "3c5a115d-61a0-4104-9883-ebe780044eb5"

CHILD = {
    "id": "33333333-3333-4333-8333-333333333333",
    "name": "Δήμητρα",
    "language": "el",
    "avatar_id": "lion",
    "total_xp": 1250,
    "streak": 3,
    "level": 4,
}


def local_utc_offset_s() -> int:
    return -(time.altzone if time.daylight and time.localtime().tm_isdst else time.timezone)


def frame(obj) -> bytes:
    payload = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    return struct.pack("<I", len(payload)) + payload


def routine(aid, name, emoji, hhmm, days, steps=None):
    return {
        "assignment_id": aid,
        "name": name,
        "emoji": emoji,
        # Short steps: a scheduled fire is being tested, not the runtime, and a
        # 5-minute first step makes every "does it defer?" run take 5 minutes.
        "steps": steps or [
            {"id": aid + "-s1", "position": 0, "label": "Βούρτσισμα", "emoji": "🪥",
             "time_limit_s": 20, "base_xp": 10, "is_required": True},
            {"id": aid + "-s2", "position": 1, "label": "Ντύσιμο", "emoji": "👕",
             "time_limit_s": 20, "base_xp": 15, "is_required": True},
        ],
        "schedules": [{"days": days, "trigger_time": hhmm}],
    }


async def write(client, blob, chunk, char, attempts: int = 4):
    """Write, retrying while the link is still being encrypted.

    Every characteristic here is WRITE_ENC, and bonding is asynchronous: the
    first write after connecting routinely lands before encryption completes
    and comes back as ATT 0x0F. That is a race, not a rejection, so it is worth
    retrying — a real permission failure fails all four times.
    """
    for attempt in range(attempts):
        try:
            for i in range(0, len(blob), chunk):
                await client.write_gatt_char(char, blob[i:i + chunk], response=True)
            return
        except Exception as e:  # noqa: BLE001
            if attempt == attempts - 1:
                raise
            print(f"    write failed ({type(e).__name__}), link probably not encrypted "
                  f"yet — retry {attempt + 1}/{attempts - 1}")
            await asyncio.sleep(2.0)


async def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    nonce = sys.argv[1].strip()
    case = sys.argv[2].strip()
    lead = int(sys.argv[3]) if len(sys.argv) > 3 else 2

    now = time.localtime()
    iso_today = now.tm_wday + 1                       # python Mon=0 -> ISO Mon=1
    fire = time.localtime(time.time() + lead * 60)
    hhmm = f"{fire.tm_hour:02d}:{fire.tm_min:02d}"
    nxt = time.localtime(time.time() + (lead + 1) * 60)
    hhmm2 = f"{nxt.tm_hour:02d}:{nxt.tm_min:02d}"
    other_days = [d for d in range(1, 8) if d != iso_today]

    if case == "soon":
        routines = [routine("11111111-1111-4111-8111-111111111111",
                            "Πρωινή ρουτίνα", "🌅", hhmm, [iso_today])]
    elif case == "wrongday":
        routines = [routine("11111111-1111-4111-8111-111111111111",
                            "Πρωινή ρουτίνα", "🌅", hhmm, other_days)]
    elif case == "collide":
        routines = [
            routine("11111111-1111-4111-8111-111111111111", "Πρωινή ρουτίνα", "🌅",
                    hhmm, [iso_today]),
            routine("22222222-2222-4222-8222-222222222222", "Bedtime", "🌙",
                    hhmm, [iso_today]),
        ]
    elif case == "pair":
        routines = [
            routine("11111111-1111-4111-8111-111111111111", "Πρωινή ρουτίνα", "🌅",
                    hhmm, [iso_today]),
            routine("22222222-2222-4222-8222-222222222222", "Bedtime", "🌙",
                    hhmm2, [iso_today]),
        ]
    elif case == "none":
        r = routine("11111111-1111-4111-8111-111111111111", "Πρωινή ρουτίνα", "🌅",
                    hhmm, [iso_today])
        r["schedules"] = []
        routines = [r]
    else:
        print(__doc__)
        return 2

    offset = local_utc_offset_s()
    print(f"local now      : {time.strftime('%a %H:%M:%S', now)} (ISO weekday {iso_today})")
    print(f"case           : {case}")
    for r in routines:
        for s in r["schedules"]:
            print(f"  scheduling   : {r['name']!r} @ {s['trigger_time']} on days {s['days']}")
    if not any(r["schedules"] for r in routines):
        print("  scheduling   : (nothing)")
    print(f"utc offset     : {offset:+d} s")

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
        print(f"connected; MTU={mtu}, chunk={chunk}")

        # Let bonding settle before the first encrypted write.
        await asyncio.sleep(2.5)
        print()

        await write(client, frame({"cmd": "nonce_auth", "nonce": nonce}), chunk, RR_CONTROL)
        print("nonce_auth  ✓")
        await asyncio.sleep(0.3)

        # Clock BEFORE schedules: the scheduler re-arms on both, and arming
        # against a stale clock first would just produce a confusing extra line
        # in the monitor.
        epoch = int(time.time())
        await client.write_gatt_char(TIME_SYNC, struct.pack("<I", epoch), response=True)
        print(f"TIME_SYNC   ✓ epoch={epoch}")
        await asyncio.sleep(0.3)

        await write(client, frame({"cmd": "set_tz", "offset_s": offset}), chunk, RR_CONTROL)
        print(f"set_tz      ✓ {offset:+d}s")
        await asyncio.sleep(0.3)

        await write(client, frame({"child": CHILD, "routines": routines}), chunk, ROUTINE_PUSH)
        print("ROUTINE_PUSH✓\n")
        print("Now watch the monitor. Disconnecting so the watch is on its own.")

    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
