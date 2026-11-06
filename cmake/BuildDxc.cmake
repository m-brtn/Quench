# Builds the pinned DXC from source into a directory of its own (script mode, run by cmake/QuenchDxc.cmake):
#   cmake -DDXC_DIR=<install dir> -DWORK_DIR=<scratch dir> [-DJOBS=<n>] [-DNINJA=<ninja>] -P cmake/BuildDxc.cmake
#
# DXC publishes release binaries for Windows and Linux x86-64 only, so macOS (and other) hosts build the tagged source
# with the submodule commits that tag records. Only the `dxc` executable and its `dxcompiler` library are kept;
# the sources and the build tree are deleted afterwards. DXC_DIR appears only when the build is complete.

cmake_minimum_required(VERSION 3.28)
if(NOT DXC_DIR OR NOT WORK_DIR)
    message(FATAL_ERROR "BuildDxc.cmake: DXC_DIR and WORK_DIR are required")
endif()

# ---------------------------------------------------------------- v1.9.2609 and its submodules
set(dxc_archives
    "DirectXShaderCompiler|https://github.com/microsoft/DirectXShaderCompiler/archive/01b62ad47db7dee3dd33f90e7339cf9e860b3f93.tar.gz|c48c223df13ba425fd9beda9843754defbcd379a3811a6a0e590110be57168c7|."
    "SPIRV-Headers|https://github.com/KhronosGroup/SPIRV-Headers/archive/496543121ce6419f23d6fa5d7194ba66c36212d2.tar.gz|a9bb9c48713245eacf97cc539b6f1d45405a92d8813f8b82e635f0a085ee9898|external/SPIRV-Headers"
    "SPIRV-Tools|https://github.com/KhronosGroup/SPIRV-Tools/archive/ef96ed763b43b59b33b31b362f09a02b729fa1c9.tar.gz|82c62146083fd558735a3171cf97cfc47903ca7d368482e87f94bd44883c0f00|external/SPIRV-Tools"
    "DirectX-Headers|https://github.com/microsoft/DirectX-Headers/archive/980971e835876dc0cde415e8f9bc646e64667bf7.tar.gz|b5a4b6d8806ff7f29f19879f83d015dbe8740676d4ca0b48647a789cc7773c4e|external/DirectX-Headers")

file(REMOVE_RECURSE ${WORK_DIR})
set(src ${WORK_DIR}/src)
set(build ${WORK_DIR}/build)
file(MAKE_DIRECTORY ${src} ${build})
foreach(item IN LISTS dxc_archives)
    string(REPLACE "|" ";" fields "${item}")
    list(GET fields 0 name)
    list(GET fields 1 url)
    list(GET fields 2 sha)
    list(GET fields 3 dest)
    set(archive ${WORK_DIR}/${name}.tar.gz)
    message(STATUS "DXC: downloading ${name}")
    file(DOWNLOAD ${url} ${archive} EXPECTED_HASH SHA256=${sha} STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "DXC: downloading ${url} failed: ${status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT ${archive} DESTINATION ${WORK_DIR}/extract-${name})
    file(GLOB top LIST_DIRECTORIES true ${WORK_DIR}/extract-${name}/*)
    if(dest STREQUAL ".")
        file(RENAME ${top} ${src}/root)
    else()
        file(REMOVE_RECURSE ${src}/root/${dest})
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
message(STATUS "DXC: configuring")
execute_process(
    COMMAND ${CMAKE_COMMAND} -G Ninja -S ${src}/root -B ${build}
            -C ${src}/root/cmake/caches/PredefinedParams.cmake
            -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_APPEND_VC_REV=OFF
            -DHLSL_INCLUDE_TESTS=OFF -DSPIRV_BUILD_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_TESTS=OFF
            -DLLVM_BUILD_TOOLS=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF ${extra_args}
    RESULT_VARIABLE result OUTPUT_QUIET)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "DXC: configure failed (${result}); scratch directory kept: ${WORK_DIR}")
endif()
message(STATUS "DXC: building (${JOBS} jobs; takes a while once per machine)")
execute_process(COMMAND ${CMAKE_COMMAND} --build ${build} --target dxc -j ${JOBS} RESULT_VARIABLE result
                OUTPUT_VARIABLE output ERROR_VARIABLE output)
if(NOT result EQUAL 0)
    string(LENGTH "${output}" length)
    math(EXPR start "${length} - 4000")
    if(start LESS 0)
        set(start 0)
    endif()
    string(SUBSTRING "${output}" ${start} -1 tail)
    message(FATAL_ERROR "DXC: build failed (${result}); scratch directory kept: ${WORK_DIR}\n${tail}")
endif()

# bin/dxc finds lib/libdxcompiler through its rpath (@executable_path/../lib, $ORIGIN/../lib).
set(stage ${DXC_DIR}.partial)
file(REMOVE_RECURSE ${stage})
file(MAKE_DIRECTORY ${stage}/bin ${stage}/lib)
file(GLOB dxc_bins ${build}/bin/dxc*)
file(GLOB dxc_libs ${build}/lib/libdxcompiler*)
file(COPY ${dxc_bins} DESTINATION ${stage}/bin FOLLOW_SYMLINK_CHAIN)
file(COPY ${dxc_libs} DESTINATION ${stage}/lib FOLLOW_SYMLINK_CHAIN)
file(COPY ${src}/root/LICENSE.TXT ${src}/root/ThirdPartyNotices.txt DESTINATION ${stage})
execute_process(COMMAND ${stage}/bin/dxc --version RESULT_VARIABLE result OUTPUT_VARIABLE version
                ERROR_VARIABLE version OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "DXC: the built dxc does not run (${result}): ${version}")
endif()
file(REMOVE_RECURSE ${DXC_DIR})
file(RENAME ${stage} ${DXC_DIR})
file(REMOVE_RECURSE ${WORK_DIR})
message(STATUS "DXC: ${version} in ${DXC_DIR}")
