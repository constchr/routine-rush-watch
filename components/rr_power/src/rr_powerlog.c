// rr_powerlog — SOC-drop power measurement. See rr_powerlog.h for why this is
// the only measurement available on this board.

#include "rr_powerlog.h"

#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rr_battery.h"

static const char *TAG = "rr_powerlog";

#define TASK_STACK 3072
#define TASK_PRIO  1          /* lowest: measuring must not perturb the thing measured */

static volatile bool s_running;
static int  s_interval_s = 60;

// Baseline for the rate calculation.
static int64_t  s_base_us;
static uint8_t  s_base_pct;
static bool     s_base_valid;
static char     s_label[32] = "unlabelled";

// Set when USB has been seen at any point since the baseline: a charge event
// invalidates the whole sample, and silently reporting the surviving numbers
// would be worse than saying so.
static bool s_charged_since_mark;

static void mark_now(const rr_battery_t *b, const char *label)
{
    s_base_us = esp_timer_get_time();
    s_base_pct = b->percent;
    s_base_valid = true;
    s_charged_since_mark = false;
    if (label != NULL) {
        // strlcpy would need string.h for one call; this is bounded and explicit.
        int i = 0;
        for (; label[i] != '\0' && i < (int) sizeof(s_label) - 1; i++) s_label[i] = label[i];
        s_label[i] = '\0';
    }
    ESP_LOGI(TAG, "POWERLOG baseline: state=\"%s\" soc=%u%% mv=%u",
             s_label, b->percent, b->millivolts);
}

static void powerlog_task(void *arg)
{
    (void) arg;

    rr_battery_t b;
    if (rr_battery_read(&b) == ESP_OK) mark_now(&b, NULL);

    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t) s_interval_s * 1000));
        if (!s_running) break;

        if (rr_battery_read(&b) != ESP_OK) {
            ESP_LOGW(TAG, "POWERLOG battery unreadable");
            continue;
        }

        const int64_t now_us = esp_timer_get_time();
        const double elapsed_h = (double) (now_us - s_base_us) / 3600e6;

        if (b.vbus_present) {
            // Charging or floating on USB: the gauge will not fall, so any rate
            // computed from here is meaningless. Say it every sample — a run
            // accidentally left plugged in is the likeliest way to waste an hour.
            s_charged_since_mark = true;
            ESP_LOGW(TAG, "POWERLOG t=%.2fh soc=%u%% mv=%u — ON USB, NOT MEASURING "
                          "(unplug and call rr_powerlog_mark)",
                     elapsed_h, b.percent, b.millivolts);
            continue;
        }

        if (!s_base_valid) { mark_now(&b, NULL); continue; }

        const int dropped = (int) s_base_pct - (int) b.percent;

        if (dropped <= 0 || elapsed_h <= 0.0) {
            // Normal early on: a 10 mA state takes ~24 min to move one percent.
            ESP_LOGI(TAG, "POWERLOG t=%.2fh soc=%u%% mv=%u state=\"%s\" — "
                          "no whole percent lost yet",
                     elapsed_h, b.percent, b.millivolts, s_label);
            continue;
        }

        // %/h -> mA. One percent of a 400 mAh cell is 4 mAh, so a drop of D
        // percent over H hours is (D * capacity/100) / H milliamps.
        const double pct_per_h = (double) dropped / elapsed_h;
        const double ma = pct_per_h * RR_POWERLOG_BATTERY_MAH / 100.0;
        const double hours_to_empty = b.percent > 0 ? (double) b.percent / pct_per_h : 0.0;

        ESP_LOGI(TAG, "POWERLOG t=%.2fh soc=%u%% mv=%u state=\"%s\" | "
                      "dropped %d%% -> %.2f %%/h ≈ %.1f mA | full charge ≈ %.1f h%s",
                 elapsed_h, b.percent, b.millivolts, s_label,
                 dropped, pct_per_h, ma, 100.0 / pct_per_h,
                 s_charged_since_mark ? "  ⚠ USB SEEN SINCE BASELINE — SAMPLE INVALID" : "");
        (void) hours_to_empty;
    }

    ESP_LOGI(TAG, "POWERLOG stopped");
    vTaskDelete(NULL);
}

esp_err_t rr_powerlog_start(int interval_s)
{
    if (s_running) return ESP_OK;
    s_interval_s = interval_s > 0 ? interval_s : 60;
    s_running = true;

    if (xTaskCreate(powerlog_task, "rr_powerlog", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "could not start the power-log task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "POWERLOG started — sampling every %d s (%d mAh cell, 1%% = %.1f mAh)",
             s_interval_s, RR_POWERLOG_BATTERY_MAH, RR_POWERLOG_BATTERY_MAH / 100.0);
    return ESP_OK;
}

void rr_powerlog_stop(void) { s_running = false; }

void rr_powerlog_mark(const char *state_label)
{
    rr_battery_t b;
    if (rr_battery_read(&b) == ESP_OK) mark_now(&b, state_label);
}

bool rr_powerlog_is_running(void) { return s_running; }
