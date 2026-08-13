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
# CI asserts it is unset for release builds (.github/workflows/build.yml).
#
# -DRR_DEV_NONCE=1 does two things, both only to let the laptop BLE harnesses
# (tools/*.py) drive an ALREADY-PAIRED watch without unpairing the phone:
#   • main.c mints and prints a pairing nonce even when paired
#   • rr_ble advertises a DIFFERENT static address at EVERY BOOT, so a host
#     holding a stale bond for the real address does not try to encrypt with a
#     key the watch no longer has
# The second one deliberately changes identity rather than weakening the
# encryption gate — the gate is the thing under test everywhere else.
#
# IT USED TO DO A THIRD THING: call ble_gap_security_initiate() on connect.
# That is now unconditional production behaviour (see BLE_GAP_EVENT_CONNECT in
# rr_ble.c) because relying on the central to react to ATT 0x0F is exactly the
# fragility the harnesses kept tripping over, and asking for security ourselves
# is correct for the phone too. What remains behind this flag is only what is
# genuinely test-only: a nonce that should not exist on a paired watch, and an
# identity that deliberately churns.
if(RR_DEV_NONCE)
    idf_build_set_property(COMPILE_DEFINITIONS "RR_DEV_NONCE_WHEN_PAIRED=1" APPEND)
    message(WARNING "RR_DEV_NONCE is set — dev nonce + alternate BLE address. TEST BUILDS ONLY.")
endif()

# -DRR_ROTATE_BLE_ADDR=1 gives the watch a fresh BLE identity at boot IF it is
# paired with zero bonds — the state where the phone holds a key the watch lost,
# every connection fails encryption, and no iOS app can forget a BLE bond. It
# keeps device_id (so the server-side pairing survives) and costs one stale entry
# in the phone's bond list. Opt-in because doing it automatically turned a crash
# loop into repeated pairing prompts; see on_sync().
if(RR_ROTATE_BLE_ADDR)
    idf_build_set_property(COMPILE_DEFINITIONS "RR_ROTATE_BLE_ADDR=1" APPEND)
    message(WARNING "RR_ROTATE_BLE_ADDR is set — the watch may present a NEW BLE identity.")
endif()

# -DRR_CLEAR_BONDS=1 wipes the BLE bond store once at boot and nothing else.
#
# Recovery for the state where the paired phone's bond has been evicted: the watch
# advertises and answers scans but every connection fails encryption, which looks
# like "sync is stuck". Flash this, boot once, connect the phone to re-bond, then
# flash a normal build. It does NOT touch device_id, pairing state or the cache —
# a factory reset would, and that costs a QR re-registration.
if(RR_CLEAR_BONDS)
    idf_build_set_property(COMPILE_DEFINITIONS "RR_CLEAR_BONDS=1" APPEND)
    message(WARNING "RR_CLEAR_BONDS is set — this image ERASES ALL BLE BONDS at boot.")
endif()

# -DRR_POWERLOG=1 compiles in the SOC-drop power measurement task (docs/POWER.md).
# There is no current-sense hardware on this board and the AXP2101 has no battery
# current ADC, so timing the fuel gauge is the only real measurement available.
if(RR_POWERLOG)
    idf_build_set_property(COMPILE_DEFINITIONS "RR_POWERLOG=1" APPEND)
    message(STATUS "RR_POWERLOG enabled — battery SOC will be logged every 60 s")
endif()

# -DRR_PED_SELFTEST=1 re-runs the QMI8658 on-chip step-engine experiment at boot
# and reports the counter alongside real accelerometer motion, so "inert engine"
# and "nobody moved it" stay distinguishable. Phase 9 concluded the engine does
# not count on this part; this is how that conclusion gets re-checked on a new
# board or a new silicon revision instead of being taken on faith.
# -DRR_LIGHT_SLEEP=1 enables automatic CPU light sleep. OFF BY DEFAULT: it is
# implemented and the PM locks are correct, but LVGL's millisecond-period tick
# timer thrashes against the sleep entry/exit overhead and starves both the IDLE
# and main tasks (reproduced on hardware — see the comment in main.c). Gating the
# LVGL tick on the panel being lit is the remaining work.
if(RR_LIGHT_SLEEP)
    idf_build_set_property(COMPILE_DEFINITIONS "RR_LIGHT_SLEEP=1" APPEND)
    message(WARNING "RR_LIGHT_SLEEP is set — KNOWN ISSUE: IDLE/main task starvation via the LVGL tick.")
endif()

# -DRR_AUDIO_BOOT_TEST=1 plays one tone at boot, to confirm the Phase 10 I2S
# rewrite (rr_audio owns bring-up so the channel is not left enabled holding a
# PM lock) did not break the only alerting channel this board has.
if(RR_AUDIO_BOOT_TEST)
    idf_build_set_property(COMPILE_DEFINITIONS "RR_AUDIO_BOOT_TEST=1" APPEND)
    message(WARNING "RR_AUDIO_BOOT_TEST is set — this image plays a tone at every boot.")
endif()

if(RR_PED_SELFTEST)
    idf_build_set_property(COMPILE_DEFINITIONS "RR_PED_SELFTEST=1" APPEND)
    message(WARNING "RR_PED_SELFTEST is set — boot runs a 30 s pedometer test. WALK WITH THE WATCH.")
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

# ⚠️ WIPE THE STAGING DIRECTORY FIRST. file(COPY) only ever ADDS, so an asset
# deleted from the repo stays in build/lfs_image forever and keeps being packed
# into the image. Removing the 36 voice clips proved it: the source directory
# dropped 647 KiB and the staged audio directory still measured 956 KiB, so the
# partition would have carried every deleted clip until someone ran a clean
# build. Re-copying costs a moment at configure time and makes the image a
# function of the repo rather than of build history.
file(REMOVE_RECURSE "${LFS_STAGE}")

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
# Measured in build/lfs_image against the 6.875 MiB partition, after the voice
# prompts were removed:
#     emoji    4200 KB
#     avatars  1392 KB
#     audio     228 KB   (6 PCM tones; was 843 KB with the ADPCM voice set)
#     ─────────────────
#     total    5820 KB   into 7040 KB — 1220 KB spare (was ~560 KB)
#
# The voice set was 36 clips: 2546 KB as 16-bit PCM, 653 KB as IMA ADPCM, which
# is why Phase 8 needed a second decoder at all. It is gone because pre-rendered
# speech cannot cover free-text step labels (see the generator header) — so the
# partition got 625 KB back, rr_audio lost its ADPCM path, and the tones that
# remain use the one playback path verified on hardware.
#
# The glob is still explicit about what it takes. If the emoji set grows, this is
# where it will overflow, and a silent overflow at image build time would present
# as "some clips just do not play".
file(MAKE_DIRECTORY "${LFS_STAGE}/audio")
file(GLOB RR_AUDIO_WAVS "${CMAKE_SOURCE_DIR}/assets/audio/tone_*.wav")
file(COPY ${RR_AUDIO_WAVS} DESTINATION "${LFS_STAGE}/audio")

# ⚠️ NOT FLASH_IN_PROJECT — deliberately.
#
# THE PROBLEM THIS SOLVES. `idf.py flash` used to rewrite the whole littlefs
# partition, and that partition holds /lfs/queue/runs.log — the durable queue of
# completed routines that have not reached the server yet. rr_store.h calls that
# queue "the ONE thing on the watch that is not disposable", and a routine flash
# destroyed it. It has already happened in this project: a watch carrying two
# unsynced runs came back from an asset flash with queued=0, and those two
# completed routines are simply gone.
#
# So the image is still BUILT (assets are staged and packed on every build) but
# not flashed automatically. Writing it is now an explicit act:
#
#     tools/flash-assets.sh [PORT]
#
# WHY THIS AND NOT A WARNING: a warning has to know whether unsynced runs exist,
# and the only thing that knows is the watch — which is not running while esptool
# has the port. Any check would be a guess, and a guess that says "probably fine"
# is worse than no check. Making the destructive write opt-in removes the class
# of accident instead of narrating it.
#
# THE COST, STATED: a fresh board flashed with `idf.py flash` alone has no emoji,
# no avatars and no audio. That fails LOUDLY rather than subtly — rr_audio logs
# "is the littlefs asset image flashed?", and the face falls back to a dot for
# every missing emoji — and the fix is one script. Worth it against silently
# eating a child's completed routines.
littlefs_create_partition_image(littlefs "${LFS_STAGE}")
