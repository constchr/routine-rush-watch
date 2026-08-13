# rr_ble — RR_SYNC GATT server, the sole uplink (spec Phase 1)
#
# Phase 1 implements TIME_SYNC only. ble_contract.h is GENERATED into
# include/ by tools/gen-ble-contract.mjs — run it before building.
idf_component_register(
    SRCS "src/rr_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer bt nvs_flash json rr_store rr_ui rr_rtc rr_routine rr_sched rr_audio rr_battery rr_steps
)
