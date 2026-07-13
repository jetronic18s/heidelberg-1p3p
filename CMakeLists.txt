cmake_minimum_required(VERSION 3.16)

# Enable IDF component manager.
set(IDF_COMPONENT_MANAGER 1)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(heidelberg-1p3p)
