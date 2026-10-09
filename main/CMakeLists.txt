idf_component_register(
    SRCS "main.cpp" "display.cpp" "../src/model.cpp" "../src/protocol.cpp" "../src/render.cpp"
    INCLUDE_DIRS "." "../include"
    REQUIRES esp_app_format esp_driver_gpio esp_driver_ledc esp_driver_spi esp_driver_uart esp_lcd esp_timer nvs_flash
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
target_compile_definitions(${COMPONENT_LIB} PRIVATE PIPKIN_GIT_REVISION="${PIPKIN_GIT_REVISION}")
