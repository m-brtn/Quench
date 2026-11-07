# Batch run (cmake -P): runs an app once on a scenario of parts (`include`, docs/app.md), with a fresh user directory
# for the captures it writes and its log in a file; CheckPart.cmake then checks each part from the two.
#
#   cmake -DAPP=<exe> -DARGS=<a|b|...> -DSCRIPT=<scenario> -DUSER_DIR=<dir> -DLOG=<file>
#         [-DENVIRONMENT=<NAME=value|...>] [-DGPU_BACKEND=<sdl|metal> -DGPU_DRIVER=<driver>] -P RunBatch.cmake
#
# Fails when the run does not reach the end of its scenario (a crash, a hang, a script that does not parse) or, with
# GPU_BACKEND, when its GPU device is not on that backend and driver (a fallback); a failed check fails only its part's
# test (the app exits with 1).

string(REPLACE "|" ";" args "${ARGS}")
string(REPLACE "|" ";" environment "${ENVIRONMENT}")
file(REMOVE_RECURSE ${USER_DIR})
file(MAKE_DIRECTORY ${USER_DIR})
file(REMOVE ${LOG})

execute_process(COMMAND ${CMAKE_COMMAND} -E env ${environment} ${APP} ${args} --script ${SCRIPT} --user-dir ${USER_DIR}
                OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
file(WRITE ${LOG} "${output}")
message("${output}")
if(NOT result EQUAL 0 AND NOT result EQUAL 1)
    message(FATAL_ERROR "the app exited with ${result} (log: ${LOG})")
endif()
if(GPU_BACKEND AND NOT output MATCHES "gpu: backend ${GPU_BACKEND}, driver ${GPU_DRIVER},")
    message(FATAL_ERROR "the app did not run on backend ${GPU_BACKEND}, driver ${GPU_DRIVER} (log: ${LOG})")
endif()
get_filename_component(script_name ${SCRIPT} NAME)
string(REPLACE "." "\\." script_name "${script_name}")
if(NOT output MATCHES "scenario: [^\n]*/${script_name}: [0-9]+ passed, [0-9]+ failed")
    message(FATAL_ERROR "the app did not reach the end of ${SCRIPT} (log: ${LOG})")
endif()
