# Power — measuring it, and what was changed

> **Phase 10 status (2026-08-13).**
>
> **Done and verified on hardware:** the PM lock that was silently blocking light
> sleep for the whole device has been found and removed — it was the **I2S audio
> driver**, not the display and not BLE. Idle advertising is at 852.5 ms, the
> wrist-raise orientation gate is live, and BLE controller sleep works.
>
> ⚠️ **Automatic light sleep is IMPLEMENTED, WORKS, and is still OFF BY DEFAULT**
> (`-DRR_LIGHT_SLEEP=1`). Measured unplugged, 2026-08-17:
>
> | Build | Entries | Mean sleep | **Residency** |
> |---|---|---|---|
> | stock 5 ms LVGL tick | 1 328 906 | 330 µs | **65.0%** |
> | `-DRR_LVGL_TICK_MS=40` | 395 284 | 3.75 ms | **92.7%** |
>
> **The LVGL tick is the mechanism, but it governs sleep LENGTH — not whether
> sleep is permitted.** Sleep always happened; the old claim in this block ("the
> tick thrashes against the sleep entry/exit overhead and starves the IDLE and main
> tasks") had the mechanism wrong, and a first correction then wrongly exonerated
> the tick altogether. All three versions are recorded at
> [Phase 10](#phase-10--light-sleep-and-the-lock-that-was-hiding-in-the-speaker)
> because each error is still available to make.
>
> **Always report entries AND mean sleep length.** A residency percentage alone
> hides the difference between 1.3 M sleeps of 330 µs and 395 k of 3.75 ms, and
> that difference is most of the win.
>
> ✅ **Both now measured, 2026-08-17: 11.1 ± 1.6 mA over 2.53 h — 33.1 → 11.1 mA,
> 12 h → 36 h of runtime — with `worst lateness 0 ms, 0 over 1 s`.** No evidence of
> task starvation at a 40 ms tick.
>
> **They remain opt-in for exactly one reason:** the lateness counter measures a
> fixed-period task, not `rr_sched`'s notification-with-timeout path, and a missed
> routine fire is the actual risk. One scheduler-integrity run (routine due ~20 min
> out, unplugged) is the last gate. A watch that saves power but misses a routine is
> a failure.
>
> ⚠️ **MEASURED 2026-08-14 — idle-asleep is 33.1 mA ±1 mA, i.e. ~12 h of
> runtime.** The first real capture on this board, and it is 3–5× worse than the
> 6–11 mA this document predicted. The cause is the one thing no lever table ever
> priced: with `RR_LIGHT_SLEEP` off the CPU never sleeps, and an always-on C6 with
> the BLE stack up accounts for essentially all of it. **Light sleep is therefore
> not one lever among six — it is the only one big enough to matter at this
> scale.** Every other estimate below is a rounding error against 33 mA; treat
> them as an ordering, never a magnitude. See
> [Measured results](#measured-results--idle-asleep-is-33-ma).
>
> That 33.1 mA was captured with `ls OFF`. **With light sleep on and a 40 ms tick
> it is 11.1 mA — a 3.0× reduction, measured over 2.53 h.** The two points together
> separate the CPU term (~23.8 mA fully awake) from a non-CPU floor of **~9.3 mA**,
> which is now the binding constraint: perfect sleep would buy only ~1.8 mA more.
> See [the non-CPU floor](#the-non-cpu-floor-is-93-ma-and-it-is-now-the-binding-constraint).
>
> ⚠️ **The harness could not return a result at all until 2026-08-14** — it was
> console-only on a device whose console is absent in the state being measured,
> the on-USB guard discarded the answer, and nothing ever called
> `rr_powerlog_mark()`. Fixed; see [The harness](#the-harness). Runs attempted
> before that date produced nothing.

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

**The baseline re-arms itself when you unplug.** `rr_powerlog_mark("label")`
still exists, but nothing calls it and on this board nothing can: there is no
console REPL, and BOOT is already wake (short press) and factory reset (10 s
hold). So the USB→battery transition is the signal that starts a measurement,
and unplugging is the only action required.

**Every whole-percent change is written to NVS** (namespace `rr_plog`), and the
result is printed back at the next `rr_powerlog_start()` as a
`POWERLOG ── PREVIOUS RUN ──` line. **Replugging finalises the window and prints
`POWERLOG ── FINAL ──` immediately**, before the on-USB guard runs.

> **⚠️ Correction (2026-08-14) — until this date the harness could not return a
> result at all, and the failure was total.** This document used to say "the
> `POWERLOG` lines are retained in the log; the last one before you plugged in is
> the answer." They were not retained anywhere. `rr_powerlog` only called
> `ESP_LOGI`, the console is USB-Serial-JTAG and nothing else
> (`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`, `CONFIG_ESP_CONSOLE_UART_NUM=-1`), so
> with the cable out every line went to a FIFO no host was draining and was
> discarded. There was no ring buffer, no NVS write and no littlefs write.
>
> Two further defects compounded it. The `vbus_present` guard ran *before* the
> rate computation and `continue`d, so the one recoverable path — the baseline
> survives in RAM, so the first sample after replugging could have computed the
> whole cumulative answer — printed `ON USB, NOT MEASURING` and threw the answer
> away. And `rr_powerlog_mark()` had no callers, so the baseline was always the
> one taken at boot, on the charger, at whatever SOC charging had reached.
>
> A two-hour run under the old procedure therefore produced a silent console, an
> unprinted answer and a baseline taken in the wrong state. All three are fixed;
> the point worth keeping is that **a harness whose only output path is the
> console cannot measure the one state that requires the console to be absent.**

**It refuses to measure on USB.** With VBUS present the PMIC is charging or
floating and the percentage will not fall, so it logs `ON USB, NOT MEASURING`
rather than printing a meaningless zero. If USB is seen at any point after the
baseline, later lines carry `⚠ USB SEEN SINCE BASELINE — SAMPLE INVALID`.

## Procedure: one before/after number for idle draw

The measurement that matters is **idle**, because that is where the day goes.

### Before

1. Charge to full, then flash with `-DRR_POWERLOG=1` and confirm the console
   shows `POWERLOG started`.
2. Put the watch in the state under test: paired, routines cached, **queue
   empty**, no phone in BLE range, **no routine due inside the window**, screen
   allowed to sleep (30 s of no interaction), sitting still and face-up on a desk.
3. **Unplug.** This is what arms the baseline — the next sample logs
   `idle-asleep (auto: unplugged)` and the window starts there.
4. Leave it **at least 2 hours**, ideally 3. Longer is strictly better: the
   quantisation error is one percent regardless of duration, so it shrinks as a
   fraction of the total. At an estimated ~8 mA a 2 h run drops ~4%, i.e. ±25%;
   3 h drops ~6%, i.e. ±17%.
5. **Replug.** Within one sample interval the console prints
   `POWERLOG ── FINAL ──` with the rate and the implied current. If the board
   resets on attach, the same numbers come back as
   `POWERLOG ── PREVIOUS RUN ──` from NVS instead.

Reading the answer needs USB and USB ends the measurement — which is fine,
because the window is *closed* by replugging rather than corrupted by it. The
rate is cumulative from the baseline, so the single final line is the whole
result. NVS is what makes it survive a reset on attach; do not rely on RAM,
because opening the port can assert reset through the USB-JTAG bridge's DTR/RTS
lines.

**Mid-run sanity check without ending the run:** press BOOT once to wake the
screen and read the battery percentage off the watch face. That costs one AMOLED
wake (~30 s at `AWAKE_MS`, order of 0.1% of the cell — well under the 1%
quantisation) and does not touch VBUS, so the window continues. It is the only
way to see progress while unplugged.

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
| Touch no longer polled while the panel is dark | `rr_idle.c` `go_to_sleep()` | ~0 (see correction below) | **was medium — now believed wrong** |
| Face gate cached (no littlefs read at 2 Hz) | `main.c` `watchface_allowed()` | small mA, large flash-idle win | high |
| Reset button interrupt-gated (was 10 Hz) | `rr_reset_button.c` | small | high |
| Heartbeat 5 s → 30 s | `main.c` | small | high |
| DFS enabled (`esp_pm_configure`, 40–160 MHz) | `main.c` | 1–3 mA | medium |

Rough expectation: **12–20 mA → 6–11 mA idle**, i.e. ~20–33 h → ~35–65 h. That
prediction is exactly what the harness is for.

> **⚠️ MEASURED 2026-08-14: 33.1 mA ±1 mA. The prediction above was wrong by
> 3–5×, and so was the "before" it was measured against.** The cause is not any
> single row of this table — it is that no row prices the baseline state, a CPU
> that never light-sleeps. See
> [MEASURED: idle-asleep is 33 mA](#measured-results--idle-asleep-is-33-ma).
> Treat every remaining estimate in this document as an ordering, never a
> magnitude.

> **⚠️ Correction (2026-08-13) — the touch row above was crediting a saving that
> was probably never available.** The row assumed LVGL polled the FT3168 over I2C
> at ~33 Hz for as long as the watch was dark, and that `set_touch_enabled(false)`
> stopped it. **The polling almost certainly was not happening in the first
> place.**
>
> `esp_lvgl_port`'s LVGL-9 touch backend registers the touch interrupt callback
> and sets `LV_INDEV_MODE_EVENT` whenever the handle carries an INT pin
> (`esp_lvgl_port_touch.c:59-73`) — and this BSP does set one
> (`BSP_LCD_TOUCH_INT = GPIO15`, passed as `.int_gpio_num` in `bsp_touch_new()`).
> So the indev has been **event-driven off TP_INT since the board was first
> brought up**: LVGL reads the controller when the interrupt fires and not
> otherwise. There was no 33 Hz I2C traffic to remove.
>
> What `set_touch_enabled(false)` actually does is call `lv_indev_enable(false)`,
> which is an LVGL-side switch. Its real effect is to stop a touch on a dark
> screen from being *delivered* — i.e. it is what removed touch-to-wake. Its
> power effect is somewhere between nil and negligible.
>
> **Unverified in both directions.** This correction is read from source
> (`esp_lvgl_port_touch.c`, the BSP touch config, `rr_idle.c`), not measured. What
> would settle it is an I2C-traffic or GPIO15-activity count over a dark hour, or
> simply the before/after idle capture with this one lever reverted. Until then,
> treat the estimate as **~0 mA, low confidence** — and note that the aggregate
> `12–20 → 6–11 mA` prediction above was summed *including* this row, so the
> optimistic end of it is correspondingly too optimistic.
>
> **The corollary matters more than the correction: nothing ever puts the FT3168
> to sleep.** `esp_lcd_touch_ft5x06` defines `FT5x06_ID_G_PMODE (0xA5)` and never
> writes it, and does not implement the `enter_sleep`/`exit_sleep` hooks that
> `esp_lcd_touch` exposes (`esp_lcd_touch.h:109,121`). The controller therefore
> scans continuously, all day and all night, and that current has been in every
> measurement ever taken on this board. See
> [the deferred touch-IC lever](#deferred--the-ft3168s-own-idle-current-and-tap-to-wake).

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

> ### 📌 THE TICK: THREE CLAIMS, TWO OF THEM WRONG — read all three
>
> This one question was answered wrongly twice before it was answered correctly,
> in both directions. The history is kept because each error had a distinct cause
> and each cause is still available to the next person.
>
> **Claim 1 (2026-08-14) — "the 5 ms tick keeps sleep windows under the 3 ms gate,
> so light sleep cannot engage." WRONG, twice over.** The arithmetic behind it
> (40% of windows pass, ~31% residency ceiling) assumed idle begins at a uniformly
> random phase within the tick period. In steady state the callback takes ~5 µs and
> idle then runs the remaining ~4.995 ms, which clears the gate comfortably. The
> supporting bench evidence was also void — see the USB warning below.
>
> **Claim 2 (2026-08-17, morning) — "the tick is exonerated; it was never the
> mechanism." ALSO WRONG, an over-correction from one data point.** The point was
> real: build A, stock 5 ms tick, 11.2 min unplugged, `entered 1328906 times,
> slept 436.2 s of 671.2 s = 65.0% residency`. Light sleep *does* engage with the
> tick untouched, which killed Claim 1 — but "it engages" was wrongly taken to mean
> "the tick doesn't matter". Two different questions.
>
> **Claim 3 (2026-08-17, A vs C) — the tick IS the mechanism, and it acts on sleep
> LENGTH, not on whether sleep is permitted.** This is the one that survived
> measurement:
>
> | Tick | Window | Entries | Wakes/s | Mean sleep | **Residency** |
> |---|---|---|---|---|---|
> | **5 ms** (stock) | 671 s | 1 328 906 | 1980 | 330 µs | **65.0%** |
> | **40 ms** | 1598 s | 395 284 | 247 | **3.75 ms** | **92.7%** |
>
> The gate was never the constraint — sleep always happened. The tick was capping
> how *long* each sleep could last, so the CPU paid entry/exit overhead ~1980 times
> a second to sleep 330 µs at a time. Lengthen the envelope and the same mechanism
> yields 3.75 ms sleeps and a 35% → 7.3% awake fraction.
>
> **The lesson that generalises:** "does it sleep at all" and "how long does each
> sleep last" are separate questions, and a residency percentage answers neither on
> its own. Always report entries AND mean sleep length; either alone is misleading.
>
> **What still stands** (measured by `rr_sleepdiag`, and independent of the wrong
> conclusion drawn from it):
> - The LVGL tick is the **only** periodic `esp_timer` while the screen is dark,
>   firing **200.0 Hz** (5 ms, `ESP_LVGL_PORT_INIT_CONFIG()`), with nothing on
>   screen. Its callback costs **0.1% CPU** — it was never a load problem.
> - The sleep gate is `sleep_time_us >= 3000` (`FREERTOS_HZ=1000` ×
>   `IDLE_TIME_BEFORE_SLEEP=3`), from `esp_pm/pm_impl.c:856`.
> - No lock blocks sleep while dark: every `NO_LIGHT_SLEEP`/`APB_FREQ_MAX` lock
>   reads `Active 0`, `i2s_driver` included, so the Phase 10 lock fix holds.
> - The 25 Hz step sampler (40 ms, 13× the gate) never blocked sleep either.
>
> ### ⚠️⚠️ NEVER JUDGE `ls`/`slp%` WITH USB ATTACHED. THIS COST TWO WRONG CONCLUSIONS.
>
> `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y` holds a `NO_LIGHT_SLEEP` lock for as
> long as a host is attached, so on the bench these counters read ~0 **no matter
> what the firmware does**. Three builds were compared plugged in and produced
> `ls 69`, `ls 0` and `ls 1`, all at `slp 0%` — numbers that look like a clean
> A/B/C result and mean nothing. The same trap in the other direction then made
> "light sleep is structurally overhead-dominated and cannot pay off" look like a
> finding. **It is not a finding. Unplug, or do not report a residency figure.**
>
> ### The real shape of the problem: 330 µs sleeps
>
> 1 328 906 entries across 671 s is **~1980 sleeps per second**, mean sleep
> **~330 µs** — an order of magnitude more frequent than the 200 Hz tick, so the
> tick is not even what sets the rate. Something wakes this CPU roughly every
> 500 µs.
>
> **65% residency is therefore worth much less than 65% sounds.** `slept_us` is
> measured as `esp_timer_get_time()` across `esp_light_sleep_start()`, so entry
> and exit overhead is counted *inside* the 436 s. At 330 µs per cycle on a board
> that cannot power down the main XTAL (no 32.768 kHz crystal; BLE on
> `MAIN_XTAL`), a large fraction of that is transition rather than sleep.
>
> **So the open question changed.** It is no longer "why won't it sleep" but
> **"what wakes it 1980 times a second, and can those wakes be batched into
> fewer, longer ones?"** Fewer/longer sleeps at the same residency is strictly
> more energy saved. Leading candidate: the 25 Hz IMU poll, replaced by the
> QMI8658's FIFO + watermark interrupt so 25 samples are collected in one wake
> per second instead of 25 — see the ranked list in
> [what to try next](#what-to-try-next-ranked).
>
> ### A negative result worth not rediscovering
>
> `lvgl_port_stop()` looks like the clean dark-only fix and is **strictly worse
> than doing nothing**. It is `esp_timer_stop(tick_timer)` *plus*
> `lv_timer_enable(false)`, and LVGL's `lv_timer.c:75` returns **1** when timers
> are disabled — not `LV_NO_TIMER_READY`. `esp_lvgl_port`'s task only substitutes
> its 500 ms max-sleep on `LV_NO_TIMER_READY`, so a return of 1 makes the
> event-group wait ONE TICK: it trades a 200 Hz timer for a **1 kHz task spin**.
> Kept behind `-DRR_LVGL_TICK_SLEEP=1`, off by default, as a documented dead end.
>
> `-DRR_LVGL_TICK_MS=<n>` (a globally slower tick, `rr_ui.c`) remains available
> and costs nothing visually — there are **no `lv_anim_*` calls anywhere in this
> firmware** and the routine countdown ring redraws on a 1 Hz `lv_timer`, so
> nothing animates faster than 1 Hz. Whether it lengthens the sleeps is an
> unplugged measurement, not a bench one.
>
> ### ✅ The 9.9 invariant, and the first milliamps
>
> Per-wake awake time: **177 µs** at a 5 ms tick, **294 µs** at 40 ms — so the
> awake fraction fell from **35% to 7.3%** even though each wake costs slightly
> more. Most of that awake time is light-sleep exit overhead, not work: the LVGL
> tick callback itself is ~5 µs.
>
> **The striking part is the ratio.** 1980 / 200 Hz = 9.9 wakes per tick period;
> 247 / 25 Hz = 9.9. **Identical.** Something subdivides every LVGL tick period
> into ~10 sleep/wake cycles, and whatever it is scales with the tick rather than
> setting its own. That is unexplained and is the most interesting open thread
> here — it predicts ~99 wakes/s and ~97% residency at a 100 ms tick, i.e. real
> but diminishing returns, and it means the ~10× subdivider is worth more than the
> tick period itself if it can be found.
>
> ### ✅✅ THE NUMBER: 11.1 mA, 36 h — measured over 2.53 h, 2026-08-17
>
> ```
> FINAL: state="idle-asleep (auto: unplugged)" 88%->81% over 2.53h
>        | 2.77 %/h ≈ 11.1 mA | full charge ≈ 36.2 h
> FINAL, LIGHT SLEEP: entered 2220167 times, slept 8411.1 s of 9111.7 s = 92.3%
> PREVIOUS RUN, SCHEDULING: 154 wake-ups, worst lateness 0 ms, 0 over 1 s
> ```
>
> Build C (`RR_LIGHT_SLEEP=1`, `RR_LVGL_TICK_MS=40`). A 7-point drop, so gauge
> quantisation is ±14%: **11.1 ± 1.6 mA.** Finalised cleanly — the board did not
> reset on attach, so this is the full window, not a truncation.
>
> | | `ls OFF` | light sleep + 40 ms tick |
> |---|---|---|
> | Idle draw | 33.1 mA | **11.1 mA** |
> | Runtime from full | 12.1 h | **36.2 h** |
>
> **Three quantities reproduced across a 5.7× longer window** than the 10-minute
> run: mean sleep 3.79 ms (was 3.75), 243.7 wakes/s (was 247), residency 92.3%
> (was 92.7%). The 9.9 invariant holds at **9.75** wakes per tick period.
>
> The earlier ≈9.0 mA guess was 20% low — the ±100% caveat on it was warranted.
>
> ### The non-CPU floor is 9.3 mA, and it is now the binding constraint
>
> Two points let the CPU term be separated from everything else. Assume draw is
> linear in the awake fraction:
>
> - 100% awake (`ls OFF`) → 33.1 mA
> - 7.7% awake (this run) → 11.1 mA
>
> Solving: **floor ≈ 9.3 mA, CPU ≈ 23.8 mA when fully awake.**
>
> **So perfect sleep — 0% awake — would give ~9.3 mA, i.e. 43 h.** The entire
> remaining CPU headroom is **~1.8 mA**, and everything else is in that 9.3 mA
> floor: the panel driver IC (est. 2–5 mA), the FT3168 (est. 1–2 mA), average BLE
> advertising (est. 1–3 mA), and the light-sleep floor itself with the main XTAL up.
>
> **This reorders the work.** Chasing the 9.75× subdivider and batching the IMU are
> now worth **at most ~1.8 mA between them**, where before this measurement they
> looked like the main event. The floor is where the next 3–8 mA lives.
>
> **A falsifiable prediction from the model, for whoever tests it:** build A (stock
> 5 ms tick, 35% awake) should measure **~17.6 mA** over 3 h. If it comes back far
> from that, the linearity assumption is wrong and this floor estimate goes with it.
>
> ### Task starvation: no evidence, and the honest limits of that
>
> `154 wake-ups, worst lateness 0 ms, 0 over 1 s` — a 60 s-period task at the main
> task's priority was never once even 1 ms late across 2.53 h of 92.3% residency.
> The starvation warning that has gated `RR_LIGHT_SLEEP` since Phase 10 has **no
> supporting evidence at a 40 ms tick.**
>
> **What this does not prove**, and neither should be glossed:
> - **`rr_sched`'s own path is not exercised by it.** The real risk is a missed
>   routine fire, and `rr_sched` waits on a task notification with a computed
>   timeout, not a fixed `vTaskDelay`. Only scheduling an actual routine inside an
>   unplugged window tests that.
> - **`task_wdt` output during the run is unobservable.** The console is dead while
>   USB is out, so a watchdog trigger would print to nobody. Absence of `task_wdt`
>   in the post-replug capture says nothing about the window. The lateness counter
>   is the only real evidence here.
>
> **The persistence work paid for itself on first use.** That record survived a
> reset on attach and came back via `PREVIOUS RUN`; RAM-only counters would have
> lost the whole run, which is exactly the failure this document already records
> twice.
>
> **Awake-side cost of a coarse tick is smaller than it looks**, and not just
> because nothing animates: touch does **not** wait for the tick. The FT3168
> interrupt calls `lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH, ...)`, which unblocks
> the port task's event group immediately and reads the indev at once
> (`esp_lvgl_port_touch.c:165`). Only `lv_timer` and animation resolution are
> quantised to the tick.

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

### Measured results — idle-asleep is 33 mA

**The first real capture on this board, and it invalidates the estimate method
rather than confirming it.**

```
POWERLOG ── FINAL ── state="idle-asleep (auto: unplugged)" 57%->25% over 3.87h
                     | 8.28 %/h ≈ 33.1 mA | full charge ≈ 12.1 h
```

| State | Predicted | **Measured** | Precision |
|---|---|---|---|
| idle-asleep (screen off, paired, queue empty, still, `ls OFF`) | 6–11 mA | **33.1 mA** | ±~1 mA |
| Implied runtime from full | 35–65 h | **~12 h** | |
| `ls` / `slp%` | n/a — light sleep off in this build | `ls OFF` confirmed in the heartbeat | |
| idle-awake (screen on) | _pending_ | _pending_ | |
| during a BLE sync | _pending_ | _pending_ | |

**This is a good measurement, and that is the uncomfortable part.** The 32-point
drop over 3.87 h means the gauge's ±1% quantisation is only ±3% of the reading —
so the honest error bar is about ±1 mA, not the ±20–30% this document warned to
expect. There is no room to hope the method was at fault. Conditions were clean:
`ble=advertising`, `routine=idle`, `screen=asleep`, `queued=0`, no phone in range,
no routine fired, and the only screen-on time was two brief BOOT presses worth
~0.1% of the cell between them.

**Where the estimate method went wrong.** Every row of the Phase 8 and Phase 10
lever tables prices a *change*. Nothing in either table prices the **baseline
state itself** — a CPU that never light-sleeps. `ls OFF` in the heartbeat is the
whole story: `RR_LIGHT_SLEEP` is off by default (the LVGL tick problem), so the
ESP32-C6 runs continuously with the BLE stack up, and an always-on C6 at
DFS 40–160 MHz accounts for essentially all 33 mA on its own. The tables treated
"light sleep off" as the status quo to measure changes against, so its cost was
never a line item and the sum of small savings was compared against a floor
nobody had measured.

**Consequences, in order of size:**

1. **Light sleep is not one lever among six — it is the only one that matters at
   this scale.** Everything else in both tables is a rounding error against
   33 mA. The LVGL-tick investigation
   ([Phase 10](#phase-10--light-sleep-and-the-lock-that-was-hiding-in-the-speaker))
   is therefore the highest-value open work on this firmware, and its hypothesis
   is still unconfirmed.
2. **The 2–5 mA panel-driver estimate and the 1–2 mA FT3168 estimate are 6–15%
   and 3–6% of idle respectively.** Both are worth having and neither changes the
   product. Do not spend a phase on either while the CPU is awake all day.
3. **Runtime is ~12 h, i.e. it does not survive a day.** That is a product
   problem, not a tuning problem.
4. **The future puck's "~2 weeks typical use"** (`routine-rush/docs/hardware-
   architecture.md`) is a *different* architecture — nRF5340 handling BLE with the
   C6 deep-asleep — so this figure does not disprove it. But it does mean the
   claim rests entirely on that offload working, and this measurement is the first
   evidence about the C6 half of it. Validate before it is repeated externally.

**What is still unmeasured:** idle-awake, during-a-BLE-sync, and — the one that
matters — idle-asleep with light sleep actually engaging. The first two are cheap
now that the harness works; the third needs the LVGL tick resolved first.

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

> ### ⚠️ CORRECTION 2026-08-17 — the panel handle IS reachable. No fork needed.
>
> The paragraph above says getting at the panel means "either reimplementing
> `bsp_display_start()` ... or patching a managed component". **That is wrong**, and
> it is wrong the same way the touch-handle claim was: the handle is recoverable
> from LVGL.
>
> `esp_lvgl_port` stores it in its per-display context and hands that context to
> LVGL as driver data (`esp_lvgl_port_disp.c:58-62,116`):
>
> ```c
> typedef struct {
>     lvgl_port_disp_type_t     disp_type;
>     esp_lcd_panel_io_handle_t io_handle;
>     esp_lcd_panel_handle_t    panel_handle;   /* <-- this */
>     ...
> } lvgl_port_display_ctx_t;
> ```
>
> So `lv_display_get_driver_data(lv_display_get_default())` yields the context, and
> `esp_lcd_panel_disp_on_off(ctx->panel_handle, false)` issues `DISPOFF` (0x28) —
> which `esp_lcd_sh8601.c:343` implements. Effort collapses from "re-do display
> bring-up including the `rounder_event_cb` x-alignment quirk" to a handle lookup
> plus one call each in `go_to_sleep()`/`wake_up()`.
>
> **Two real caveats, neither fatal:**
> - It reads a **private struct layout** from a managed component, so a component
>   update could reorder those fields silently. Same fragility already accepted for
>   the touch handle. Guard it: assert the handle is non-NULL and that `disp_type`
>   holds a sane value before trusting the pointer.
> - **`DISPON` must work on wake, and Phase 4b is the warning.** "Went unresponsive"
>   is what careless display surgery looks like here, so this needs its own visual
>   regression check — blank, wake, confirm pixels — not just a current reading.

### Floor follow-up, ranked (2026-08-17)

With the CPU term spent (~1.8 mA left) the remaining work is the **9.3 mA floor**.
Ranked by reward ÷ effort, after the handle-reachability correction above:

1. **`DISPOFF` — do this first.** Biggest single item (est. 2–5 mA, i.e. **22–54%
   of the whole floor**) and no longer the hardest, now that the panel handle turns
   out to be reachable. The AMOLED emission current already goes to ~zero at
   `brightness_set(0)`; this is the SH8601 driver IC and its boost converter, which
   keep running all night. Needs the visual regression check above.
2. **Idle advertising 852.5 ms → 2–3 s.** Est. 0.5–1.5 mA for a one-constant change
   in `rr_ble.c advertise()` — the cheapest thing on the list. The cost is phone
   discovery latency, and that is **already mitigated**: `queue_changed()` bumps
   advertising to a brisk window whenever the queue is non-empty, which is exactly
   when being found quickly matters.
3. **FT3168 hibernate.** Est. 1–2 mA, and last of the three despite being cheap to
   write (`PMODE`, register `0xA5`). Two unknowns keep it here: waking a hibernating
   FocalTech part typically needs a RST pulse, and `BSP_LCD_TOUCH_RST` is GPIO10
   which the BSP comments claim is shared with the LCD reset (the pin numbers differ
   — 10 vs 11 — so that comment is probably wrong, but "probably" is doing work in a
   sentence about resetting the display). It also forecloses tap-to-wake.

### Pending verification

- **The floor model's falsifiable check has NOT been run.** Build A (stock 5 ms
  tick, 35% awake) should measure **~17.6 mA** over 3 h if draw is linear in awake
  fraction. Deliberately deferred rather than forgotten: it validates the ~9.3 mA
  floor and therefore the ranking above, but it is not a prerequisite for shipping
  the 40 ms tick. If it comes back far from 17.6 mA, redo the ranking.

### What to try next, ranked

Ordered by (expected saving × confidence) ÷ effort, against the **measured**
33.1 mA `ls OFF` idle and the **measured** sleep shape (65% / 330 µs at a 5 ms
tick, 92.7% / 3.75 ms at 40 ms). The CPU term
is of order 20–28 mA of that 33 (estimate), so **items 2–6 together cannot
produce a multi-day watch — only sleeping the CPU properly can.**

**⚠️ REORDERED 2026-08-17 after the 11.1 mA measurement.** With the CPU term
separated out at ~23.8 mA and the non-CPU floor at ~9.3 mA, the remaining CPU
headroom is only **~1.8 mA**. The sleep-shape work that dominated this list is
now worth less than the floor items below it.

1. **Scheduler-integrity run — the last gate before shipping this.** Schedule a
   routine ~20 min out, unplug, confirm it fires with audio and screen. Cheap, and
   it is the only thing that tests `rr_sched`'s actual timing path; the lateness
   counter cannot. Keep it separate from a current measurement — the wake pollutes
   it.
2. **The 9.3 mA floor is where the next 3–8 mA lives.** Panel driver `DISPOFF`
   (est. 2–5 mA), FT3168 hibernate (est. 1–2 mA), idle advertising 852.5 ms → 2–3 s
   (est. 0.5–1.5 mA). Each is independently attributable and each now matters more
   than anything on the CPU side.
3. **Only ~1.8 mA left in sleep shape — spend accordingly.** The 9.75× subdivider
   (wakes/s is 9.75–9.9× the tick frequency across a 8× tick change; suspects are
   BLE controller wake timing, dynamically armed `nimble_timer` one-shots,
   `FREERTOS_HZ=1000`) and IMU FIFO batching are still real and still correct, but
   they share that 1.8 mA ceiling. Worth doing for a 43 h device, not worth a phase.
   `build-D` (`RR_LVGL_TICK_MS=100`) is built and tests the invariant in 10 min.
   Capping the CPU max frequency (e.g. 160 → 80 MHz) belongs in this bucket too —
   it only scales the awake term, which is now 7.7% of the time.
4. **Deep sleep — investigated and effectively blocked on this board.**
   `SOC_RTCIO_PIN_COUNT` is **8** on the ESP32-C6, so only GPIO0–7 can wake it,
   and here all eight are committed: QSPI display (0–5), audio amp enable (6),
   I2C SCL (7). BOOT is GPIO9, TP_INT GPIO15, IMU INT1/INT2 GPIO16/17 — all
   outside the range. **So deep sleep can only ever be woken by the LP timer: no
   button, no touch, no wrist.** Usable for a scheduled night mode, never for
   interactive idle.
5. **Product-level.** Nightly charging (12 h covers a day), or the puck
   architecture where the nRF5340 owns BLE and the C6 deep-sleeps. Note the
   puck's "~2 weeks" claim rests entirely on that offload.

### Deferred — the FT3168's own idle current, and tap-to-wake

These two are one decision, because **they are mutually exclusive** and the repo
currently has neither.

**The unclaimed saving.** The touch controller scans continuously (see the
correction above — `PMODE` is never written). Putting it into hibernate while the
panel is dark is the last untaken idle lever on this board. Order of magnitude
from FocalTech datasheets for the family: single-digit µA hibernating versus
~1–2 mA actively scanning. **Both figures are unverified here** — this board has
no battery-current ADC, so the only way to see it is the fuel-gauge rate-of-fall
procedure at the top of this document, with the lever toggled.

**Why it conflicts with tap-to-wake.** A hibernating FocalTech part cannot detect
a touch at all, and coming back out typically needs a RST pulse rather than an
I2C write — and `BSP_LCD_TOUCH_RST` is GPIO10, which the BSP comments claim is
shared with the LCD reset. Monitor mode (low-rate scan that still asserts INT)
would be the compromise that allows both, **but whether the FT3168 has one is a
datasheet question that has not been answered.**

**So: measure before choosing.** If the touch IC turns out to be ~1.5 mA of a
~8 mA idle, tap-to-wake costs ~20% of the battery story; if idle lands low, it
costs almost nothing. Nobody can tell which until the `idle-asleep` row in
[Measured results](#measured-results--idle-asleep-is-33-ma) is filled in. **That capture is the
prerequisite, and it is why tap-to-wake is deferred rather than rejected.**

> **✅ RESOLVED 2026-08-14 — the prerequisite is measured, and it unblocks
> tap-to-wake.** Idle-asleep is **33.1 mA**, so the FT3168's estimated 1–2 mA is
> **3–6% of idle**, not the ~20% that would have justified blocking a UX feature.
> Hibernating the touch controller is still worth doing eventually, but it is no
> longer a reason to withhold tap-to-wake, and neither is worth a phase while the
> CPU is awake all day.
>
> **The constraints below are unchanged, and constraint 2 matters MORE now, not
> less.** At a 33 mA floor a screen-on wake is a smaller *multiple* of idle than
> assumed — but it is also 12 h of runtime that cannot afford avoidable wakes.
> A tap-to-wake watch that lights the AMOLED for 30 s on every sleeve brush is
> still the most expensive mistake available here.

**What tap-to-wake would cost nothing.** Worth stating plainly, because it is the
part that is genuinely free: the hardware is present and connected (`TP_INT` →
GPIO15, confirmed at the MCU in the V1.0 net list — it is the contrast case
`rr_sched.h` uses to prove `RTC_INT` is *not* connected), the pin is claimed by
nothing else in this firmware, and `esp_lvgl_port` already drives the indev from
that interrupt. Against *today's* build it adds no touch-controller current,
because that current is already being paid.

**Constraints for whoever implements it** (product decision, 2026-08-13):

1. **Behind a build flag**, in the shape of `RR_LIGHT_SLEEP` — not on by default
   until it has been measured on a wrist.
2. **A shorter awake timeout for touch-originated wakes than for scheduler ones.**
   `AWAKE_MS` is 30 s because a *deliberate* button press means the child intends
   to read something; a tap that may have been a sleeve does not earn 30 s of
   AMOLED. 5–8 s — the original §9B.2 figure — is enough to read the time. This
   is the single most important line in this section: **the real cost of
   tap-to-wake is false wakes lighting the dominant load, not the controller's
   scan current.** It is the same economics that made raise-to-wake expensive.
3. Wake-source registration is only needed once light sleep actually ships. With
   `RR_LIGHT_SLEEP` off, a plain GPIO ISR runs and no registration is required.
   When it is needed: `gpio_sleep_sel_dis(GPIO15)` (because
   `CONFIG_PM_SLP_DISABLE_GPIO=y` powers the pad down otherwise), and beware that
   on the C6 the light-sleep wake **level** and the ordinary edge `INT_TYPE` are
   the same register field — the exact bug recorded in `rr_reset_button.c:213-228`.
   Light-sleep GPIO wake is level-triggered only, so the FT3168's INT must be
   *held* for the duration of the touch rather than pulsed; the driver never
   writes `G_MODE (0xA4)`, so the part's default INT mode decides this and it
   needs checking on the bench.
4. The touch handle is reachable — this is **not** the panel-handle trap. The BSP
   keeps `tp` in a file-static with no accessor, but
   `lv_indev_get_driver_data(bsp_display_get_input_dev())` returns the
   `lvgl_port_touch_ctx_t` whose first member is the handle, and `bsp_touch_new()`
   is public in `bsp/touch.h`. No BSP fork is needed for either lever.
