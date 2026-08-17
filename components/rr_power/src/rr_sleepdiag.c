// rr_sleepdiag — see rr_sleepdiag.h for why this is a schedule question and not
// a power question.

#include "rr_sleepdiag.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "rr_idle.h"
#include "rr_pm.h"
#include "rr_steps.h"

static const char *TAG = "rr_sleepdiag";

#define TASK_STACK 4096
#define TASK_PRIO  1

static int s_first_delay_s = 20;
static int s_interval_s = 30;

// The gate from vApplicationSleep(), computed rather than quoted so it cannot
// drift away from the build it is describing.
#define SLEEP_GATE_US (CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP * portTICK_PERIOD_MS * 1000)

static void banner(const char *what)
{
    ESP_LOGW(TAG, "╔═══════════════════════════════════════════════════════════");
    ESP_LOGW(TAG, "║ %s", what);
    ESP_LOGW(TAG, "╚═══════════════════════════════════════════════════════════");
}

static void diag_task(void *arg)
{
    (void) arg;

    vTaskDelay(pdMS_TO_TICKS((uint32_t) s_first_delay_s * 1000));

    banner("SLEEP DIAGNOSTIC — the gate, and who fails it");

    ESP_LOGW(TAG, "The gate in IDF's vApplicationSleep():");
    ESP_LOGW(TAG, "  sleep_time_us = MIN(idle_ticks_us, next_esp_timer_alarm - now)");
    ESP_LOGW(TAG, "  sleep happens only if sleep_time_us >= %d us", SLEEP_GATE_US);
    ESP_LOGW(TAG, "  (FREERTOS_HZ=%d, tick=%d ms, IDLE_TIME_BEFORE_SLEEP=%d)",
             CONFIG_FREERTOS_HZ, (int) portTICK_PERIOD_MS,
             CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP);
    ESP_LOGW(TAG, "So ANY periodic wake faster than %d us caps every sleep window "
                  "below the gate and light sleep can never engage.", SLEEP_GATE_US);

    ESP_LOGW(TAG, "screen=%s (the census is only meaningful while DARK — a lit "
                  "panel legitimately holds rr_display)",
             rr_idle_is_awake() ? "AWAKE ⚠" : "asleep");

    // ── Census pass 1 ───────────────────────────────────────────────────────
    banner("esp_timer census — PASS 1");
    const int64_t t1 = esp_timer_get_time();
#if CONFIG_ESP_TIMER_PROFILING
    esp_timer_dump(stdout);
#else
    ESP_LOGE(TAG, "CONFIG_ESP_TIMER_PROFILING is OFF — periods and counts are not "
                  "available. Build with sdkconfig.diag.");
    esp_timer_dump(stdout);
#endif
    fflush(stdout);

    ESP_LOGW(TAG, "PM locks at pass 1:");
    rr_pm_dump_locks();

    // ── Wait, then census pass 2 ────────────────────────────────────────────
    // Differencing the trigger counts across a known interval gives each timer's
    // REAL rate. A configured period that the code does not actually honour
    // shows up here as a rate that disagrees with it.
    vTaskDelay(pdMS_TO_TICKS((uint32_t) s_interval_s * 1000));

    const int64_t t2 = esp_timer_get_time();
    banner("esp_timer census — PASS 2");
    ESP_LOGW(TAG, "interval between passes: %.3f s — subtract PASS 1 counts from "
                  "PASS 2 counts and divide by this to get each timer's Hz",
             (double) (t2 - t1) / 1e6);
#if CONFIG_ESP_TIMER_PROFILING
    esp_timer_dump(stdout);
#endif
    fflush(stdout);

    ESP_LOGW(TAG, "PM locks at pass 2:");
    rr_pm_dump_locks();

    // ── The task-delay side, which esp_timer_dump cannot see ────────────────
    // rr_steps sleeps on vTaskDelayUntil, not an esp_timer, so it bounds
    // xExpectedIdleTime rather than next_esp_timer_alarm. Both feed the same MIN.
    banner("the task-delay side — rr_steps");
    char sbuf[160];
    rr_steps_describe_input(sbuf, sizeof(sbuf));
    ESP_LOGW(TAG, "%s", sbuf);
    ESP_LOGW(TAG, "rr_steps samples at %d Hz => a %d us period, which is %s the "
                  "%d us gate.",
             RR_STEPS_SAMPLE_HZ, 1000000 / RR_STEPS_SAMPLE_HZ,
             (1000000 / RR_STEPS_SAMPLE_HZ) >= SLEEP_GATE_US ? "COMFORTABLY ABOVE"
                                                             : "BELOW",
             SLEEP_GATE_US);

    banner("HOW TO READ THIS");
    ESP_LOGW(TAG, "1. Find the timer with the HIGHEST measured rate.");
    ESP_LOGW(TAG, "2. Its period is the ceiling on every sleep window.");
    ESP_LOGW(TAG, "3. If that period < %d us, light sleep CANNOT engage and no "
                  "amount of lock-fixing will change it.", SLEEP_GATE_US);
    ESP_LOGW(TAG, "4. If that period > %d us, the tick hypothesis is WRONG and "
                  "the blocker is a lock or a task, not a timer.", SLEEP_GATE_US);

    ESP_LOGW(TAG, "diagnostic complete — this task now exits");
    vTaskDelete(NULL);
}

esp_err_t rr_sleepdiag_start(int first_delay_s, int interval_s)
{
    s_first_delay_s = first_delay_s > 0 ? first_delay_s : 20;
    s_interval_s = interval_s > 0 ? interval_s : 30;

    if (xTaskCreate(diag_task, "rr_sleepdiag", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the diagnostic task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
