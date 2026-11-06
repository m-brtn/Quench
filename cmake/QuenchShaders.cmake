# Build-time shader compilation for the engine and for games: quench_add_shaders().
#
# Every entry point of an HLSL source is compiled by the host tool `quench_shaderc` (tools/shaderc):
#   - SPIR-V (Vulkan), on every platform;
#   - Apple platforms: SPIR-V -> MSL (SPIRV-Cross, SDL GPU resource slots) -> xcrun metal -> one .metallib per entry
#     point. Without the Metal toolchain the MSL source is embedded instead and SDL compiles it at startup;
#   - Windows hosts: DXIL with the pinned DXC release.
# Cross builds (iOS) cannot run a compiler built for the target: they take a macOS `quench_shaderc` through
# QUENCH_HOST_SHADERC (the `host-tools` preset builds it). All blobs are embedded into the binary; no shader files are
# read at run time.

set(QUENCH_HOST_SHADERC "" CACHE FILEPATH "quench_shaderc built for the host (required for cross builds)")
if(NOT QUENCH_HOST_SHADERC AND NOT CMAKE_CROSSCOMPILING)
    include(${CMAKE_CURRENT_LIST_DIR}/ShaderToolchain.cmake)
endif()
set(QUENCH_SHADER_DXC "${QUENCH_DXC}" CACHE INTERNAL "")
include(${CMAKE_CURRENT_LIST_DIR}/QuenchDxc.cmake)

set(_metal_ok OFF)
set(_metal_sdk macosx)
set(_metal_target_flag -mmacosx-version-min)
set(_msl_platform macos)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(_msl_platform ios)
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
        set(_metal_sdk iphonesimulator)
        set(_metal_target_flag -mios-simulator-version-min)
    else()
        set(_metal_sdk iphoneos)
        set(_metal_target_flag -mios-version-min)
    endif()
endif()
# Without a deployment target the metallibs target the SDK's default, like the rest of the build.
set(_metal_target "")
if(CMAKE_OSX_DEPLOYMENT_TARGET)
    set(_metal_target ${_metal_target_flag}=${CMAKE_OSX_DEPLOYMENT_TARGET})
endif()
if(APPLE)
    execute_process(COMMAND xcrun -sdk ${_metal_sdk} -f metal
        OUTPUT_VARIABLE _metal_path OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _metal_res ERROR_QUIET)
    if(_metal_res EQUAL 0 AND EXISTS "${_metal_path}")
        set(_metal_ok ON)
        message(STATUS "Quench: Metal compiler: ${_metal_path} - MSL is precompiled into metallibs")
    else()
        message(WARNING "Quench: xcrun metal not found - the MSL source is embedded (compiled at startup). "
                        "To install the toolchain: xcodebuild -downloadComponent MetalToolchain")
    endif()
endif()
set(QUENCH_SHADER_METAL_OK ${_metal_ok} CACHE INTERNAL "")
set(QUENCH_SHADER_METAL_SDK ${_metal_sdk} CACHE INTERNAL "")
set(QUENCH_SHADER_METAL_TARGET ${_metal_target} CACHE INTERNAL "")
set(QUENCH_SHADER_MSL_PLATFORM ${_msl_platform} CACHE INTERNAL "")

# quench_add_shaders(<target> NAME <name> SOURCE <file.hlsl> ENTRIES <entry>:<vertex|fragment|compute>[:rayquery]...
#                    [INCLUDE_DIRS <dir>...])
#
# Compiles each entry point with QUENCH_ENTRY_<entry> and QUENCH_STAGE_VERTEX, QUENCH_STAGE_FRAGMENT or
# QUENCH_STAGE_COMPUTE defined and embeds the blobs into <target>. The generated header "<name>_shaders.h" declares the table as
# quench::shaders::<name> (include/quench/gpu/ShaderBlob.h). #include resolves relative to the including file, then in
# INCLUDE_DIRS, then in the engine shader library (shaders/); included files are tracked as dependencies.
# Entries marked `rayquery` use ray queries: they are compiled with DXC (cmake/QuenchDxc.cmake; shader model 6.5,
# SPIR-V for Vulkan 1.2) and left out of the table when no DXC is available (logged at configure).
function(quench_add_shaders target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "NAME;SOURCE" "ENTRIES;INCLUDE_DIRS")
    if(NOT arg_NAME OR NOT arg_SOURCE OR NOT arg_ENTRIES)
        message(FATAL_ERROR "quench_add_shaders: NAME, SOURCE and ENTRIES are required")
    endif()
    if(NOT arg_NAME MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "quench_add_shaders: NAME '${arg_NAME}' is not a C++ identifier")
    endif()
    if(QUENCH_HOST_SHADERC)
        if(NOT EXISTS ${QUENCH_HOST_SHADERC})
            message(FATAL_ERROR "Quench: QUENCH_HOST_SHADERC=${QUENCH_HOST_SHADERC} does not exist. Build it first: "
                                "cmake --preset host-tools && cmake --build --preset host-tools")
        endif()
        set(shaderc ${QUENCH_HOST_SHADERC})
    elseif(TARGET quench_shaderc)
        set(shaderc quench_shaderc)
    else()
        message(FATAL_ERROR "Quench: cross builds need QUENCH_HOST_SHADERC (cmake --preset host-tools && "
                            "cmake --build --preset host-tools)")
    endif()

    get_filename_component(source ${arg_SOURCE} ABSOLUTE)
    get_filename_component(source_name ${source} NAME)
    set(gen_dir ${CMAKE_CURRENT_BINARY_DIR}/quench_shaders/${arg_NAME})
    file(MAKE_DIRECTORY ${gen_dir})

    set(include_args "")
    set(dxc_include_args "")
    foreach(dir IN LISTS arg_INCLUDE_DIRS)
        get_filename_component(abs ${dir} ABSOLUTE)
        list(APPEND include_args --include-dir ${abs})
        list(APPEND dxc_include_args -I ${abs})
    endforeach()
    get_filename_component(library ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../shaders ABSOLUTE)
    list(APPEND include_args --include-dir ${library})
    list(APPEND dxc_include_args -I ${library})

    set(formats spirv)
    if(APPLE)
        if(QUENCH_SHADER_METAL_OK)
            list(APPEND formats metallib)
        else()
            list(APPEND formats msl)
        endif()
    endif()
    if(QUENCH_SHADER_DXC)
        list(APPEND formats dxil)
    endif()

    set(blobs "")
    set(entries "")
    foreach(item IN LISTS arg_ENTRIES)
        string(REPLACE ":" ";" pair ${item})
        list(LENGTH pair pair_length)
        if(NOT pair_length EQUAL 2 AND NOT pair_length EQUAL 3)
            message(FATAL_ERROR "quench_add_shaders: '${item}' is not <entry>:<stage>[:rayquery]")
        endif()
        list(GET pair 0 entry)
        list(GET pair 1 stage)
        set(ray_query OFF)
        if(pair_length EQUAL 3)
            list(GET pair 2 flag)
            if(NOT flag STREQUAL "rayquery")
                message(FATAL_ERROR "quench_add_shaders: '${item}': the only entry flag is 'rayquery'")
            endif()
            set(ray_query ON)
        endif()
        if(NOT entry MATCHES "^[A-Za-z_][A-Za-z0-9_]*$" OR NOT stage MATCHES "^(vertex|fragment|compute)$")
            message(FATAL_ERROR "quench_add_shaders: '${item}' is not <identifier>:<vertex|fragment|compute>")
        endif()
        set(dxc_args "")
        set(dxc_depends "")
        if(ray_query)
            quench_ray_query_dxc(ray_query_dxc)
            if(NOT ray_query_dxc)
                continue()
            endif()
            set(dxc_args --dxc ${ray_query_dxc})
            set(dxc_depends ${ray_query_dxc})
        endif()
        list(APPEND entries ${entry})

        set(outs ${gen_dir}/${entry}.spv ${gen_dir}/${entry}.reflect)
        set(msl_args "")
        if(APPLE)
            list(APPEND outs ${gen_dir}/${entry}.msl)
            set(msl_args --msl ${gen_dir}/${entry}.msl --msl-platform ${QUENCH_SHADER_MSL_PLATFORM})
        endif()
        add_custom_command(
            OUTPUT ${outs}
            COMMAND ${shaderc} --input ${source} --entry ${entry} --stage ${stage} --spirv ${gen_dir}/${entry}.spv
                    --reflect ${gen_dir}/${entry}.reflect ${msl_args} ${include_args} --depfile ${gen_dir}/${entry}.d
                    ${dxc_args}
            DEPENDS ${source} ${shaderc} ${dxc_depends}
            DEPFILE ${gen_dir}/${entry}.d
            COMMENT "Compiling ${source_name}:${entry} (${stage})"
            VERBATIM)
        list(APPEND blobs ${outs})

        if(APPLE AND QUENCH_SHADER_METAL_OK)
            add_custom_command(
                OUTPUT ${gen_dir}/${entry}.metallib
                COMMAND xcrun -sdk ${QUENCH_SHADER_METAL_SDK} metal -x metal -c ${gen_dir}/${entry}.msl
                        -o ${gen_dir}/${entry}.air ${QUENCH_SHADER_METAL_TARGET}
                COMMAND xcrun -sdk ${QUENCH_SHADER_METAL_SDK} metallib ${gen_dir}/${entry}.air
                        -o ${gen_dir}/${entry}.metallib
                DEPENDS ${gen_dir}/${entry}.msl
                COMMENT "Compiling ${entry}.msl -> ${entry}.metallib"
                VERBATIM)
            list(APPEND blobs ${gen_dir}/${entry}.metallib)
        endif()

        if(QUENCH_SHADER_DXC)
            # Shader model 6.5 for ray queries (DXR 1.1).
            set(model 6_0)
            if(ray_query)
                set(model 6_5)
            endif()
            if(stage STREQUAL "vertex")
                set(profile vs_${model})
                set(stage_define QUENCH_STAGE_VERTEX=1)
            elseif(stage STREQUAL "fragment")
                set(profile ps_${model})
                set(stage_define QUENCH_STAGE_FRAGMENT=1)
            else()
                set(profile cs_${model})
                set(stage_define QUENCH_STAGE_COMPUTE=1)
            endif()
            # DXC writes no depfile; depending on the SPIR-V output (rebuilt when an included file changes) reruns it
            # for the same changes.
            add_custom_command(
                OUTPUT ${gen_dir}/${entry}.dxil
                COMMAND ${QUENCH_SHADER_DXC} -nologo -T ${profile} -E ${entry} -D QUENCH_ENTRY_${entry}=1
                        -D ${stage_define} -HV 2021 -O3 ${dxc_include_args} -Fo ${gen_dir}/${entry}.dxil ${source}
                DEPENDS ${source} ${gen_dir}/${entry}.spv
                COMMENT "Compiling ${source_name}:${entry} -> DXIL (${profile})"
                VERBATIM)
            list(APPEND blobs ${gen_dir}/${entry}.dxil)
        endif()
    endforeach()

    set(header ${gen_dir}/${arg_NAME}_shaders.h)
    file(CONFIGURE OUTPUT ${header} CONTENT
"// Generated by quench_add_shaders() (cmake/QuenchShaders.cmake). Do not edit.
#pragma once

#include <quench/gpu/ShaderBlob.h>

namespace quench::shaders {
extern const ShaderTable ${arg_NAME};
} // namespace quench::shaders
")
    string(REPLACE ";" "," entries_csv "${entries}")
    string(REPLACE ";" "," formats_csv "${formats}")
    set(embed_script ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedShaders.cmake)
    add_custom_command(
        OUTPUT ${gen_dir}/${arg_NAME}_shaders.cpp
        COMMAND ${CMAKE_COMMAND} -DNAME=${arg_NAME} -DSOURCE_NAME=${source_name} -DENTRIES=${entries_csv}
                -DFORMATS=${formats_csv} -DGEN_DIR=${gen_dir} -DOUTPUT=${gen_dir}/${arg_NAME}_shaders.cpp
                -P ${embed_script}
        DEPENDS ${blobs} ${embed_script}
        COMMENT "Embedding shaders '${arg_NAME}' (${formats_csv})"
        VERBATIM)
    target_sources(${target} PRIVATE ${gen_dir}/${arg_NAME}_shaders.cpp ${header})
    target_include_directories(${target} PRIVATE ${gen_dir})
    target_link_libraries(${target} PRIVATE quench_gpu_shaders)
endfunction()
