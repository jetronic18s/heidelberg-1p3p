cmake_minimum_required(VERSION 3.16)

# Disable IDF component manager (offline/component registry not needed).
set(IDF_COMPONENT_MANAGER 0)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/components"
)

# Ensure wrapped log symbols are retained when esp_diagnostics adds --wrap flags.
set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} -Wl,--undefined=__wrap_esp_log_write -Wl,--undefined=__wrap_esp_log_writev"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(heidelberg-1p3p)
