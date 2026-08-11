# rr_power — sleep, raise-to-wake, battery (spec Phase 10)
#
# Phase 1 uses only rr_rtc.c (PCF85063), which lives here because it shares the
# board I2C bus. The power management itself is still a stub.
idf_component_register(
    SRCS "src/rr_rtc.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
)
