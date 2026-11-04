# `cmake -P` script run after a test executable is built: writes one add_test() per case it lists (`EXE --list`) into
# OUTPUT, which CTest includes. Parameters: EXE, PREFIX, OUTPUT, WORKING_DIRECTORY.
cmake_minimum_required(VERSION 3.28)
execute_process(COMMAND ${EXE} --list OUTPUT_VARIABLE _out RESULT_VARIABLE _res)
if(NOT _res EQUAL 0)
    message(FATAL_ERROR "DiscoverTests.cmake: '${EXE} --list' failed (${_res})")
endif()
string(REPLACE "\n" ";" _names "${_out}")
set(_content "")
foreach(_name IN LISTS _names)
    if(_name STREQUAL "")
        continue()
    endif()
    string(APPEND _content "add_test([=[${PREFIX}.${_name}]=] [=[${EXE}]=] [=[${_name}]=])\n")
    string(APPEND _content
        "set_tests_properties([=[${PREFIX}.${_name}]=] PROPERTIES WORKING_DIRECTORY [=[${WORKING_DIRECTORY}]=])\n")
endforeach()
file(WRITE "${OUTPUT}" "${_content}")
