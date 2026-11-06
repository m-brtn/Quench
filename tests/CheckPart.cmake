# Part check (cmake -P): one part of a batch run (RunBatch.cmake) passed its checks, by its line in the log, and its
# capture, if it has one, matches the golden image.
#
#   cmake -DLOG=<file> -DPART=<scenario file name> [-DCAPTURE=<png> -DGOLDEN=<png> -DCOMPARE=<quench_image_compare>]
#         -P CheckPart.cmake
#
# Without a golden image the check prints QUENCH_SKIP (the test's SKIP_REGULAR_EXPRESSION) and how to add one: look at
# the capture first, then copy it to GOLDEN. On a mismatch the difference image is written next to the capture.

if(NOT EXISTS ${LOG})
    message(FATAL_ERROR "no log ${LOG}: the batch run did not start")
endif()
file(READ ${LOG} log)
string(REPLACE "." "\\." part "${PART}")
string(REGEX MATCHALL "[^\n]*FAIL line [0-9]+ of [^\n]*/${part}:[^\n]*" fails "${log}")
foreach(line IN LISTS fails)
    message("${line}")
endforeach()
if(NOT log MATCHES "[ /]${part}: ([0-9]+) passed, ([0-9]+) failed")
    message(FATAL_ERROR "the part ${PART} did not end (log: ${LOG})")
endif()
set(passed ${CMAKE_MATCH_1})
set(failed ${CMAKE_MATCH_2})
message("${PART}: ${passed} passed, ${failed} failed")
if(NOT failed EQUAL 0)
    message(FATAL_ERROR "${failed} checks of ${PART} failed (log: ${LOG})")
endif()

if(NOT CAPTURE)
    return()
endif()
if(NOT EXISTS ${CAPTURE})
    message(FATAL_ERROR "the part wrote no capture ${CAPTURE}")
endif()
if(NOT EXISTS ${GOLDEN})
    message("QUENCH_SKIP: no golden image ${GOLDEN}. Look at ${CAPTURE}; if it is right, copy it there.")
    return()
endif()
get_filename_component(capture_dir ${CAPTURE} DIRECTORY)
get_filename_component(capture_name ${CAPTURE} NAME_WE)
set(diff ${capture_dir}/${capture_name}_diff.png)
execute_process(COMMAND ${COMPARE} ${CAPTURE} ${GOLDEN} --diff ${diff} RESULT_VARIABLE compare_result)
if(NOT compare_result EQUAL 0)
    message(FATAL_ERROR "the capture ${CAPTURE} differs from ${GOLDEN} (differences: ${diff})")
endif()
file(REMOVE ${diff})
