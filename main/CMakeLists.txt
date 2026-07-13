set(SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../src")
set(INC_DIR "${CMAKE_CURRENT_LIST_DIR}/../include")
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

set(MAIN_SRCS
    "${SRC_DIR}/config.cpp"
    "${SRC_DIR}/debug.cpp"
    "${SRC_DIR}/ethernet_jl1101.cpp"
    "${SRC_DIR}/main.cpp"
    "${SRC_DIR}/pages.cpp"
    "${SRC_DIR}/switch.cpp"
    "${SRC_DIR}/wifi_native.cpp"
    "${SRC_DIR}/uptime_native.cpp"
)

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "${INC_DIR}" "${GENERATED_DIR}"
    REQUIRES
        nvs_flash
        app_update
        esp_http_server
        esp-modbus
        eth_phy_jl1101
        esp_eth
        esp_wifi
        esp_netif
        esp_timer
        esp_event
        esp_system
        driver
)

add_custom_target(app_git_version
    COMMAND "${CMAKE_COMMAND}"
        -DPROJECT_DIR=${CMAKE_SOURCE_DIR}
        -DOUTPUT_DIR=${GENERATED_DIR}
        -P "${CMAKE_SOURCE_DIR}/cmake/git_version.cmake"
    BYPRODUCTS "${GENERATED_DIR}/app_git_version.h"
    VERBATIM
)
add_dependencies(${COMPONENT_LIB} app_git_version)

target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_DINGTIAN)
