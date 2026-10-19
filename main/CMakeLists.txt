# The esp32 target builds the 2.8-inch CYD (ESP32-2432S028R); esp32s3 builds the 4-inch ES3C40P.
set(requires esp_app_format esp_driver_gpio esp_driver_ledc esp_driver_spi esp_lcd esp_timer nvs_flash)
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND requires esp_driver_i2c esp_driver_usb_serial_jtag)
    set(board_definitions PIPKIN_BOARD_ES3C40P=1 PIPKIN_DISPLAY_480X320=1)
else()
    list(APPEND requires esp_driver_uart)
    set(board_definitions)
endif()

idf_component_register(
    SRCS "main.cpp" "display.cpp" "link.cpp" "../src/model.cpp" "../src/protocol.cpp" "../src/render.cpp" "../src/demo.cpp"
    INCLUDE_DIRS "." "../include"
    REQUIRES ${requires}
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

execute_process(COMMAND git -C "${PROJECT_DIR}" rev-parse --verify HEAD
    RESULT_VARIABLE git_result OUTPUT_QUIET ERROR_QUIET)
if(git_result EQUAL 0)
    git_describe(PIPKIN_GIT_REVISION "${PROJECT_DIR}" "--exclude=*" "--abbrev=12")
else()
    set(PIPKIN_GIT_REVISION "Uncommitted")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE PIPKIN_GIT_REVISION="${PIPKIN_GIT_REVISION}"
    PIPKIN_DEMO=$<BOOL:${PIPKIN_DEMO}> ${board_definitions})
