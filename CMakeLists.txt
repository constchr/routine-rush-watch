# Routine Rush Watch — ESP32-C6 firmware
# Requires ESP-IDF v5.5+ (see sdkconfig.defaults for why).
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Must come AFTER project.cmake — that is what defines IDF_VERSION_*.
# Fail loudly and early: on older IDF the real symptom is an opaque dependency
# solver error about the Waveshare BSP, which is much harder to diagnose.
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_LESS "5.5")
    message(FATAL_ERROR
        "Routine Rush Watch requires ESP-IDF v5.5 or newer "
        "(found ${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}). "
        "The board BSP waveshare/esp32_c6_touch_amoled_2_06 declares idf >=5.5.0 "
        "and will not resolve on older IDF.")
endif()

project(routine_rush_watch)

# ⚠️ TEMPORARY TEST SWITCH — never build a shipping image with it.
#
# -DRR_DEV_NONCE=1 does two things, both only to let the laptop BLE harnesses
# (tools/*.py) drive an ALREADY-PAIRED watch without unpairing the phone:
#   • main.c mints and prints a pairing nonce even when paired
#   • rr_ble advertises a DIFFERENT static address, so a host holding a stale
#     bond for the real address does not try to encrypt with a key the watch
#     no longer has (macOS does exactly this, and reports it as ATT 0x0F with
#     no attempt to re-pair)
# The second one deliberately changes identity rather than weakening the
# encryption gate — the gate is the thing under test everywhere else.
if(RR_DEV_NONCE)
    idf_build_set_property(COMPILE_DEFINITIONS "RR_DEV_NONCE_WHEN_PAIRED=1" APPEND)
    message(WARNING "RR_DEV_NONCE is set — dev nonce + alternate BLE address. TEST BUILDS ONLY.")
endif()

# ── littlefs image: the emoji set ────────────────────────────────────────────
# The manifest expects the images under an "emoji/" directory INSIDE the
# partition, so stage them into that shape at configure time rather than
# restructuring assets/ (the emoji are generated into assets/emoji/ by the app
# repo's tooling).
#
# ⚠️ Flashing this image REPLACES the whole littlefs partition, which includes
# the routine cache written at runtime. That is acceptable — the cache is
# disposable by design and refills on the next ROUTINE_PUSH — but it does mean
# a firmware flash with FLASH_IN_PROJECT un-caches the watch's routines.
set(LFS_STAGE "${CMAKE_BINARY_DIR}/lfs_image")
file(MAKE_DIRECTORY "${LFS_STAGE}/emoji")
file(GLOB RR_EMOJI_BINS "${CMAKE_SOURCE_DIR}/assets/emoji/*.bin")
file(COPY ${RR_EMOJI_BINS} DESTINATION "${LFS_STAGE}/emoji")

# Hero-size avatars (280px) for the idle face. Separate directory because they
# are a different asset class from the 96px step emoji, generated on demand for
# the six AVATARS the app offers rather than the whole icon set.
file(MAKE_DIRECTORY "${LFS_STAGE}/avatars")
file(GLOB RR_AVATAR_BINS "${CMAKE_SOURCE_DIR}/assets/avatars/*.bin")
file(COPY ${RR_AVATAR_BINS} DESTINATION "${LFS_STAGE}/avatars")

# Audio (Phase 7). The generated audio_manifest.h hardcodes "/lfs/audio/...",
# so the clips must land in an "audio/" directory inside the partition to be
# findable — same contract the emoji manifest has.
#
# These were generated in Phase 6 but never staged, so the partition on the
# board has no audio in it at all. That is worth stating plainly: an alarm is
# the ONLY alerting channel on a watch with no vibration motor, and the failure
# mode of a missing asset is a scheduled routine that fires perfectly and
# silently. Flashing the app alone (idf.py app-flash) does NOT fix it — the
# asset image has to go down with it at least once.
#
# ⚠️ TONES ONLY — THE VOICE PROMPTS DO NOT FIT, AND PHASE 8 MUST SOLVE THAT.
# Measured against this partition:
#     emoji    4.1 MB
#     avatars  1.4 MB
#     tones    0.2 MB   (5 clips — staged here)
#     voices   2.5 MB   (36 clips — NOT staged)
#     ─────────────────
#     total    8.2 MB into a 6.9 MB partition
# The five tones fit in what is left; the voice set is 1.1 MB past the end.
# Globbing *.wav would silently overflow the image at build time, so the
# pattern is deliberately narrow rather than "everything, and hope".
#
# Phase 8 owns the fix and has real options: the clips are 22.05 kHz 16-bit
# PCM, so IMA-ADPCM alone is a 4x saving, and the emoji set is the bigger
# tenant anyway. Phase 7 does not need to make that call — it needs an alarm
# tone, and that fits today.
file(MAKE_DIRECTORY "${LFS_STAGE}/audio")
file(GLOB RR_AUDIO_WAVS "${CMAKE_SOURCE_DIR}/assets/audio/tone_*.wav")
file(COPY ${RR_AUDIO_WAVS} DESTINATION "${LFS_STAGE}/audio")

littlefs_create_partition_image(littlefs "${LFS_STAGE}" FLASH_IN_PROJECT)
