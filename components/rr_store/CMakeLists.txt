# rr_store — LittleFS routine cache + durable queue (spec Phase 3/5)
# SCAFFOLD: no sources yet. Add them to SRCS as the module is implemented.
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES esp_partition spiffs nvs_flash
)
