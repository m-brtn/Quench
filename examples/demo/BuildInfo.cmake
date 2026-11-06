# Writes OUTPUT (demo_build_info.h) with the short commit of SOURCE_DIR, "unknown" without git; the file is touched
# only when its text changes, so a build without a new commit compiles nothing again.
find_package(Git QUIET)
set(commit unknown)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${SOURCE_DIR} rev-parse --short HEAD
        OUTPUT_VARIABLE out OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result ERROR_QUIET)
    if(result EQUAL 0 AND out)
        set(commit ${out})
    endif()
endif()
set(text "#pragma once\n#define QUENCH_DEMO_COMMIT \"${commit}\"\n")
if(EXISTS ${OUTPUT})
    file(READ ${OUTPUT} old)
endif()
if(NOT old STREQUAL text)
    file(WRITE ${OUTPUT} "${text}")
endif()
