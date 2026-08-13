# rr_battery — AXP2101 fuel gauge (percent, millivolts, charge state).
#
# PROMOTED OUT OF rr_power so the BLE server can report the battery.
# rr_power REQUIRES rr_ble (the idle face pushes state to the uplink), so
# rr_ble could not depend on rr_power to read the gauge without a cycle — and
# QUEUE_STATUS carries a `batt` byte that was hardcoded to 0 for exactly that
# reason. The gauge itself needs nothing but the shared I2C bus, so it belongs
# below both. Same reason rr_rtc was promoted out of rr_power in Phase 3B.
idf_component_register(
    SRCS "src/rr_battery.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
)
