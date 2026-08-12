// rr_rtc — PCF85063 real-time clock driver.
//
// Register map (PCF85063A datasheet §8.3):
//   0x00 Control_1   bit5 STOP
//   0x04 Seconds     bit7 OS (oscillator stop flag), bits6..0 BCD seconds
//   0x05 Minutes     BCD
//   0x06 Hours       BCD (24h mode — Control_1 bit1 = 0)
//   0x07 Days        BCD
//   0x08 Weekdays    0..6
//   0x09 Months      BCD
//   0x0A Years       BCD, offset from 2000

#include "rr_rtc.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rr_rtc";

#define PCF85063_ADDR       0x51
#define REG_CONTROL_1       0x00
#define REG_CONTROL_2       0x01
#define REG_SECONDS         0x04
#define REG_ALARM_SECOND    0x0B   /* then minute, hour, day, weekday */
#define CTRL1_STOP_BIT      0x20
#define SECONDS_OS_BIT      0x80

// Control_2 (datasheet §8.3.2): bit7 AIE, bit6 AF, bit5 MI, bit4 HMI,
// bit3 TF, bits2..0 COF (clock-output frequency).
#define CTRL2_AIE_BIT       0x80
#define CTRL2_AF_BIT        0x40
#define CTRL2_TF_BIT        0x08

// Each alarm register's bit 7 is AEN_x, and its polarity is the trap: 1
// DISABLES that field from the comparison, 0 enables it. An alarm register
// block left at its power-on 0x80,0x80,... is therefore "match nothing".
#define ALARM_DISABLE_BIT   0x80

static i2c_master_dev_handle_t s_dev;

static void tz_load_from_nvs(void);   // defined with the UTC-offset section below

static uint8_t to_bcd(uint8_t v) { return (uint8_t) (((v / 10) << 4) | (v % 10)); }
static uint8_t from_bcd(uint8_t v) { return (uint8_t) (((v >> 4) * 10) + (v & 0x0F)); }

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 1000);
}

static esp_err_t reg_write(uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t tmp[8];
    if (n + 1 > sizeof(tmp)) return ESP_ERR_INVALID_SIZE;
    tmp[0] = reg;
    memcpy(&tmp[1], buf, n);
    return i2c_master_transmit(s_dev, tmp, n + 1, 1000);
}

esp_err_t rr_rtc_init(i2c_master_bus_handle_t bus)
{
    if (bus == NULL) return ESP_ERR_INVALID_ARG;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t ctrl = 0;
    err = reg_read(REG_CONTROL_1, &ctrl, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCF85063 not responding at 0x%02X: %s", PCF85063_ADDR, esp_err_to_name(err));
        return err;
    }

    // Clear STOP so the counter runs. A factory board arrives with the clock
    // running but never set, so this is usually already 0.
    if (ctrl & CTRL1_STOP_BIT) {
        ctrl &= (uint8_t) ~CTRL1_STOP_BIT;
        ESP_ERROR_CHECK_WITHOUT_ABORT(reg_write(REG_CONTROL_1, &ctrl, 1));
        ESP_LOGW(TAG, "clock was stopped — started it");
    }

    ESP_LOGI(TAG, "PCF85063 ready (Control_1=0x%02X)", ctrl);

    // Restore the UTC offset before anything renders. app_main calls this after
    // nvs_flash_init(), so NVS is up. Without it the first face drawn on a
    // no-phone boot would show UTC for as long as it took a phone to appear.
    tz_load_from_nvs();
    return ESP_OK;
}

esp_err_t rr_rtc_get(rr_rtc_time_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t r[7];
    esp_err_t err = reg_read(REG_SECONDS, r, sizeof(r));
    if (err != ESP_OK) return err;

    out->osc_ok = (r[0] & SECONDS_OS_BIT) == 0;
    out->second = from_bcd(r[0] & 0x7F);
    out->minute = from_bcd(r[1] & 0x7F);
    out->hour   = from_bcd(r[2] & 0x3F);
    out->day    = from_bcd(r[3] & 0x3F);
    out->month  = from_bcd(r[5] & 0x1F);
    out->year   = (uint16_t) (2000 + from_bcd(r[6]));
    return ESP_OK;
}

esp_err_t rr_rtc_set_epoch(uint32_t epoch_utc)
{
    time_t t = (time_t) epoch_utc;
    struct tm tm_utc;
    if (gmtime_r(&t, &tm_utc) == NULL) return ESP_ERR_INVALID_ARG;

    // The PCF85063 year register is an offset from 2000 and cannot represent
    // anything outside 2000..2099. Reject rather than silently wrapping — a
    // bad epoch would otherwise land as a plausible-looking wrong date.
    int year = tm_utc.tm_year + 1900;
    if (year < 2000 || year > 2099) {
        ESP_LOGE(TAG, "epoch %" PRIu32 " → year %d, outside the RTC's 2000..2099 range", epoch_utc, year);
        return ESP_ERR_INVALID_ARG;
    }

    // Writing seconds also clears the OS flag (bit 7 = 0), which is what marks
    // the time as trustworthy from here on.
    uint8_t r[7] = {
        to_bcd((uint8_t) tm_utc.tm_sec),
        to_bcd((uint8_t) tm_utc.tm_min),
        to_bcd((uint8_t) tm_utc.tm_hour),
        to_bcd((uint8_t) tm_utc.tm_mday),
        (uint8_t) tm_utc.tm_wday,
        to_bcd((uint8_t) (tm_utc.tm_mon + 1)),
        to_bcd((uint8_t) (year - 2000)),
    };

    esp_err_t err = reg_write(REG_SECONDS, r, sizeof(r));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC write failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

void rr_rtc_format(const rr_rtc_time_t *t, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "%04u-%02u-%02u %02u:%02u:%02u",
             t->year, t->month, t->day, t->hour, t->minute, t->second);
}

uint32_t rr_rtc_get_epoch(void)
{
    rr_rtc_time_t t;
    if (rr_rtc_get(&t) != ESP_OK) return 0;

    struct tm tm_utc = {
        .tm_year = t.year - 1900,
        .tm_mon  = t.month - 1,
        .tm_mday = t.day,
        .tm_hour = t.hour,
        .tm_min  = t.minute,
        .tm_sec  = t.second,
    };
    // Computed directly rather than via mktime(): mktime applies the local
    // timezone, which would shift every recorded run. timegm() is not in
    // newlib, so this is the days-from-civil algorithm (Howard Hinnant's),
    // valid for the RTC's whole 2000..2099 range.
    int y = tm_utc.tm_year + 1900;
    unsigned m = (unsigned) tm_utc.tm_mon + 1;
    unsigned d = (unsigned) tm_utc.tm_mday;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned) (y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = (long) era * 146097 + (long) doe - 719468;

    long secs = days * 86400L + tm_utc.tm_hour * 3600L + tm_utc.tm_min * 60L + tm_utc.tm_sec;
    return secs < 0 ? 0 : (uint32_t) secs;
}

void rr_rtc_format_iso(const rr_rtc_time_t *t, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "%04u-%02u-%02uT%02u:%02u:%02uZ",
             t->year, t->month, t->day, t->hour, t->minute, t->second);
}

void rr_rtc_epoch_to_iso(uint32_t epoch, char *buf, size_t buflen)
{
    time_t e = (time_t) epoch;
    struct tm tm_utc;
    if (gmtime_r(&e, &tm_utc) == NULL) { snprintf(buf, buflen, "1970-01-01T00:00:00Z"); return; }
    snprintf(buf, buflen, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
             tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
}

// ═════════════════════════════════════════════════════════════════════════════
// UTC offset — local wall-clock for the SCREEN ONLY (see rr_rtc.h)
//
// Its own NVS namespace rather than rr_identity's: the offset is a property of
// WHERE THE WATCH IS, not of whose watch it is, so it deliberately survives a
// factory reset — a re-paired watch in the same house is in the same timezone,
// and the next connect overwrites it anyway.
// ═════════════════════════════════════════════════════════════════════════════

#define TZ_NVS_NAMESPACE "rr_tz"
#define TZ_NVS_KEY       "offset_s"

// Same bounds the contract states (TZ_OFFSET_MIN_S / TZ_OFFSET_MAX_S): real
// offsets run UTC-12:00 to UTC+14:00. Anything outside is a bug on the wire,
// and applying it would put the face hours or days out.
#define TZ_OFFSET_MIN_S (-12 * 3600)
#define TZ_OFFSET_MAX_S (14 * 3600)

static int32_t s_utc_offset_s;
static bool    s_utc_offset_known;

static void tz_load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(TZ_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no stored UTC offset — the face will show UTC until a phone sends set_tz");
        return;
    }
    int32_t v = 0;
    if (nvs_get_i32(h, TZ_NVS_KEY, &v) == ESP_OK &&
        v >= TZ_OFFSET_MIN_S && v <= TZ_OFFSET_MAX_S) {
        s_utc_offset_s = v;
        s_utc_offset_known = true;
        ESP_LOGI(TAG, "UTC offset restored from NVS: %+" PRId32 " s (%+d:%02d) — "
                      "local time is correct with no phone present",
                 v, (int) (v / 3600), (int) ((v < 0 ? -v : v) % 3600) / 60);
    } else {
        ESP_LOGW(TAG, "no stored UTC offset — the face will show UTC until a phone sends set_tz");
    }
    nvs_close(h);
}

esp_err_t rr_rtc_set_utc_offset(int32_t offset_s)
{
    if (offset_s < TZ_OFFSET_MIN_S || offset_s > TZ_OFFSET_MAX_S) {
        ESP_LOGE(TAG, "set_tz REJECTED: offset_s=%" PRId32 " is not a real UTC offset", offset_s);
        return ESP_ERR_INVALID_ARG;
    }

    const int32_t previous = s_utc_offset_s;
    s_utc_offset_s = offset_s;
    s_utc_offset_known = true;

    nvs_handle_t h;
    esp_err_t err = nvs_open(TZ_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        // In force for this boot, but gone after a reboot. Worth a loud line:
        // "correct until you power-cycle" is a confusing symptom to chase.
        ESP_LOGE(TAG, "offset applied but NOT PERSISTED (nvs_open: %s) — it will "
                      "be lost on reboot", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_i32(h, TZ_NVS_KEY, offset_s);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "offset applied but NOT PERSISTED (%s)", esp_err_to_name(err));
        return err;
    }

    if (previous != offset_s) {
        ESP_LOGI(TAG, "UTC offset CHANGED %+" PRId32 " → %+" PRId32 " s (DST boundary, or a "
                      "watch that has travelled)", previous, offset_s);
    }
    return ESP_OK;
}

int32_t rr_rtc_get_utc_offset(void) { return s_utc_offset_s; }

bool rr_rtc_has_utc_offset(void) { return s_utc_offset_known; }

esp_err_t rr_rtc_get_local(rr_rtc_time_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    rr_rtc_time_t utc;
    esp_err_t err = rr_rtc_get(&utc);
    if (err != ESP_OK) return err;

    if (s_utc_offset_s == 0) { *out = utc; return ESP_OK; }

    // Via the epoch so the offset carries across minute, hour, day, month and
    // year boundaries correctly — a watch in Nicosia at 23:30 UTC is already
    // on TOMORROW's date locally, and the face shows the date.
    uint32_t epoch = rr_rtc_get_epoch();
    if (epoch == 0) { *out = utc; return ESP_OK; }

    int64_t local = (int64_t) epoch + s_utc_offset_s;
    if (local < 0) { *out = utc; return ESP_OK; }   // pre-1970: RTC is unset garbage

    time_t e = (time_t) local;
    struct tm tm_local;
    if (gmtime_r(&e, &tm_local) == NULL) { *out = utc; return ESP_OK; }

    out->year   = (uint16_t) (tm_local.tm_year + 1900);
    out->month  = (uint8_t) (tm_local.tm_mon + 1);
    out->day    = (uint8_t) tm_local.tm_mday;
    out->hour   = (uint8_t) tm_local.tm_hour;
    out->minute = (uint8_t) tm_local.tm_min;
    out->second = (uint8_t) tm_local.tm_sec;
    out->osc_ok = utc.osc_ok;   // a validity flag about the hardware, not the zone
    return ESP_OK;
}

// ═════════════════════════════════════════════════════════════════════════════
// Alarm — see the long comment in rr_rtc.h before using any of this.
//
// Short version: the comparator works and is verified; the /INT pin is not
// connected to the ESP32-C6 on this board, so the alarm cannot wake the CPU.
// ═════════════════════════════════════════════════════════════════════════════

// The one pin in the board's GPIO map with no net on it (the vendor pin audit
// jumps 13 → 15, and the MCU symbol carries no GPIO14 line). If RTC_INT were
// quietly routed anywhere the CPU can see, this is the only candidate left —
// so the self-test watches it rather than trusting the schematic alone.
#define UNROUTED_PROBE_GPIO 14

static esp_err_t ctrl2_update(uint8_t set_bits, uint8_t clear_flags)
{
    uint8_t c2 = 0;
    esp_err_t err = reg_read(REG_CONTROL_2, &c2, 1);
    if (err != ESP_OK) return err;

    // AF and TF are "write 0 to clear, write 1 to leave alone" — the opposite
    // of a normal read-modify-write. Writing back a byte you just read would
    // therefore preserve them, and clearing one means deliberately masking it
    // out while holding the OTHER high. Getting this backwards silently eats
    // the timer flag, so both are named explicitly.
    c2 |= (CTRL2_AF_BIT | CTRL2_TF_BIT);   // default: preserve both flags
    c2 &= (uint8_t) ~clear_flags;          // then knock down only what we mean to
    c2 |= set_bits;
    return reg_write(REG_CONTROL_2, &c2, 1);
}

esp_err_t rr_rtc_alarm_at(uint32_t epoch_utc)
{
    time_t t = (time_t) epoch_utc;
    struct tm tm_utc;
    if (gmtime_r(&t, &tm_utc) == NULL) return ESP_ERR_INVALID_ARG;

    // AEN_x = 0 on second/minute/hour/day: all four must match. Weekday is
    // left DISABLED (0x80) — matching both day-of-month and weekday would
    // require them to agree, which they only do once every few years.
    uint8_t a[5] = {
        to_bcd((uint8_t) tm_utc.tm_sec),
        to_bcd((uint8_t) tm_utc.tm_min),
        to_bcd((uint8_t) tm_utc.tm_hour),
        to_bcd((uint8_t) tm_utc.tm_mday),
        ALARM_DISABLE_BIT,
    };

    esp_err_t err = reg_write(REG_ALARM_SECOND, a, sizeof(a));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "alarm register write failed: %s", esp_err_to_name(err));
        return err;
    }

    // Enable the interrupt and clear any stale flag in one write. Order
    // matters: arming the comparator before clearing AF would leave a previous
    // fire looking like this one.
    err = ctrl2_update(CTRL2_AIE_BIT, CTRL2_AF_BIT);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "alarm armed for %04d-%02d-%02d %02d:%02d:%02d UTC",
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
             tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    return ESP_OK;
}

esp_err_t rr_rtc_alarm_cancel(void)
{
    uint8_t a[5] = { ALARM_DISABLE_BIT, ALARM_DISABLE_BIT, ALARM_DISABLE_BIT,
                     ALARM_DISABLE_BIT, ALARM_DISABLE_BIT };
    esp_err_t err = reg_write(REG_ALARM_SECOND, a, sizeof(a));
    if (err != ESP_OK) return err;
    return ctrl2_update(0, CTRL2_AIE_BIT | CTRL2_AF_BIT);
}

esp_err_t rr_rtc_alarm_pending(bool *out_fired, bool clear)
{
    uint8_t c2 = 0;
    esp_err_t err = reg_read(REG_CONTROL_2, &c2, 1);
    if (err != ESP_OK) return err;

    const bool fired = (c2 & CTRL2_AF_BIT) != 0;
    if (out_fired != NULL) *out_fired = fired;
    if (fired && clear) return ctrl2_update(0, CTRL2_AF_BIT);
    return ESP_OK;
}

esp_err_t rr_rtc_alarm_selftest(int seconds_ahead)
{
    if (seconds_ahead < 2) seconds_ahead = 2;

    rr_rtc_time_t before;
    if (rr_rtc_get(&before) != ESP_OK) {
        ESP_LOGE(TAG, "alarm selftest SKIPPED — cannot read the RTC");
        return ESP_FAIL;
    }
    if (!before.osc_ok) {
        // Not a failure of the alarm: an unset clock has no meaningful "now"
        // to schedule against. Say which it is, or this reads as a dead alarm.
        ESP_LOGW(TAG, "alarm selftest SKIPPED — RTC never set (osc flag latched); "
                      "it will run on the boot after the first TIME_SYNC");
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t t0 = rr_rtc_get_epoch();
    const uint32_t target = t0 + (uint32_t) seconds_ahead;

    // Watch the pin the board leaves unrouted. Pulled up, so an open-drain
    // /INT that reached it would read 0 while the alarm is asserted.
    gpio_config_t probe = {
        .pin_bit_mask = 1ULL << UNROUTED_PROBE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const bool probe_ok = gpio_config(&probe) == ESP_OK;
    const int probe_idle = probe_ok ? gpio_get_level(UNROUTED_PROBE_GPIO) : -1;

    esp_err_t err = rr_rtc_alarm_at(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ALARM SELFTEST FAILED — could not arm: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "alarm selftest: armed +%d s, watching AF and GPIO%d (idle level %d)",
             seconds_ahead, UNROUTED_PROBE_GPIO, probe_idle);

    // Poll AF over I2C. This is a ONE-OFF boot check, not how the scheduler
    // works — rr_sched never polls the RTC waiting for a fire.
    const uint32_t deadline = target + 5;
    bool fired = false, probe_moved = false;
    uint32_t observed = 0;

    while (rr_rtc_get_epoch() <= deadline) {
        if (probe_ok && gpio_get_level(UNROUTED_PROBE_GPIO) != probe_idle) probe_moved = true;

        if (rr_rtc_alarm_pending(&fired, false) == ESP_OK && fired) {
            observed = rr_rtc_get_epoch();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (probe_ok) gpio_reset_pin(UNROUTED_PROBE_GPIO);
    rr_rtc_alarm_cancel();

    if (!fired) {
        ESP_LOGE(TAG, "════ ALARM SELFTEST FAILED ════");
        ESP_LOGE(TAG, "AF never set. The comparator is not running — do NOT build "
                      "scheduling on this path until it does.");
        return ESP_ERR_TIMEOUT;
    }

    const int32_t skew = (int32_t) observed - (int32_t) target;
    ESP_LOGI(TAG, "════ ALARM SELFTEST: AF SET, %+" PRId32 " s from target ════", skew);
    ESP_LOGI(TAG, "  the PCF85063 alarm comparator WORKS (verified, not assumed)");
    ESP_LOGI(TAG, "  GPIO%d during the fire: %s", UNROUTED_PROBE_GPIO,
             probe_moved ? "MOVED — /INT may be reachable after all, investigate"
                         : "no change — /INT is not reachable from the CPU");
    if (!probe_moved) {
        ESP_LOGW(TAG, "  → RTC_INT reaches pad P4 only (schematic V1.0), so the RTC "
                      "CANNOT wake the CPU on this board. rr_sched times its own "
                      "wake with the LP timer and reads this RTC for the wall clock.");
    }
    return ESP_OK;
}
