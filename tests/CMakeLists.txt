add_library(quench_test_harness STATIC harness/TestMain.cpp)
quench_configure_target(quench_test_harness)
target_include_directories(quench_test_harness PUBLIC harness)

# quench_add_test_executable(<name> PREFIX <ctest prefix> SOURCES <file>... LIBS <target>...)
function(quench_add_test_executable name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "PREFIX" "SOURCES;LIBS")
    add_executable(${name} ${arg_SOURCES})
    quench_configure_target(${name})
    target_link_libraries(${name} PRIVATE quench_test_harness ${arg_LIBS})
    set(list_file ${CMAKE_CURRENT_BINARY_DIR}/${name}_cases.cmake)
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:${name}> -DPREFIX=${arg_PREFIX} -DOUTPUT=${list_file}
                -DWORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR} -DSKIP_RETURN_CODE=77
                "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}" -P ${CMAKE_CURRENT_SOURCE_DIR}/DiscoverTests.cmake
        VERBATIM)
    set(include_file ${CMAKE_CURRENT_BINARY_DIR}/${name}_include.cmake)
    file(CONFIGURE OUTPUT ${include_file} CONTENT
"if(EXISTS [=[${list_file}]=])
    include([=[${list_file}]=])
else()
    add_test([=[${name}_NOT_BUILT]=] [=[${name}_NOT_BUILT]=])
endif()
")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES ${include_file})
endfunction()

quench_add_test_executable(quench_core_tests PREFIX core
    SOURCES
        core/AssertTests.cpp
        core/FileSystemTests.cpp
        core/FixedStepClockTests.cpp
        core/JsonTests.cpp
        core/LogTests.cpp
        core/ResultTests.cpp
        core/ThreadTests.cpp
    LIBS quench_core)
target_compile_definitions(quench_core_tests PRIVATE QUENCH_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

quench_add_test_executable(quench_physics_tests PREFIX physics
    SOURCES physics/PhysicsWorldTests.cpp
    LIBS quench_physics)

quench_add_test_executable(quench_gpu_tests PREFIX gpu
    SOURCES gpu/ShaderTests.cpp
    LIBS quench_gpu)
quench_add_shaders(quench_gpu_tests NAME test SOURCE gpu/shaders/test.hlsl
    ENTRIES test_vs:vertex test_fs:fragment
    INCLUDE_DIRS gpu/shaders/include)
target_compile_definitions(quench_gpu_tests PRIVATE
    QUENCH_TEST_METAL=$<BOOL:${APPLE}>
    QUENCH_TEST_METALLIB=$<BOOL:${QUENCH_SHADER_METAL_OK}>
    QUENCH_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/quench_shaders/test")

# Every public header compiles on its own and twice in a row (#pragma once).
file(GLOB_RECURSE public_headers CONFIGURE_DEPENDS ${Quench_SOURCE_DIR}/include/quench/*.h)
set(header_sources "")
foreach(header IN LISTS public_headers)
    file(RELATIVE_PATH rel ${Quench_SOURCE_DIR}/include ${header})
    string(MAKE_C_IDENTIFIER ${rel} id)
    set(src ${CMAKE_CURRENT_BINARY_DIR}/headers/${id}.cpp)
    file(CONFIGURE OUTPUT ${src} CONTENT "#include <${rel}>\n#include <${rel}>\n")
    list(APPEND header_sources ${src})
endforeach()
add_library(quench_header_check OBJECT ${header_sources})
quench_configure_target(quench_header_check)
target_link_libraries(quench_header_check PRIVATE quench_core quench_physics quench_gpu)
