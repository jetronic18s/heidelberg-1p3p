cmake_minimum_required(VERSION 3.16)

# Enable IDF component manager.
set(IDF_COMPONENT_MANAGER 1)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/lib/eth_phy_jl1101"
)

# Firmware version from git tags, also embedded in the app description
# (boot log, esp_app_get_description()). Evaluated at configure time.
include("${CMAKE_SOURCE_DIR}/cmake/app_version.cmake")
app_git_describe(PROJECT_VER "${CMAKE_SOURCE_DIR}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(heidelberg-1p3p)
