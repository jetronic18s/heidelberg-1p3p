set(GIT_SHA "unknown")
set(GIT_DIRTY "0")

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=8 HEAD
        WORKING_DIRECTORY "${PROJECT_DIR}"
        OUTPUT_VARIABLE GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT GIT_SHA)
        set(GIT_SHA "unknown")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" diff --quiet --ignore-submodules --
        WORKING_DIRECTORY "${PROJECT_DIR}"
        RESULT_VARIABLE GIT_WORKTREE_DIRTY
        OUTPUT_QUIET
        ERROR_QUIET
    )
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" diff --cached --quiet --ignore-submodules --
        WORKING_DIRECTORY "${PROJECT_DIR}"
        RESULT_VARIABLE GIT_INDEX_DIRTY
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(GIT_WORKTREE_DIRTY OR GIT_INDEX_DIRTY)
        set(GIT_DIRTY "1")
    endif()
endif()

set(HEADER_CONTENT "#pragma once

#define APP_GIT_SHA \"${GIT_SHA}\"
#define APP_GIT_DIRTY ${GIT_DIRTY}
")

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(OUTPUT_FILE "${OUTPUT_DIR}/app_git_version.h")
if(EXISTS "${OUTPUT_FILE}")
    file(READ "${OUTPUT_FILE}" OLD_HEADER_CONTENT)
endif()
if(NOT OLD_HEADER_CONTENT STREQUAL HEADER_CONTENT)
    file(WRITE "${OUTPUT_FILE}" "${HEADER_CONTENT}")
endif()
