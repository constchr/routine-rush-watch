# rr_routine — routine runtime, single-step-focus loop (spec Phase 4)
idf_component_register(
    SRCS "src/rr_routine.c"
    INCLUDE_DIRS "include"
    REQUIRES rr_store rr_ui rr_rtc lvgl__lvgl json
)
