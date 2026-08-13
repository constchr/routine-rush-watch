#pragma once
// rr_battery — AXP2101 fuel gauge + charge state.
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

typedef struct {
    bool     valid;
    uint8_t  percent;          /**< 0..100 from the PMIC's own gauge */
    uint16_t millivolts;
    bool     charging;
    bool     vbus_present;
    bool     battery_present;
} rr_battery_t;

/** Attach to the shared board I2C bus. Call after bsp_i2c_init(). */
esp_err_t rr_battery_init(i2c_master_bus_handle_t bus);

/**
 * Read the gauge. Cheap (a few I2C reads) but NOT free — call it on wake,
 * not on a timer. A watch-face percentage that is a few minutes stale is
 * fine; waking the CPU to refresh it is not.
 */
esp_err_t rr_battery_read(rr_battery_t *out);

bool rr_battery_is_ready(void);
