# rr_power — sleep/wake, the idle face, and the local factory-reset button.
# Real power tuning (CPU light-sleep, BLE duty cycle) is Phase 10.
idf_component_register(
    SRCS "src/rr_reset_button.c" "src/rr_idle.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio rr_ble rr_ui rr_imu rr_rtc rr_store lvgl__lvgl
             waveshare__esp32_c6_touch_amoled_2_06
)
