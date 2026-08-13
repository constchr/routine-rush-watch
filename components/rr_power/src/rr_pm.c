// rr_pm — light sleep + the display lock. See rr_pm.h for the reasoning.

#include "rr_pm.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "rr_pm";

static esp_pm_lock_handle_t s_display_lock;
static bool s_display_held;
static bool s_light_sleep_on;

// Written from the light-sleep callbacks, which run in the IDLE task context.
// Both are read from the heartbeat task, so they are volatile and the 64-bit
// one is only ever accumulated by a single writer — a torn read of a
// diagnostic counter is not worth a critical section on the sleep path.
static volatile uint32_t s_entries;
static volatile uint64_t s_slept_us;

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
// ⚠️ These run from the IDLE task with the scheduler about to stop. Nothing
// blocking, nothing that logs, nothing that takes a lock. Two integer updates.
static esp_err_t IRAM_ATTR on_sleep_enter(int64_t sleep_time_us, void *arg)
{
    (void) sleep_time_us; (void) arg;
    return ESP_OK;
}

static esp_err_t IRAM_ATTR on_sleep_exit(int64_t sleep_time_us, void *arg)
{
    (void) arg;
    s_entries++;
    if (sleep_time_us > 0) s_slept_us += (uint64_t) sleep_time_us;
    return ESP_OK;
}
#endif

esp_err_t rr_pm_init(bool enable_light_sleep)
{
    // The lock is created BEFORE light sleep is enabled, so there is never a
    // window where the panel could be lit with no way to protect it.
    esp_err_t err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "rr_display",
                                       &s_display_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not create the display PM lock: %s — refusing to "
                      "enable light sleep, because the panel would freeze",
                 esp_err_to_name(err));
        enable_light_sleep = false;
    }

    // The watch boots with the screen ON (the face, or the pairing QR), so the
    // lock is taken here rather than waiting for rr_idle's first wake. Getting
    // this backwards would light the panel with sleep permitted, which is the
    // Phase 4b "went unresponsive" failure exactly.
    if (s_display_lock != NULL) {
        esp_pm_lock_acquire(s_display_lock);
        s_display_held = true;
    }

    const esp_pm_config_t pm = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,          // the XTAL rate on this part
        .light_sleep_enable = enable_light_sleep,
    };
    err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_pm_configure failed: %s", esp_err_to_name(err));
        return err;
    }
    s_light_sleep_on = enable_light_sleep;

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    esp_pm_sleep_cbs_register_config_t cbs = {
        .enter_cb = on_sleep_enter,
        .exit_cb = on_sleep_exit,
        .enter_cb_prior = 0,
        .exit_cb_prior = 0,
    };
    esp_err_t cb_err = esp_pm_light_sleep_register_cbs(&cbs);
    if (cb_err != ESP_OK) {
        ESP_LOGW(TAG, "light-sleep callbacks not registered (%s) — the heartbeat "
                      "will not be able to prove sleep is happening",
                 esp_err_to_name(cb_err));
    }
#else
    ESP_LOGW(TAG, "CONFIG_PM_LIGHT_SLEEP_CALLBACKS is off — sleep time cannot be "
                  "measured, only configured. Turn it on before trusting any "
                  "before/after power number.");
#endif

    ESP_LOGI(TAG, "power management: DFS %d-%d MHz, light sleep %s "
                  "(display lock HELD — screen is on at boot)",
             pm.min_freq_mhz, pm.max_freq_mhz,
             enable_light_sleep ? "ENABLED" : "off");
    return ESP_OK;
}

void rr_pm_display_hold(bool hold)
{
    if (s_display_lock == NULL) return;
    if (hold == s_display_held) return;

    if (hold) {
        esp_pm_lock_acquire(s_display_lock);
        s_display_held = true;
        ESP_LOGD(TAG, "display lock acquired — light sleep inhibited");
    } else {
        esp_pm_lock_release(s_display_lock);
        s_display_held = false;
        ESP_LOGD(TAG, "display lock released — light sleep permitted");
    }
}

bool rr_pm_display_is_held(void) { return s_display_held; }

void rr_pm_dump_locks(void)
{
    ESP_LOGI(TAG, "──── power management locks ────");
    esp_pm_dump_locks(stdout);
    ESP_LOGI(TAG, "──── (a non-zero count on any NO_LIGHT_SLEEP / APB_FREQ_MAX "
                  "lock is why the chip is awake) ────");
}

void rr_pm_stats(uint32_t *entries, uint64_t *slept_us)
{
    if (entries != NULL) *entries = s_entries;
    if (slept_us != NULL) *slept_us = s_slept_us;
}

bool rr_pm_stats_available(void)
{
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    return true;
#else
    return false;
#endif
}

void rr_pm_describe(char *buf, size_t len)
{
    if (buf == NULL || len == 0) return;

    if (!s_light_sleep_on) {
        snprintf(buf, len, "ls OFF");
        return;
    }
    if (!rr_pm_stats_available()) {
        snprintf(buf, len, "ls on (unmeasured)%s", s_display_held ? " disp-lock" : "");
        return;
    }

    // Percentage of WALL CLOCK spent asleep, not of idle time — that is the
    // number that maps to battery life, and it is the one that makes a
    // regression obvious (a stuck lock takes it to 0, not to "slightly less").
    const uint64_t up_us = (uint64_t) esp_timer_get_time();
    const unsigned pct = up_us > 0 ? (unsigned) ((s_slept_us * 100) / up_us) : 0;
    snprintf(buf, len, "ls %" PRIu32 " slp %u%%%s",
             s_entries, pct, s_display_held ? " disp-lock" : "");
}
