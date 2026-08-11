# rr_rtc — PCF85063 real-time clock.
#
# Promoted out of rr_power in Phase 3B: rr_power gained the factory-reset
# button (which needs rr_ble), and rr_ble needs the RTC, so leaving the RTC in
# rr_power created a dependency cycle. The RTC is the more fundamental of the
# two and has no dependencies of its own.
idf_component_register(
    SRCS "src/rr_rtc.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
)
