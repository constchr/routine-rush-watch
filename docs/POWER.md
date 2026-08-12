# Power — measuring it, and what was changed

## Why measurement is awkward on this board

There is no current-sense hardware, and **the AXP2101 has no battery-current
ADC** — it exposes battery voltage (reg `0x34`) and a fuel-gauge percentage (reg
`0xA4`), nothing more. The AXP192 it replaced did have one; this part does not.

So every milliamp figure in the Phase 10 audit is a **datasheet estimate with a
±50% margin**. The rankings are solid (they come from file/line evidence about
what runs how often); the absolute numbers are not.

What the fuel gauge *can* give is a **rate of fall**. The cell is ~400 mAh, so
1% ≈ 4 mAh, and a state drawing 10 mA loses a percent every ~24 minutes. Hold a
state for an hour and the arithmetic gives a real number.

Accuracy: expect ±20–30% on a one-hour hold — the gauge reports whole percent,
its coulomb counting is approximate, and 400 mAh is nominal. But it is
*consistent*, so a **before/after comparison of the same state on the same cell**
is far more trustworthy than either absolute reading. That comparison is the
point.

## The harness

Compiled in only when asked:

```bash
idf.py -DRR_POWERLOG=1 build
idf.py -p /dev/cu.usbmodem1101 app-flash
```

It samples the PMIC every 60 s and logs one `POWERLOG` line per sample. Once at
least one whole percent has been lost it also reports the rate and the implied
current:

```
POWERLOG t=1.05h soc=93% mv=3921 state="idle-after" | dropped 5% -> 4.76 %/h ≈ 19.0 mA | full charge ≈ 21.0 h
```

`rr_powerlog_mark("label")` re-baselines to now — call it when a new state has
just been established so the rate describes the state and not the transition
into it.

**It refuses to measure on USB.** With VBUS present the PMIC is charging or
floating and the percentage will not fall, so it logs `ON USB, NOT MEASURING`
rather than printing a meaningless zero. If USB is seen at any point after the
baseline, later lines carry `⚠ USB SEEN SINCE BASELINE — SAMPLE INVALID`.

## Procedure: one before/after number for idle draw

The measurement that matters is **idle**, because that is where the day goes.

### Before

1. Charge to 100%, then **unplug** — the whole run is on battery.
2. Flash the pre-change firmware with `-DRR_POWERLOG=1`.
3. Put the watch in the state under test: paired, routines cached, **queue
   empty**, screen allowed to sleep, sitting still (no wrist movement, or the IMU
   wakes the screen and you are measuring something else).
4. Leave it **at least 1 hour**, ideally 2–3. Longer is strictly better: the
   quantisation error is one percent regardless of duration, so it shrinks as a
   fraction of the total.
5. Reconnect USB **only at the end** and capture the console. The `POWERLOG`
   lines are retained in the log; the last one before you plugged in is the
   answer.

Because reading the log needs USB and USB stops the measurement, either capture
continuously from a host that is *powering nothing* (not possible here — USB
powers the board) or accept reading it at the end. Reading at the end is fine:
every line is timestamped and the rate is cumulative from the baseline.

### After

Repeat identically with the new firmware. Same cell, same starting charge, same
still-on-a-desk conditions, same duration.

### Interpreting

Compare the `≈ N mA` figures. A change smaller than ~25% is inside the noise of
this method — treat it as "no measured difference" rather than a small win.

## What changed in this batch, and what to expect

| Change | Where | Est. saving | Confidence |
|---|---|---|---|
| Advertising 30–60 ms → 1000–1500 ms when idle | `rr_ble.c` `advertise()` | 3–6 mA | high (duty cycle drops ~20×) |
| Relaxed connection interval (90–150 ms, latency 4) | `rr_ble.c` on connect | 1–3 mA while connected | medium (central may refuse) |
| Touch no longer polled while the panel is dark | `rr_idle.c` `go_to_sleep()` | 0.5–1.5 mA | medium |
| Face gate cached (no littlefs read at 2 Hz) | `main.c` `watchface_allowed()` | small mA, large flash-idle win | high |
| Reset button interrupt-gated (was 10 Hz) | `rr_reset_button.c` | small | high |
| Heartbeat 5 s → 30 s | `main.c` | small | high |
| DFS enabled (`esp_pm_configure`, 40–160 MHz) | `main.c` | 1–3 mA | medium |

Rough expectation: **12–20 mA → 6–11 mA idle**, i.e. ~20–33 h → ~35–65 h. That
prediction is exactly what the harness is for.

## Two things deliberately NOT changed

**Automatic light sleep is off**, and that is a hardware conclusion:

- `CONFIG_BT_LE_SLEEP_ENABLE` is not set, so the BLE controller holds the main
  XTAL up. Automatic light sleep would buy little while advertising — which is
  always — and risks BLE timing.
- Enabling BLE controller sleep properly wants a 32.768 kHz low-power clock, and
  **this board cannot give the SoC one**: the ESP32-C6's `XTAL_32K` pins are
  GPIO0/GPIO1, which here are `QSPI_SCL` and `QSPI_SIO0` for the display (vendor
  pin audit). The 32 kHz crystal on the board belongs to the PCF85063 RTC. Hence
  `CONFIG_RTC_CLK_SRC_INT_RC`.

So light sleep is a separate exercise with the BLE controller config in scope,
measured — not a flag to flip. DFS is enabled because it is safe and independent.

**The panel driver IC is still powered when the screen is "off".**
`bsp_display_backlight_off()` is `brightness_set(0)` — MIPI command `0x51` with
value 0 — so AMOLED emission current (the dominant term) does go to ~zero, but
the SH8601 driver and its boost converter keep running. `DISPOFF` (0x28) would
fix that and the driver supports it (`esp_lcd_sh8601.c:343`), **but the panel
handle is unreachable from application code**: the BSP keeps it in a file-static
and exposes no accessor (`bsp_display_start()` returns only an `lv_display_t*`).
Getting it means either reimplementing `bsp_display_start()` — including the
panel's `rounder_event_cb` x-alignment quirk that everything on screen depends on
— or patching a managed component that dependency resolution will overwrite.

Estimated 2–5 mA, entirely from a datasheet figure for a driver IC whose idle
draw cannot be verified here. **Measure first**: run the harness, and if idle
lands near the low end of the prediction the remaining headroom may not be in the
panel at all. If it is worth doing, the clean route is to take ownership of
display bring-up in `rr_ui_init()` deliberately, as its own change with its own
visual regression check.
