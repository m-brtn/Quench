# Capture check (cmake -P): runs an app with --capture, then compares the capture with a golden image.
#
#   cmake -DAPP=<exe> -DARGS=<a|b|...> -DCAPTURE=<png> -DGOLDEN=<png> -DCOMPARE=<quench_image_compare>
#         [-DENVIRONMENT=<NAME=value|...>] [-DOUTPUTS=<file|...>] -P RunCapture.cmake
#
# Without a golden image the check prints QUENCH_SKIP (the test's SKIP_REGULAR_EXPRESSION) and how to add one: look at
# the capture first, then copy it to GOLDEN. On a mismatch the difference image is written next to the capture.
# OUTPUTS are files the app writes besides the capture: removed before the run, and the check fails without them.

string(REPLACE "|" ";" args "${ARGS}")
string(REPLACE "|" ";" environment "${ENVIRONMENT}")
string(REPLACE "|" ";" outputs "${OUTPUTS}")
file(REMOVE ${CAPTURE} ${outputs})
get_filename_component(capture_dir ${CAPTURE} DIRECTORY)
file(MAKE_DIRECTORY ${capture_dir})

execute_process(COMMAND ${CMAKE_COMMAND} -E env ${environment} ${APP} ${args} --capture ${CAPTURE}
                RESULT_VARIABLE app_result)
if(NOT app_result EQUAL 0)
    message(FATAL_ERROR "the app exited with ${app_result}")
endif()
if(NOT EXISTS ${CAPTURE})
    message(FATAL_ERROR "the app wrote no capture to ${CAPTURE}")
endif()
foreach(output IN LISTS outputs)
    if(NOT EXISTS ${output})
        message(FATAL_ERROR "the app wrote no ${output}")
    endif()
endforeach()
if(NOT EXISTS ${GOLDEN})
    message("QUENCH_SKIP: no golden image ${GOLDEN}. Look at ${CAPTURE}; if it is right, copy it there.")
    return()
endif()

get_filename_component(capture_name ${CAPTURE} NAME_WE)
set(diff ${capture_dir}/${capture_name}_diff.png)
execute_process(COMMAND ${COMPARE} ${CAPTURE} ${GOLDEN} --diff ${diff} RESULT_VARIABLE compare_result)
if(NOT compare_result EQUAL 0)
    message(FATAL_ERROR "the capture ${CAPTURE} differs from ${GOLDEN} (differences: ${diff})")
endif()
file(REMOVE ${diff})
