# rr_store — LittleFS cache + durable completion queue (spec Phase 3/5)
#
# Phase 3 implements identity + the routine cache. The completion queue
# (Phase 5) is still absent.
idf_component_register(
    SRCS "src/rr_identity.c" "src/rr_store.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash json
)
