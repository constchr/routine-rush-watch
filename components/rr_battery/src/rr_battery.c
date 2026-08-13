// rr_battery — AXP2101 fuel gauge + charge state.
//
// NOTE: this is NEW in this phase. Phase 0 read the PMIC with a throwaway
// bring-up probe and Phase 5 deliberately left QUEUE_STATUS.batt at 0 rather
// than guess, so there was no battery code to reuse.
//
// Register map from the AXP2101 datasheet, confirmed against the Phase 0 probe
// on this board (chip ID 0x4A, ~4.10 V / 82% while on USB).

#include "rr_battery.h"

#include <string.h>
#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "rr_battery";

#define AXP2101_ADDR 0x34
#define REG_STATUS0  0x00   // bit5 vbus_good, bit3 batt_present
#define REG_STATUS1  0x01   // bits[6:5] charge state
#define REG_ADC_EN   0x30
#define REG_VBAT_H   0x34
#define REG_SOC      0xA4   // battery percentage, 0..100

static i2c_master_dev_handle_t s_dev;
static bool s_ready;

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 1000);
}

esp_err_t rr_battery_init(i2c_master_bus_handle_t bus)
{
    if (s_ready) return ESP_OK;
    if (bus == NULL) return ESP_ERR_INVALID_ARG;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) return err;

    uint8_t id = 0;
    if (rd(0x03, &id, 1) != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 not responding at 0x%02X", AXP2101_ADDR);
        return ESP_FAIL;
    }

    // Enable the battery-voltage ADC. Without this VBAT reads zero, which is
    // indistinguishable from a flat battery.
    uint8_t en[2] = { REG_ADC_EN, 0x01 };
    i2c_master_transmit(s_dev, en, 2, 1000);

    s_ready = true;
    ESP_LOGI(TAG, "AXP2101 ready (chip id 0x%02X)", id);
    return ESP_OK;
}

esp_err_t rr_battery_read(rr_battery_t *out)
{
    if (!s_ready || out == NULL) return ESP_ERR_INVALID_STATE;
    memset(out, 0, sizeof(*out));

    uint8_t soc = 0, s0 = 0, s1 = 0, v[2] = {0};
    if (rd(REG_SOC, &soc, 1) != ESP_OK) return ESP_FAIL;
    rd(REG_STATUS0, &s0, 1);
    rd(REG_STATUS1, &s1, 1);
    if (rd(REG_VBAT_H, v, 2) == ESP_OK) {
        out->millivolts = (uint16_t) (((v[0] & 0x3F) << 8) | v[1]);
    }

    out->percent = soc > 100 ? 100 : soc;
    out->vbus_present = (s0 & 0x20) != 0;
    out->battery_present = (s0 & 0x08) != 0;

    // STATUS1 bits[6:5]: 01 = charging. Treat "USB present AND actively
    // charging" as charging; USB alone can also mean "charged, floating".
    out->charging = out->vbus_present && (((s1 >> 5) & 0x03) == 0x01);

    out->valid = true;
    return ESP_OK;
}

bool rr_battery_is_ready(void) { return s_ready; }
