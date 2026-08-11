# routine-rush-watch

ESP32-C6 firmware for the **Routine Rush watch companion** — a Waveshare
ESP32-C6-Touch-AMOLED-2.06 running the full routine loop on-device.

The watch is a **peer** of the kids tablet app, not an accessory. A child with
only the watch can follow their routines with no tablet or phone nearby;
completions queue durably in flash and relay via BLE whenever a parent phone
appears.

This repo is **firmware only**. It is deliberately separate from the
`routine-rush` React Native monorepo — different toolchain, different CI,
different release artifacts (binary OTA images, not app-store builds).

---

## Requirements

| | |
|---|---|
| **ESP-IDF** | **v5.5+** — v5.5.5 is the verified baseline |
| Target | `esp32c6` |
| Board | Waveshare ESP32-C6-Touch-AMOLED-2.06 |
| Node | 20+ (only for the BLE contract generator) |

> **v5.4.x will not work.** The board BSP
> `waveshare/esp32_c6_touch_amoled_2_06 ^2.0.0` declares `idf: ">=5.5.0"` and
> the dependency solver fails outright on older IDF.

## Build & flash

```sh
source ~/esp/v5.5.5-idf/export.sh     # or wherever your IDF v5.5.5 lives

node tools/gen-ble-contract.mjs        # generate ble_contract.h (not committed)
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor
```

The board enumerates over its built-in USB-Serial-JTAG — one USB-C cable, no
adapter, no driver. On macOS it appears as `/dev/cu.usbmodem101`.

---

## Phase 0 hardware findings

Board bring-up is complete and signed off. These are things the physical board
actually did, not datasheet claims — several contradict what the spec
originally assumed.

### ⚠️ There is no vibration motor

Verified absent from the schematic, Waveshare's pin audit, and their product
spec table. **All alerting is audio + screen wake.** This is why `rr_audio`
(Phase 8) is on the critical path and Phase 7 (scheduler) depends on it — a
scheduler that fires silently is a routine nobody notices.

Consequence: because the alert is *audible* rather than a silent buzz, volume
and quiet hours are product requirements. "Silent mode" on this board can only
mean visual-only.

### ⚠️ The console must stay on USB-Serial-JTAG

Waveshare's examples route the console to UART on **GPIO 16/17** — which are
the **QMI8658 INT1/INT2** pins that raise-to-wake and the pedometer need.

The failure mode is silent: nothing errors, the IMU still reads fine over I2C,
and the board looks healthy — but the interrupt lines are taken, so wake-on-
motion degrades to CPU polling and the battery budget quietly collapses. It
would present much later as "the wake gesture is just unreliable".

`sdkconfig.defaults` pins the console to USB-Serial-JTAG, and CI asserts it.

### Confirmed working

| Peripheral | Detail |
|---|---|
| AMOLED display | 410×502 QSPI. Driven by the **SH8601** component — the panel is a CO5300 and the two are register-compatible |
| Touch | **FT3168** via `esp_lcd_touch_ft5x06`, registered as an LVGL input device |
| IMU | QMI8658 @ 0x6B, `WHO_AM_I=0x05`, correct gravity vector, tracks real motion |
| RTC | PCF85063 @ 0x51, ticking (ships unset — set it from `TIME_SYNC`) |
| Audio | ES8311 @ 0x18, **test tone audible from the onboard speaker**. PA enable is GPIO 6, driven by `esp_codec_dev` — do not toggle it yourself |
| AEC ADC | ES7210 @ 0x40 (unused in v1) |
| PMIC | AXP2101 @ 0x34, chip ID 0x4A, reports ~4.10 V / 82 %, vbus + battery-present flags |

### RAM is the ceiling

**~274 KiB free at boot.** No PSRAM. A full-screen 410×502 16bpp framebuffer is
~400 KiB — larger than all available RAM. LVGL renders in 50-line bands; see
the comments in `sdkconfig.defaults` before changing that.

### Factory image

The board shipped running the `esp-brookesia` demo. The original 16 MB flash is
archived at `~/esp/backup/factory-flash-16MB-2a3608d.bin`:

```sh
esptool.py -p /dev/cu.usbmodem101 write_flash 0x0 ~/esp/backup/factory-flash-16MB-2a3608d.bin
```

---

## Layout

```
main/              boot, event loop, screen router
components/
  rr_ble/          RR_SYNC GATT server — the sole uplink      (Phase 1)
  rr_store/        LittleFS cache + durable completion queue  (Phase 3/5)
  rr_sync/         sync engine: pull / push / reconcile       (Phase 5)
  rr_routine/      routine runtime, single-step-focus loop    (Phase 4)
  rr_ui/           LVGL screens + idle watch face             (Phase 4/6)
  rr_audio/        ES8311 effect + jingle playback            (Phase 8)
  rr_steps/        QMI8658 pedometer + daily count            (Phase 9)
  rr_power/        sleep, raise-to-wake, battery              (Phase 10)
assets/            WAV clips, fonts, emoji
tools/             BLE contract vendoring + generator
```

Every module is currently an **empty stub** — a `CMakeLists.txt` and a header
documenting the intended interface. Nothing is implemented.

## The BLE contract

`components/rr_ble/include/ble_contract.h` is **generated, not committed**. It
is produced from `tools/watchProtocol.ts`, a vendored copy of
`routine-rush/packages/ble/src/watchProtocol.ts`.

**The app repo is the source of truth.** Spec §6B.3 marks that contract FROZEN;
the phone and this firmware implement opposite ends of the same byte layouts.

```sh
node tools/gen-ble-contract.mjs          # regenerate
node tools/gen-ble-contract.mjs --check  # CI: fail if stale
```

To change the protocol: edit the app repo, re-vendor the copy here, regenerate,
and land both sides together. The generator refuses to emit a header if the
contract's documented field offsets don't sum to its exported size constants,
so a self-inconsistent contract fails the build rather than baking a wrong
offset into firmware.

## Spec

The firmware specification lives in the app repo and is **not** duplicated here:

```
routine-rush/docs/RoutineRush_Watch_Firmware_Spec.md
```

See §12.1 for the phase plan, §6B.3 for the frozen GATT contract, and the
"Phase 0 verified on hardware" block in §12 for the findings summarised above.
