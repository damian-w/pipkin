cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.0.0")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pipkin)

# Prebuilt SDK archives contain build-machine paths in their debug sections.
target_link_options(pipkin.elf PRIVATE "-Wl,--strip-debug")

if(NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" VERSION_EQUAL "5.4.2")
    message(FATAL_ERROR "Pipkin requires ESP-IDF v5.4.2")
endif()

if(NOT IDF_TARGET STREQUAL "esp32")
    message(FATAL_ERROR "The provisional board profile requires the esp32 target")
endif()
