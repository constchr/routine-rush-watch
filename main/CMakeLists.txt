idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES rr_ble rr_routine rr_store rr_sync rr_audio rr_ui rr_steps rr_power waveshare__esp32_c6_touch_amoled_2_06
)
