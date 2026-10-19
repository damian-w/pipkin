cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.0.1")
# -DPIPKIN_DEMO=ON builds the demo reel firmware, which pipkin flash --demo installs.
option(PIPKIN_DEMO "Build the demo reel firmware" OFF)
if(PIPKIN_DEMO)
    string(APPEND PROJECT_VER "-demo")
endif()
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pipkin)

# Prebuilt SDK archives contain build-machine paths in their debug sections.
target_link_options(pipkin.elf PRIVATE "-Wl,--strip-debug")

if(NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" VERSION_EQUAL "5.4.2")
    message(FATAL_ERROR "Pipkin requires ESP-IDF v5.4.2")
endif()

# esp32 builds the 2.8-inch CYD (ESP32-2432S028R) and esp32s3 the 4-inch ES3C40P.
if(NOT IDF_TARGET STREQUAL "esp32" AND NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "Pipkin boards use the esp32 or esp32s3 target")
endif()
