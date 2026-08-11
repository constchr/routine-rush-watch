# docs

The firmware specification is **not** duplicated here — a second copy would
drift from the original and there would be no way to tell which was current.

**Source of truth:**

    routine-rush/docs/RoutineRush_Watch_Firmware_Spec.md

Sections most relevant to this repo:

| Section | What |
|---|---|
| §2, §2.1 | Hardware inventory and the RAM ceiling (no PSRAM) |
| §2.2 | Why Wi-Fi is disabled |
| §5 | Local storage: cache + completion queue |
| §6, §6B | Sync engine and the BLE relay transport |
| **§6B.3** | **FROZEN GATT contract** — mirrored into `ble_contract.h` |
| §7 | Scheduler (audio alarm — no haptic on this board) |
| §9B | Idle watch face |
| §10, §10B | Power / raise-to-wake, and the ES8311 audio subsystem |
| §11.0 | Board resources, toolchain version, factory image recovery |
| **§12** | **Phase 0 verified-on-hardware findings** |
| §12.1 | Firmware phase plan |

Anything discovered here that contradicts the spec should be fixed **in the
spec**, in the app repo — not documented only in this repo.
