# Derive the firmware version from the latest "v*" tag, e.g. "v2.0.0",
# "v2.0.0-3-gf4881c3a" (3 commits after the tag) or with "-dirty" suffix.
# Falls back to the commit hash if no version tag is reachable.
function(app_git_describe OUT_VAR SOURCE_DIR)
    set(version "unknown")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v*" --always --dirty --abbrev=8
            WORKING_DIRECTORY "${SOURCE_DIR}"
            OUTPUT_VARIABLE describe
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(describe)
            set(version "${describe}")
        endif()
    endif()
    set(${OUT_VAR} "${version}" PARENT_SCOPE)
endfunction()
