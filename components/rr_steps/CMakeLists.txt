# rr_steps — daily step count over the QMI8658 on-chip pedometer (spec §10.1)
#
# The sensor and the step engine belong to rr_imu (the shared sensor owner since
# Phase 6). This component only turns that engine's counter into a daily total,
# resets it at LOCAL midnight (hence rr_rtc), and persists it (hence nvs_flash).
idf_component_register(
    SRCS "src/rr_steps.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash rr_imu rr_rtc
)
