include("${CMAKE_CURRENT_LIST_DIR}/app_version.cmake")
app_git_describe(APP_VERSION "${PROJECT_DIR}")

set(HEADER_CONTENT "#pragma once

#define APP_VERSION \"${APP_VERSION}\"
")

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(OUTPUT_FILE "${OUTPUT_DIR}/app_git_version.h")
if(EXISTS "${OUTPUT_FILE}")
    file(READ "${OUTPUT_FILE}" OLD_HEADER_CONTENT)
endif()
if(NOT OLD_HEADER_CONTENT STREQUAL HEADER_CONTENT)
    file(WRITE "${OUTPUT_FILE}" "${HEADER_CONTENT}")
endif()
