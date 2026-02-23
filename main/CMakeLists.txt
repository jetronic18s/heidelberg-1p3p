set(SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../src")
set(INC_DIR "${CMAKE_CURRENT_LIST_DIR}/../include")

file(GLOB MAIN_SRCS
    "${SRC_DIR}/*.cpp"
)
list(APPEND MAIN_SRCS
    "${SRC_DIR}/modbus_serial_adapter.cpp"
)

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "${INC_DIR}"
    REQUIRES
        arduino
        nvs_flash
        app_update
        esp_http_server
        eModbus
        Uptime
        eth_phy_jl1101
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_DINGTIAN)
