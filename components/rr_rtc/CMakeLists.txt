# rr_rtc — PCF85063 real-time clock.
#
# Promoted out of rr_power in Phase 3B: rr_power gained the factory-reset
# button (which needs rr_ble), and rr_ble needs the RTC, so leaving the RTC in
# rr_power created a dependency cycle. The RTC is the more fundamental of the
# two and has no dependencies of its own.
idf_component_register(
    SRCS "src/rr_rtc.c"
    INCLUDE_DIRS "include"
    # nvs_flash: the UTC offset is persisted so local time survives a reboot
    # with no phone in range.
    # esp_driver_gpio: the alarm self-test samples the board's one unrouted pin
    # to confirm by measurement that RTC /INT reaches no GPIO (see rr_rtc.h).
    REQUIRES esp_driver_i2c esp_driver_gpio nvs_flash
)
