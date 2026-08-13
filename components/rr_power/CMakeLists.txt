# rr_power — sleep/wake, the idle face, and the local factory-reset button.
# Phase 10 added rr_pm: CPU light sleep and the lock that keeps the panel alive.
idf_component_register(
    SRCS "src/rr_reset_button.c" "src/rr_idle.c" "src/rr_battery.c" "src/rr_powerlog.c"
         "src/rr_pm.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_pm esp_timer rr_ble rr_ui rr_imu rr_rtc rr_steps rr_store lvgl__lvgl
             waveshare__esp32_c6_touch_amoled_2_06
)
