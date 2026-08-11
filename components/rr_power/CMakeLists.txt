# rr_power — sleep, raise-to-wake, battery (spec Phase 10)
# SCAFFOLD: no sources yet. Add them to SRCS as the module is implemented.
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES esp_pm nvs_flash
)
