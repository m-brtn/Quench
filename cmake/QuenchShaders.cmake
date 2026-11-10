# Build-time shader compilation for the engine and for games: quench_add_shaders().
#
# Every entry point of an HLSL source is compiled by the host tool `quench_shaderc` (tools/shaderc):
#   - SPIR-V (Vulkan), on every platform;
#   - Apple platforms: SPIR-V -> MSL (SPIRV-Cross, SDL GPU resource slots) -> xcrun metal -> one .metallib per entry
#     point. Without the Metal toolchain the MSL source is embedded instead and SDL compiles it at startup;
#   - Windows hosts: DXIL with the pinned DXC release;
#   - web builds (Emscripten with the GPU layer): WGSL only, for the WebGPU backend (src/gpu/webgpu). quench_shaderc
#     writes the SPIR-V again with that backend's bindings and Tint translates it (cmake/QuenchTint.cmake); ray
#     query entries are left out (WebGPU has no ray tracing).
# Cross builds (iOS, the web) cannot run a compiler built for the target: they take a macOS `quench_shaderc` through
# QUENCH_HOST_SHADERC (the `host-tools` preset builds it). All blobs are embedded into the binary; no shader files are
# read at run time, except by shader hot reload (gpu::Device::enableShaderHotReload): host builds with a
# single-configuration generator write a manifest per set (<set>/manifest.json: the source, the build's depfiles and
# the compile commands with {source} and {output} for the paths), and each blob names it relative to the build
# directory.

set(QUENCH_HOST_SHADERC "" CACHE FILEPATH "quench_shaderc built for the host (required for cross builds)")
if(NOT QUENCH_HOST_SHADERC AND NOT CMAKE_CROSSCOMPILING)
    include(${CMAKE_CURRENT_LIST_DIR}/ShaderToolchain.cmake)
endif()
set(QUENCH_SHADER_DXC "${QUENCH_DXC}" CACHE INTERNAL "")
include(${CMAKE_CURRENT_LIST_DIR}/QuenchDxc.cmake)
set(_wgsl OFF)
if(EMSCRIPTEN AND NOT QUENCH_HEADLESS)
    set(_wgsl ON)
    include(${CMAKE_CURRENT_LIST_DIR}/QuenchTint.cmake)
endif()
set(QUENCH_SHADER_WGSL ${_wgsl} CACHE INTERNAL "")

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
if(APPLE)
    find_program(QUENCH_SHADER_XCRUN xcrun)
    mark_as_advanced(QUENCH_SHADER_XCRUN)
endif()
set(QUENCH_SHADER_METAL_OK ${_metal_ok} CACHE INTERNAL "")
set(QUENCH_SHADER_METAL_SDK ${_metal_sdk} CACHE INTERNAL "")
set(QUENCH_SHADER_METAL_TARGET ${_metal_target} CACHE INTERNAL "")
set(QUENCH_SHADER_MSL_PLATFORM ${_msl_platform} CACHE INTERNAL "")

# A JSON array of strings from a CMake list (the manifests).
function(_quench_json_array out)
    set(items "")
    foreach(item IN LISTS ARGN)
        string(REPLACE "\\" "\\\\" item "${item}")
        string(REPLACE "\"" "\\\"" item "${item}")
        list(APPEND items "\"${item}\"")
    endforeach()
    list(JOIN items ", " joined)
    set(${out} "[${joined}]" PARENT_SCOPE)
endfunction()

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
    # Shader hot reload runs the same commands with the compiler named here; a multi-configuration generator has one
    # per configuration (not supported), a cross build none that runs on the target.
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    set(manifest_entries "")
    set(write_manifest OFF)
    if(NOT CMAKE_CROSSCOMPILING AND NOT multi_config)
        set(write_manifest ON)
        if(QUENCH_HOST_SHADERC)
            set(manifest_shaderc ${QUENCH_HOST_SHADERC})
        else()
            set(manifest_shaderc $<TARGET_FILE:quench_shaderc>)
        endif()
    endif()

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
    if(QUENCH_SHADER_WGSL)
        set(formats wgsl)
        quench_tint(tint)
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
            if(QUENCH_SHADER_WGSL)
                continue()
            endif()
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
        set(wgsl_args "")
        if(QUENCH_SHADER_WGSL)
            list(APPEND outs ${gen_dir}/${entry}.wgsl.spv)
            set(wgsl_args --wgsl-spirv ${gen_dir}/${entry}.wgsl.spv)
        endif()
        add_custom_command(
            OUTPUT ${outs}
            COMMAND ${shaderc} --input ${source} --entry ${entry} --stage ${stage} --spirv ${gen_dir}/${entry}.spv
                    --reflect ${gen_dir}/${entry}.reflect ${msl_args} ${include_args} --depfile ${gen_dir}/${entry}.d
                    ${dxc_args} ${wgsl_args}
            DEPENDS ${source} ${shaderc} ${dxc_depends}
            DEPFILE ${gen_dir}/${entry}.d
            COMMENT "Compiling ${source_name}:${entry} (${stage})"
            VERBATIM)
        list(APPEND blobs ${outs})

        if(QUENCH_SHADER_WGSL)
            # HLSL allows derivatives in non-uniform control flow; WGSL only with the diagnostic this flag adds.
            add_custom_command(
                OUTPUT ${gen_dir}/${entry}.wgsl
                COMMAND ${tint} --input-format spirv --format wgsl --allow-non-uniform-derivatives
                        -o ${gen_dir}/${entry}.wgsl ${gen_dir}/${entry}.wgsl.spv
                DEPENDS ${gen_dir}/${entry}.wgsl.spv ${tint}
                COMMENT "Translating ${entry} -> WGSL"
                VERBATIM)
            list(APPEND blobs ${gen_dir}/${entry}.wgsl)
        endif()

        if(write_manifest)
            set(reload_msl "")
            if(APPLE)
                set(reload_msl --msl {output}/${entry}.msl --msl-platform ${QUENCH_SHADER_MSL_PLATFORM})
            endif()
            _quench_json_array(compile ${manifest_shaderc} --input {source} --entry ${entry} --stage ${stage}
                --spirv {output}/${entry}.spv --reflect {output}/${entry}.reflect ${reload_msl} ${include_args}
                --depfile {output}/${entry}.d ${dxc_args})
            set(json_entry "    {\"entry\": \"${entry}\", \"depfile\": \"${gen_dir}/${entry}.d\",\n")
            string(APPEND json_entry "     \"compile\": [${compile}]")
            if(APPLE AND QUENCH_SHADER_METAL_OK AND QUENCH_SHADER_XCRUN)
                _quench_json_array(air ${QUENCH_SHADER_XCRUN} -sdk ${QUENCH_SHADER_METAL_SDK} metal -x metal -c
                    {output}/${entry}.msl -o {output}/${entry}.air ${QUENCH_SHADER_METAL_TARGET})
                _quench_json_array(lib ${QUENCH_SHADER_XCRUN} -sdk ${QUENCH_SHADER_METAL_SDK} metallib
                    {output}/${entry}.air -o {output}/${entry}.metallib)
                string(APPEND json_entry ",\n     \"metallib\": [${air}, ${lib}]")
            endif()
        endif()

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
            if(write_manifest)
                _quench_json_array(dxil ${QUENCH_SHADER_DXC} -nologo -T ${profile} -E ${entry}
                    -D QUENCH_ENTRY_${entry}=1 -D ${stage_define} -HV 2021 -O3 ${dxc_include_args}
                    -Fo {output}/${entry}.dxil {source})
                string(APPEND json_entry ",\n     \"dxil\": [${dxil}]")
            endif()
        endif()
        if(write_manifest)
            list(APPEND manifest_entries "${json_entry}}")
        endif()
    endforeach()

    set(manifest "")
    if(write_manifest)
        list(JOIN manifest_entries ",\n" entries_json)
        file(GENERATE OUTPUT ${gen_dir}/manifest.json CONTENT
"{\"set\": \"${arg_NAME}\", \"source\": \"${source}\", \"output\": \"${gen_dir}/reload\",
 \"entries\": [
${entries_json}
 ]}
")
        file(RELATIVE_PATH manifest ${CMAKE_BINARY_DIR} ${gen_dir}/manifest.json)
    endif()

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
                -DMANIFEST=${manifest} -P ${embed_script}
        DEPENDS ${blobs} ${embed_script}
        COMMENT "Embedding shaders '${arg_NAME}' (${formats_csv})"
        VERBATIM)
    target_sources(${target} PRIVATE ${gen_dir}/${arg_NAME}_shaders.cpp ${header})
    target_include_directories(${target} PRIVATE ${gen_dir})
    target_link_libraries(${target} PRIVATE quench_gpu_shaders)
endfunction()

# quench_add_ray_hits(<target> NAME <name> SURFACES <surface.hlsl>... [INCLUDE_DIRS <dir>...])
#
# The engine's tracing entries (traced reflections, indirect light, the probe rays and the path tracer, from
# src/render/shaders/raylight.hlsl) compiled again with the game's surface shaders in their hits, as the table
# quench::shaders::<name> in "<name>_shaders.h". A material whose surface passes this table and its number as
# SurfaceDesc::rayHitShaders and rayHit (the surfaces are numbered from 1 in the order of SURFACES) is shaded in the
# hits by that surface's function (QUENCH_SURFACE_HIT is 1 there; docs/render.md, "Surface shaders"). The surfaces
# are included into one source, each in a namespace of its own: their functions and globals may share names, their
# macros may not. Without DXC the table is empty and hits use the standard surface.
function(quench_add_ray_hits target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "NAME" "SURFACES;INCLUDE_DIRS")
    if(NOT arg_NAME OR NOT arg_SURFACES)
        message(FATAL_ERROR "quench_add_ray_hits: NAME and SURFACES are required")
    endif()
    set(gen_dir ${CMAKE_CURRENT_BINARY_DIR}/quench_shaders/${arg_NAME}_source)
    get_filename_component(raylight ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/render/shaders/raylight.hlsl ABSOLUTE)
    set(surfaces "")
    set(cases "")
    set(index 0)
    foreach(file IN LISTS arg_SURFACES)
        math(EXPR index "${index} + 1")
        get_filename_component(path ${file} ABSOLUTE)
        if(NOT EXISTS ${path})
            message(FATAL_ERROR "quench_add_ray_hits: ${path} does not exist")
        endif()
        string(APPEND surfaces "namespace quench_ray_surface_${index} {\n#include \"${path}\"\n"
            "#ifndef QUENCH_SURFACE\n#define QUENCH_SURFACE quenchStandardSurface\n#endif\n"
            "QuenchSurface quenchRaySurface(QuenchSurfaceInput i) { return QUENCH_SURFACE(i); }\n"
            "} // namespace quench_ray_surface_${index}\n#undef QUENCH_SURFACE\n#undef QUENCH_SURFACE_VERTEX\n")
        string(APPEND cases "    if (n == ${index}u) return quench_ray_surface_${index}::quenchRaySurface(i);\n")
    endforeach()
    file(CONFIGURE OUTPUT ${gen_dir}/QuenchRaySurfaces.hlsli @ONLY CONTENT
"// Generated by quench_add_ray_hits() (cmake/QuenchShaders.cmake). Do not edit.
// The surfaces whose functions shade ray hits (src/render/shaders/RayHit.hlsli). Hits have no pixel footprint.
#define ddx(v) ((v) * 0.0)
#define ddy(v) ((v) * 0.0)
#define ddx_fine(v) ((v) * 0.0)
#define ddy_fine(v) ((v) * 0.0)
#define ddx_coarse(v) ((v) * 0.0)
#define ddy_coarse(v) ((v) * 0.0)
#define fwidth(v) ((v) * 0.0)
${surfaces}#undef ddx
#undef ddy
#undef ddx_fine
#undef ddy_fine
#undef ddx_coarse
#undef ddy_coarse
#undef fwidth

QuenchSurface quenchRaySurface(uint n, QuenchSurfaceInput i) {
${cases}    return quenchStandardSurface(i);
}
")
    file(CONFIGURE OUTPUT ${gen_dir}/${arg_NAME}.hlsl @ONLY CONTENT
"// Generated by quench_add_ray_hits() (cmake/QuenchShaders.cmake). Do not edit.
#define QUENCH_RT_SURFACES 1
#include \"${raylight}\"
")
    quench_add_shaders(${target} NAME ${arg_NAME} SOURCE ${gen_dir}/${arg_NAME}.hlsl
        ENTRIES rt_reflect_fs:fragment:rayquery rt_indirect_fs:fragment:rayquery rt_probe_trace_fs:fragment:rayquery
                pt_fs:fragment:rayquery rt_reflect_maps_fs:fragment:rayquery rt_indirect_maps_fs:fragment:rayquery
                rt_probe_trace_maps_fs:fragment:rayquery pt_maps_fs:fragment:rayquery
        INCLUDE_DIRS ${gen_dir} ${arg_INCLUDE_DIRS})
endfunction()
