#pragma once
// rr_pm — CPU light sleep, and the locks that keep it from breaking things.
//
// ═════════════════════════════════════════════════════════════════════════════
// WHY THIS EXISTS, AND WHAT THE OLD "ls=0" ACTUALLY MEANT
//
// The heartbeat printed `40-160MHz ls=0` and that was read as "light sleep is
// not engaging". It was not evidence of anything: the line came from
// esp_pm_get_configuration(), which just echoes the config back, and main.c had
// passed light_sleep_enable=false. So ls=0 said "we never asked", not "we asked
// and it refused". That distinction cost real debugging time, which is why this
// module reports MEASURED sleep (rr_pm_stats) rather than configured intent.
//
// Two separate things had to be true before light sleep could be switched on,
// and only one of them was ever about the display:
//
//  1. THE BLE CONTROLLER. Without CONFIG_BT_LE_SLEEP_ENABLE, esp32c6/bt.c never
//     registers controller_sleep_cb/controller_wakeup_cb and never calls
//     esp_sleep_enable_bt_wakeup(). Light sleep would then stop the controller's
//     clock with nothing arranged to wake it for its next advertising event —
//     BLE would simply rot. docs/POWER.md concluded this was unfixable on this
//     board because BLE sleep "wants a 32.768 kHz low-power clock" that GPIO0/1
//     cannot provide (they are QSPI pins). That conclusion was too pessimistic:
//     CONFIG_BT_LE_LP_CLK_SRC_MAIN_XTAL exists precisely for boards without a
//     32.768 kHz crystal — IDF's own help text says "recommended if external
//     32.768k XTAL is not available". Its cost is that light sleep cannot power
//     down the main XTAL, so the floor is higher than it would be with a
//     crystal; its benefit is that light sleep works at all. See
//     sdkconfig.defaults.
//
//  2. THE DISPLAY AND TOUCH. This is the part Phase 6 got right and Phase 4b was
//     bitten by: QSPI to the SH8601 and I2C to the FT3168 cannot survive the
//     clock stopping underneath an in-flight transfer, and the panel comes back
//     frozen. The fix is NOT to disable power management globally — it is to
//     hold a lock for exactly as long as the panel is lit, which is a small
//     fraction of the day.
//
// ⚠️ ON THIS CHIP, ESP_PM_APB_FREQ_MAX AND ESP_PM_NO_LIGHT_SLEEP ARE THE SAME
// LOCK IN PRACTICE. The ESP32-C6's APB clock is fixed at 40 MHz and does not
// follow the CPU frequency, so "hold APB at max" cannot mean "raise APB" — all
// it can do is forbid the sleep that would stop it. This module asks for
// NO_LIGHT_SLEEP because that is the thing actually being requested, and naming
// it honestly is worth more than matching the phrasing in the brief.
// ═════════════════════════════════════════════════════════════════════════════

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * Configure DFS and (optionally) automatic light sleep, and create the locks.
 *
 * Call EARLY — before the display, BLE or any driver that might take a lock of
 * its own. Safe to call when light sleep is off; the locks still work and
 * simply have nothing to prevent.
 */
esp_err_t rr_pm_init(bool enable_light_sleep);

/**
 * Hold (or release) the no-light-sleep lock for the display + touch.
 *
 * TRUE while the panel is lit, FALSE the moment it goes dark. Idempotent and
 * reference-free on purpose: there is exactly one display and exactly one
 * caller (rr_idle), and a counted lock would only make a missed release harder
 * to spot. A missed release costs the entire battery win, so it is logged.
 */
void rr_pm_display_hold(bool hold);

/** True while the display lock is held. */
bool rr_pm_display_is_held(void);

/**
 * MEASURED light-sleep activity since boot — the thing `ls=` should have been
 * reporting all along.
 *
 * `entries` counts actual light sleeps; `slept_us` totals the time spent in
 * them. Both stay zero if light sleep is configured but something is silently
 * holding a lock, which is exactly the failure the old heartbeat could not see.
 *
 * Requires CONFIG_PM_LIGHT_SLEEP_CALLBACKS; without it both read zero and
 * rr_pm_stats_available() is false, so the heartbeat can say "unknown" rather
 * than print a confident zero.
 */
void rr_pm_stats(uint32_t *entries, uint64_t *slept_us);

bool rr_pm_stats_available(void);

/**
 * One line for the heartbeat, e.g. "ls 412 slp 71% disp-lock".
 * Never NULL; writes into the caller's buffer.
 */
void rr_pm_describe(char *buf, size_t len);

/**
 * List every power-management lock and who is holding it, to the console.
 *
 * ⚠️ THE ONLY HONEST WAY TO ANSWER "why is it not sleeping?". A held lock is
 * invisible from the outside — the system looks configured correctly and simply
 * never sleeps — and there are more holders than anyone expects: IDF's own SPI,
 * I2C, I2S and USB-Serial-JTAG drivers all take one, several of them for as
 * long as a bus is merely INITIALISED rather than in use. Guessing which is a
 * waste of an afternoon; this prints the answer.
 */
void rr_pm_dump_locks(void);
