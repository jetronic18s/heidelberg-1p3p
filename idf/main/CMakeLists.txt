set(SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src")
set(INC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../include")

file(GLOB MAIN_SRCS
    "${SRC_DIR}/*.cpp"
)

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "${INC_DIR}"
    REQUIRES
        arduino
        AsyncTCP
        ESPAsyncWebServer
        WiFiManager
        eModbus
        ESPTelnet
        Uptime
        eth_phy_jl1101
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_DINGTIAN)
