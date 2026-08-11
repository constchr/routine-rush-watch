# rr_imu — QMI8658 shared bring-up: raise-to-wake (Phase 6) + pedometer (Phase 9)
idf_component_register(
    SRCS "src/rr_imu.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_gpio waveshare__qmi8658
)
