# Builds the pinned Tint (Dawn's shader compiler) from source into a directory of its own (script mode, run by
# cmake/QuenchTint.cmake):
#   cmake -DTINT_DIR=<install dir> -DWORK_DIR=<scratch dir> [-DJOBS=<n>] [-DNINJA=<ninja>] -P cmake/BuildTint.cmake
#
# Only the `tint` command line tool with its SPIR-V reader and WGSL writer is built (no Dawn backends, no other
# readers or writers); its three dependencies are the commits Dawn's DEPS pins (abseil-cpp through the upstream
# revision of Chromium's mirror). The sources and the build tree are deleted afterwards; TINT_DIR appears only when
# the build is complete.

cmake_minimum_required(VERSION 3.28)
if(NOT TINT_DIR OR NOT WORK_DIR)
    message(FATAL_ERROR "BuildTint.cmake: TINT_DIR and WORK_DIR are required")
endif()

# ---------------------------------------------------------------- Dawn 66f8992 and the dependencies its DEPS pins
set(tint_archives
    "dawn|https://github.com/google/dawn/archive/66f899221d41dbc6d6257350c4b654a9b375c7f2.tar.gz|9aedcfdbc0c4b05379c675018bc7c42bec8bee4b94b5a5d660e31695d86a79b2|."
    "abseil-cpp|https://github.com/abseil/abseil-cpp/archive/cf015c9b764fae69f2140ff2504cb9a4e3de06fc.tar.gz|689091fabffee3f42828d8eb060ca8c9a303cc52626e6833ec89c4bd4a8f2913|third_party/abseil-cpp"
    "SPIRV-Headers|https://github.com/KhronosGroup/SPIRV-Headers/archive/6efe5ae6b7c6ad29da3f2cc3f164b9c3d72b78b1.tar.gz|d035de73b4085301bf921dc66997ce993fbf7c0d78f3308e67b6be2513a8b88c|third_party/spirv-headers/src"
    "SPIRV-Tools|https://github.com/KhronosGroup/SPIRV-Tools/archive/b570f00a782a7bf4006964e8284dd8c10c4c549b.tar.gz|5f03c36bcf930fdc766821eb76f18fe81ab1492f69e4995d7b6f2b87132c8ffb|third_party/spirv-tools/src")

file(REMOVE_RECURSE ${WORK_DIR})
set(src ${WORK_DIR}/src)
set(build ${WORK_DIR}/build)
file(MAKE_DIRECTORY ${src} ${build})
foreach(item IN LISTS tint_archives)
    string(REPLACE "|" ";" fields "${item}")
    list(GET fields 0 name)
    list(GET fields 1 url)
    list(GET fields 2 sha)
    list(GET fields 3 dest)
    set(archive ${WORK_DIR}/${name}.tar.gz)
    message(STATUS "Tint: downloading ${name}")
    file(DOWNLOAD ${url} ${archive} EXPECTED_HASH SHA256=${sha} STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "Tint: downloading ${url} failed: ${status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT ${archive} DESTINATION ${WORK_DIR}/extract-${name})
    file(GLOB top LIST_DIRECTORIES true ${WORK_DIR}/extract-${name}/*)
    if(dest STREQUAL ".")
        file(RENAME ${top} ${src}/root)
    else()
        file(REMOVE_RECURSE ${src}/root/${dest})
        get_filename_component(parent ${src}/root/${dest} DIRECTORY)
        file(MAKE_DIRECTORY ${parent})
        file(RENAME ${top} ${src}/root/${dest})
    endif()
    file(REMOVE ${archive})
endforeach()

if(NOT JOBS)
    cmake_host_system_information(RESULT JOBS QUERY NUMBER_OF_LOGICAL_CORES)
endif()
set(extra_args "")
if(CMAKE_HOST_APPLE)
    # The host's architecture and SDK, whatever the configure that started the build targets.
    execute_process(COMMAND uname -m OUTPUT_VARIABLE host_arch OUTPUT_STRIP_TRAILING_WHITESPACE)
    list(APPEND extra_args -DCMAKE_OSX_ARCHITECTURES=${host_arch})
endif()
if(NINJA)
    list(APPEND extra_args -DCMAKE_MAKE_PROGRAM=${NINJA})
endif()
set(off_options
    DAWN_FETCH_DEPENDENCIES DAWN_BUILD_SAMPLES DAWN_BUILD_TESTS DAWN_BUILD_BENCHMARKS DAWN_BUILD_FUZZERS
    DAWN_BUILD_NODE_BINDINGS DAWN_BUILD_PROTOBUF DAWN_ENABLE_INSTALL DAWN_ENABLE_D3D11 DAWN_ENABLE_D3D12
    DAWN_ENABLE_METAL DAWN_ENABLE_VULKAN DAWN_ENABLE_DESKTOP_GL DAWN_ENABLE_OPENGLES DAWN_ENABLE_NULL
    DAWN_ENABLE_SWIFTSHADER DAWN_ENABLE_SPIRV_VALIDATION DAWN_USE_GLFW DAWN_USE_WAYLAND DAWN_USE_X11
    DAWN_USE_BUILT_DXC TINT_BUILD_TESTS TINT_BUILD_BENCHMARKS TINT_BUILD_FUZZERS TINT_BUILD_GLSL_WRITER
    TINT_BUILD_GLSL_VALIDATOR TINT_BUILD_HLSL_WRITER TINT_BUILD_MSL_WRITER TINT_BUILD_SPV_WRITER
    TINT_BUILD_NULL_WRITER TINT_BUILD_IR_BINARY TINT_ENABLE_INSTALL)
set(on_options TINT_BUILD_CMD_TOOLS TINT_BUILD_SPV_READER TINT_BUILD_WGSL_READER TINT_BUILD_WGSL_WRITER)
foreach(option IN LISTS off_options)
    list(APPEND extra_args -D${option}=OFF)
endforeach()
foreach(option IN LISTS on_options)
    list(APPEND extra_args -D${option}=ON)
endforeach()
message(STATUS "Tint: configuring")
execute_process(
    COMMAND ${CMAKE_COMMAND} -G Ninja -S ${src}/root -B ${build} -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF ${extra_args}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Tint: configure failed (${result}); scratch directory kept: ${WORK_DIR}\n${output}")
endif()
message(STATUS "Tint: building (${JOBS} jobs; about a minute once per machine)")
execute_process(COMMAND ${CMAKE_COMMAND} --build ${build} --target tint_cmd_tint_cmd -j ${JOBS}
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
if(NOT result EQUAL 0)
    string(LENGTH "${output}" length)
    math(EXPR start "${length} - 4000")
    if(start LESS 0)
        set(start 0)
    endif()
    string(SUBSTRING "${output}" ${start} -1 tail)
    message(FATAL_ERROR "Tint: build failed (${result}); scratch directory kept: ${WORK_DIR}\n${tail}")
endif()

set(stage ${TINT_DIR}.partial)
file(REMOVE_RECURSE ${stage})
file(MAKE_DIRECTORY ${stage}/bin)
file(COPY ${build}/tint${CMAKE_HOST_EXECUTABLE_SUFFIX} DESTINATION ${stage}/bin)
file(COPY ${src}/root/LICENSE DESTINATION ${stage})
# `tint --help` exits with 1.
execute_process(COMMAND ${stage}/bin/tint --help RESULT_VARIABLE result OUTPUT_VARIABLE help ERROR_VARIABLE help)
if(NOT help MATCHES "Usage: tint")
    message(FATAL_ERROR "Tint: the built tint does not run (${result}): ${help}")
endif()
file(REMOVE_RECURSE ${TINT_DIR})
file(RENAME ${stage} ${TINT_DIR})
file(REMOVE_RECURSE ${WORK_DIR})
message(STATUS "Tint: built into ${TINT_DIR}")
