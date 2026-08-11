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

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"

static const char *TAG = "rr_rtc";

#define PCF85063_ADDR       0x51
#define REG_CONTROL_1       0x00
#define REG_SECONDS         0x04
#define CTRL1_STOP_BIT      0x20
#define SECONDS_OS_BIT      0x80

static i2c_master_dev_handle_t s_dev;

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
