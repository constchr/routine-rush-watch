# rr_store — LittleFS cache + durable completion queue (spec Phase 3/5)
#
# Phase 2 implements rr_identity.c only (NVS device_id + pairing nonce).
# The cache and queue are still stubs.
idf_component_register(
    SRCS "src/rr_identity.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
)
