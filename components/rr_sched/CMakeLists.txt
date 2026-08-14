# rr_sched — the on-watch scheduler (spec §7, Phase 7).
#
# Deliberately does NOT depend on rr_power, even though a fire has to wake the
# screen: rr_power depends on rr_ble, and rr_ble depends on this component so
# that a ROUTINE_PUSH can re-arm the schedule. The wake is a hook that main.c
# fills in with rr_idle_wake_manual — the same shape rr_routine already uses.
idf_component_register(
    SRCS "src/rr_sched.c"
    INCLUDE_DIRS "include"
    # nvs_flash: the last handled fire is persisted, so a reboot neither
    # repeats a routine that already rang nor loses one it slept through.
    # rr_ui dropped when the alarm screen was replaced by READY: the fire
    # raises no screen itself any more, it goes through rr_routine like the
    # phone's remote start does.
    REQUIRES rr_store rr_rtc rr_routine rr_audio nvs_flash
)
