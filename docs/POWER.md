# Power — measuring it, and what was changed

> **Phase 10 status (2026-08-13).**
>
> **Done and verified on hardware:** the PM lock that was silently blocking light
> sleep for the whole device has been found and removed — it was the **I2S audio
> driver**, not the display and not BLE. Idle advertising is at 852.5 ms, the
> wrist-raise orientation gate is live, and BLE controller sleep works.
>
> ⚠️ **Automatic light sleep itself is IMPLEMENTED BUT OFF BY DEFAULT**
> (`-DRR_LIGHT_SLEEP=1` to enable). Every PM lock is now correct — the lock dump
> with it enabled showed all of them at `Active 0` — but **LVGL's millisecond
> tick timer thrashes against the sleep entry/exit overhead and starves the IDLE
> *and* main tasks**, reproduced on hardware. A starved main task can make
> `rr_sched` miss a fire, so it is not shippable yet. See
> [Phase 10](#phase-10--light-sleep-and-the-lock-that-was-hiding-in-the-speaker).
>
> ⚠️ **No current-draw figures have been measured.** The harness and procedure are
> in place; the captures need the watch off USB for a couple of hours per
> firmware and had not been run. Every number below is an estimate and is
> labelled as one. Do not quote them as results.

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

## Two things deliberately NOT changed (in Phase 8 — the first is now done)

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

> **Phase 10 correction.** The first bullet was right; the second's *conclusion*
> was wrong. The premise is true — there is no 32.768 kHz clock available to the
> SoC — but BLE sleep does not require one. `CONFIG_BT_LE_LP_CLK_SRC_MAIN_XTAL`
> exists for exactly this board, and IDF's own help text calls it "recommended
> if external 32.768k XTAL is not available". It is now set, and
> `esp_pm_dump_locks()` confirms the controller's `bt` lock sits at `Active 0`
> while idle — the controller really is sleeping between advertising events.
> The cost is honest and worth naming: without a 32 kHz crystal, light sleep
> cannot power down the main XTAL, so the sleeping floor is higher than a
> crystal-equipped board would reach.

---

## Phase 10 — light sleep, and the lock that was hiding in the speaker

### The symptom, and why the old evidence was worthless

The heartbeat printed `40-160MHz ls=0`, which was read as "light sleep is not
engaging". It never said that. The value came from `esp_pm_get_configuration()`,
which just echoes back the config the firmware itself had passed in — and that
config said `light_sleep_enable = false`. **`ls=0` meant "we never asked", not
"we asked and were refused".** A flag that can only ever report your own input
looks like telemetry and carries none.

`rr_pm_describe()` replaces it with counters that can actually be wrong:
`ls <n>` is the number of light sleeps the chip really entered (via
`CONFIG_PM_LIGHT_SLEEP_CALLBACKS`), and `slp <n>%` is the share of wall clock
spent in them. A lock left held by mistake now shows as a number that stops
moving, instead of a flag that still cheerfully reads 1.

### What was actually blocking it

Light sleep was switched on, the display lock was releasing correctly, the BLE
controller was sleeping — and `ls` stayed at **0**. `esp_pm_dump_locks()` gave
the answer in one line:

```
Name            Type            Arg    Active
i2s_driver      APB_FREQ_MAX    0      1        <-- held forever
i2s_driver      APB_FREQ_MAX    0      1        <-- held forever
bt              APB_FREQ_MAX    0      0
I2C_0           NO_LIGHT_SLEEP  0      0
spi_master      APB_FREQ_MAX    0      0
rr_display      NO_LIGHT_SLEEP  0      0
rtos0           CPU_FREQ_MAX    0      1        (normal — does not block sleep)
```

**`bsp_audio_init()` calls `i2s_channel_enable()` on both the TX and RX channels
at boot and never disables them**, and `i2s_channel_enable()` takes an
`ESP_PM_APB_FREQ_MAX` lock. So from boot onwards the chip held two of them, and
automatic light sleep could not engage for a single millisecond — on a device
whose speaker is used for a few seconds a day.

It was completely invisible from the outside. Audio worked, the display slept,
BLE behaved, the config said light sleep was on, and the only symptom was a
battery that drained as though none of the power work existed.

**The lesson is the dumpable one:** on this SoC a held PM lock is silent by
construction. `rr_pm_dump_locks()` exists so the next person spends one boot on
this question instead of an afternoon.

### The fix

The BSP keeps both I2S handles in file-statics with no accessor and no deinit —
the same trap this document already records for the panel handle — so the
channels cannot be disabled from outside. `rr_audio` therefore owns its own I2S
bring-up now (`audio_hw_init()`), as a faithful transcription of
`bsp_audio_init()` + `bsp_audio_codec_speaker_init()` with two differences:

1. **No RX channel is created at all.** v1 never uses the microphone, so the BSP
   was holding a PM lock and DMA buffers for a peripheral nothing reads.
2. **The channel is not enabled at init.** It does not need to be:
   `esp_codec_dev`'s I2S data interface already calls `i2s_channel_enable()` on
   open and `i2s_channel_disable()` on close, and the player opens and closes per
   clip. The lock is now held for the ~2 s a sound plays instead of forever, and
   **the playback path did not change at all**.

### ⚠️ Why light sleep is still off: the LVGL tick

With the I2S locks released, every sleep-blocking lock reads `Active 0` while
idle — the lock problem is genuinely solved. Enabling light sleep anyway produced
this, at five and a half minutes of uptime, repeating every 5 s:

```
E (339487) task_wdt: Task watchdog got triggered ... IDLE (CPU 0)
E (339487) task_wdt: Tasks currently running: CPU 0: esp_timer
```

with the **30 s heartbeat absent entirely** — so the main task was starved too,
not just the idle task. The counters said the same thing from the other side:
`ls 1 slp 0%`, i.e. it entered light sleep and got essentially no sleep from it.

**Cause — HYPOTHESIS, NOT ESTABLISHED.** Be careful with this section; it is the
one part of this document that is not backed by a measurement.

The leading theory is that automatic light sleep pays a fixed entry/exit cost,
and LVGL drives its tick from an `esp_timer` whose period is single-digit
milliseconds — the same order as that overhead — running whether or not anything
is on screen. That would make the scheduler thrash: sleep, wake on the tick
almost immediately, sleep again, never completing an idle pass. `CPU 0:
esp_timer` in every watchdog dump is consistent with it.

**But it was never proven, and one observation argues against jumping to it:** an
unrelated change (leaving/re-entering wake-on-motion from the IMU interrupt task,
since reverted) produced the *same* `IDLE (CPU 0)` / `CPU 0: esp_timer`
signature **with light sleep switched off entirely**. So this watchdog signature
is not specific to light sleep, and may indicate I2C or task-priority contention
rather than sleep thrash. Whoever picks this up should **confirm the cause before
building a fix on it** — e.g. by enabling `CONFIG_PM_PROFILING`, or simply by
stopping the LVGL tick timer and seeing whether `slp%` moves.

It is **not** the 25 Hz step sampler, which was the obvious suspect: a 40 ms
period leaves plenty of room for a worthwhile sleep.

**Candidate fix, unverified.** Stop LVGL's tick and refresh timers while the panel
is dark and restore them in `wake_up()`. This was deliberately deferred during
the main change to avoid a display regression — Phase 4b's "went unresponsive"
incident is what careless display surgery looks like.

**Until then `-DRR_LIGHT_SLEEP` stays off**, because a starved main task can make
`rr_sched` miss a scheduled routine, and a watch that saves power but misses a
routine is a failure.

### The other locks, and who holds them

| Lock | Holder | When held |
|---|---|---|
| `rr_display` | `rr_pm`, ours | Only while the panel is lit. Taken first in `wake_up()`, released last in `go_to_sleep()`, so no QSPI/I2C transfer can be issued in a state where the clocks might stop under it. |
| `i2s_driver` | `esp_codec_dev` | Now only while a clip is playing. |
| `spi_master`, `I2C_0` | IDF drivers | Per transaction — they were never the problem. |
| `bt` | BLE controller | Between advertising events only, now that `BT_LE_SLEEP_ENABLE` is set. |
| `usb_serial_jtag` | IDF | Whenever USB is attached — see below. |

### ⚠️ USB inhibits light sleep, deliberately

USB-Serial-JTAG cannot operate across light sleep: the CDC link drops and the
log simply stops. The first light-sleep build went dark about 9 s after boot —
the moment the screen slept — and **a silent console is indistinguishable from a
crash**. Worse, the sleeping chip stops answering `esptool`, so the board could
not be re-flashed without the BOOT-button recovery below.

`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y` makes IDF hold a `NO_LIGHT_SLEEP` lock
for as long as a USB host is attached. This costs nothing that matters: VBUS
present means the watch is on a charger, which is the one state where draw is
irrelevant — and `rr_powerlog` already refuses to measure on USB for the same
reason.

**Consequence for verification: `ls`/`slp%` read 0 for the whole time the watch
is plugged in, and that is correct rather than a regression.** Because the
counters are cumulative, the way to prove sleep works is:

1. Unplug. Leave it 10+ minutes, screen off, sitting still.
2. Replug and read the next heartbeat.
3. `ls` and `slp%` describe what happened **while the cable was out**.

### If the board stops answering esptool

A watch running a light-sleep build from **before** `USJ_NO_AUTO_LS_ON_CONNECTION`
was added will refuse to flash (`Failed to connect to ESP32-C6: No serial data
received`). Recover it manually:

1. Hold **BOOT** (GPIO9).
2. While holding it, unplug and replug USB-C.
3. Release BOOT **within a second or two** of replugging.
4. Flash normally.

Releasing promptly matters: BOOT held for 10 s *by the running application* is
the local factory reset. In ROM download mode the application never runs, so
this is safe — but only once download mode has actually been entered.

Firmware from this phase onward does not have the problem, because USB now
inhibits sleep.

### Wake sources — the part that is easy to get fatally wrong

`CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP` is set, so **an ordinary GPIO
interrupt does not run during light sleep** — only a registered wake source
brings the chip back. Two pins therefore had to be registered explicitly, and
both would have failed silently on the bench (where the watch is awake and
plugged in, hence never sleeping):

- **QMI8658 INT1 (GPIO16)** — raise-to-wake. Without this the watch sleeps
  beautifully and never wakes on a wrist, which is the most expensive possible
  way to be wrong. Light-sleep GPIO wakeup is **level**-triggered only, so
  `rr_imu` derives the active level from the pin's observed resting level after
  the arming transient rather than assuming the datasheet default.
- **BOOT (GPIO9)** — the factory reset, and the *only* recovery for a watch
  unlinked while out of BLE range. A reset button that quietly stopped working
  once power management was enabled would be discovered by a parent.

`CONFIG_PM_SLP_DISABLE_GPIO` powers every pad down during sleep to save
200–300 µA, so both pins also call `gpio_sleep_sel_dis()` to opt out.

### ⚠️ The IMU never reboots when the MCU does — and step counting was dead

**The most important finding of this phase, and it is not about power — it is
about correctness. Step counting had never worked.**

The QMI8658 keeps its configuration across an ESP32 reset, a reflash and a
watchdog reboot; only a full power cycle clears it. `rr_imu_init()` wrote range
and ODR over the top of whatever was already there and assumed the rest was
default — so a part left in a non-converting state by an earlier firmware stayed
that way through every reflash.

The signature is badly misleading:

```
ax=-32768 ay=-32768 az=-32768          <- 0x8000 on every axis
  -> x=-2.00 y=-2.00 z=-2.00  |a|=3.46 g
CTRL2=0x07 (2G, 62.5 Hz)  CTRL7=0x01 (accel on)    <- all CORRECT
STATUS0=0x00                                        <- never data-ready
```

`0x8000` is the "no valid sample" sentinel and at 2G scales to exactly −2.00 g,
so it reads as a *saturated* accelerometer rather than an absent one. Every
control register says healthy, nothing errors, and the only real tell is that
the values never change — which is indistinguishable from a watch nobody moved.

**The fix** is a soft reset (`0xB0` → register `0x60`) at bring-up, before any
configuration. After it, `STATUS0` asserts and the counts show real noise:

```
sample 1: ax=  -115 ay=   969 az= 15886   STATUS0=0x01
sample 2: ax=  -115 ay=   974 az= 15889
heartbeat: accel 0.97g sprd 2.586 n=758   <- no FROZEN flag
```

**A correction worth recording.** This was first blamed on wake-on-motion —
"arming WoM freezes the output registers" — because it was first seen while WoM
was armed. That was wrong, or at least never demonstrated: the same frozen
`0x8000` appears with WoM entirely absent from the firmware, and a soft reset
fixes it either way. The sensor had been left in a bad state by a previous boot,
so the correlation with arming was incidental. **If wake-on-motion is ever wanted
again, it is not ruled out on those grounds** — re-test against a freshly reset
part.

**Guarding against a repeat.** `rr_steps_describe_input()` now reports the
*spread* of the magnitudes feeding the detector, in the heartbeat
(`accel 0.97g sprd 2.586 n=758`, flagged `⚠FROZEN` at zero spread). A live
accelerometer at rest still jitters by a few mg; a dead one reports an identical
value forever. Without that, "steps unchanged" is ambiguous between a dead sensor
and a stationary watch — the exact ambiguity that hid this for two phases.
`rr_imu_init()` also dumps five spaced raw samples at boot and shouts if they are
byte-identical.

**Wake-on-motion is gone regardless**, by product decision: the screen is woken by
a short press on BOOT or by the scheduler. That keeps the accelerometer in one
continuous sampling mode all day and removes a class of mode-switching bugs.

### The Phase 10 levers

Listed separately because they are independent — each can be reverted on its own,
and each should be attributable in the measurements.

| Lever | Where | Est. saving | Confidence |
|---|---|---|---|
| I2S channels no longer held enabled | `rr_audio.c` `audio_hw_init()` | Enables everything below; ~0 on its own | high (lock dump is direct evidence) |
| Automatic light sleep + display lock | `rr_pm.c`, `rr_idle.c` | The large one — CPU idles instead of running | medium (floor raised by main-XTAL BLE clock) |
| BLE controller sleep | `CONFIG_BT_LE_SLEEP_ENABLE` | 1–3 mA while advertising | medium |
| Idle advertising 417.5 → 852.5 ms | `rr_ble.c` | 0.5–1.5 mA | high (duty cycle halves) |
| Wrist-raise gate (fewer AMOLED wakes) | `rr_imu.c` | Large but usage-dependent | high (AMOLED is the dominant burst draw) |
| RX (microphone) I2S channel never created | `rr_audio.c` | small, plus DMA RAM | high |

### Measured results

**NOT YET CAPTURED.** The harness, the counters and the procedure are ready; the
runs need the watch off USB for ~2 h per firmware. Fill this table in from
`POWERLOG` output — and note that comparing the same state on the same cell is
what makes these trustworthy, so keep the conditions identical.

| State | Before (Phase 8) | After (Phase 10) | Notes |
|---|---|---|---|
| idle-asleep (screen off, paired, queue empty, still) | _pending_ | _pending_ | The number that decides the battery story |
| `ls` / `slp%` after 10 min unplugged | 0 / 0% (by construction) | _pending_ | Proof sleep engages at all |
| idle-awake (screen on) | _pending_ | _pending_ | Should be ~unchanged — the display lock inhibits sleep here by design |
| during a BLE sync | _pending_ | _pending_ | |

Estimated battery life for a realistic day (mostly asleep, a few wakes, 2
routines, a couple of syncs) — **also pending**, and it should be computed from
the idle-asleep measurement rather than predicted, because idle dominates.

**What can be said without the captures**, and no more than this:

- The CPU duty cycle while "asleep" is now bounded by the 25 Hz step sampler:
  ~0.5 ms of work per 40 ms window, so the chip is a candidate for light sleep
  ~98% of the time. Whether that converts into current depends on the light
  sleep floor, which is raised on this board by the main-XTAL BLE clock.
- Idle advertising transmissions dropped 2× (417.5 → 852.5 ms).
- The AMOLED wake rate should fall sharply from the wrist-raise gate, and that
  is measurable directly from the heartbeat's `raise <confirmed>/<interrupts>`
  ratio without any current measurement at all.

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
