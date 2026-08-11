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

littlefs_create_partition_image(littlefs "${LFS_STAGE}" FLASH_IN_PROJECT)
