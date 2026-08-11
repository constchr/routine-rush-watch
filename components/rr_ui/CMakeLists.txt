# rr_ui — LVGL screens + idle watch face (spec Phase 4/6)
#
# Phase 2 implements display bring-up and the pairing QR only.
idf_component_register(
    SRCS "src/rr_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl waveshare__esp32_c6_touch_amoled_2_06
)
