# rr_power — sleep, raise-to-wake, battery (spec Phase 10)
#
# Currently only the local factory-reset button. Power management is a stub;
# the RTC moved to its own rr_rtc component (see that CMakeLists for why).
idf_component_register(
    SRCS "src/rr_reset_button.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio rr_ble rr_ui
)
