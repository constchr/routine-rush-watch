# rr_routine — routine runtime, single-step-focus loop (spec Phase 4)
idf_component_register(
    SRCS "src/rr_routine.c"
    INCLUDE_DIRS "include"
    # waveshare BSP: bsp_display_lock/unlock, so a start requested from another
    # task (BLE now, the scheduler in Phase 7) can hand off to the LVGL task.
    REQUIRES rr_store rr_ui rr_rtc rr_audio lvgl__lvgl json
             waveshare__esp32_c6_touch_amoled_2_06
)
