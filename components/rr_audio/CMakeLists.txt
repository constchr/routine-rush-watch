# rr_audio — ES8311 playback.
#
# Phase 7 built the minimum that makes the scheduler audible (there is no
# vibration motor on this board, so a silent alarm is no alarm). The full
# subsystem — voice prompts in the runtime, chimes, mixing — is Phase 8.
idf_component_register(
    SRCS "src/rr_audio.c"
    # assets/audio carries the GENERATED audio_manifest.h: the tone enum and
    # the /lfs/audio/... path table. Same arrangement as rr_ui and the emoji
    # manifest — generated tables live with the assets, not in include/.
    INCLUDE_DIRS "include" "../../assets/audio"
    REQUIRES esp_driver_i2s espressif__esp_codec_dev nvs_flash rr_rtc
             waveshare__esp32_c6_touch_amoled_2_06
)
