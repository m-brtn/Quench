# DXC for the shader entries that use ray queries (quench_add_shaders: <entry>:<stage>:rayquery). glslang, which
# compiles every other entry, cannot parse RayQuery or RaytracingAccelerationStructure (docs/architecture.md,
# "Shaders"), so quench_shaderc hands these entries to DXC for the HLSL -> SPIR-V step.
#
# The DXC is the pinned v1.9.2609 (01b62ad, the release cmake/ShaderToolchain.cmake pins for DXIL on Windows):
#   - QUENCH_RAY_QUERY_DXC: a dxc executable to use instead (for example on a machine without a compiler for DXC);
#   - Windows hosts: the prebuilt release archive (cmake/ShaderToolchain.cmake);
#   - other hosts (DXC publishes no macOS binary): built from its pinned source archives by cmake/BuildDxc.cmake at
#     the first configure that needs it, into a per-machine tool cache shared by every build tree and game
#     (QUENCH_TOOL_CACHE; default ~/Library/Caches/Quench on macOS, $XDG_CACHE_HOME/quench or ~/.cache/quench
#     elsewhere). The build takes about 2 minutes of wall time on an M4 Pro and keeps 27 MB.
# With QUENCH_RAY_QUERY_SHADERS off, or when no DXC can be had (logged), the ray query entries are left out of the
# shader tables and the renderer takes its fallbacks, as on a device without ray tracing.

option(QUENCH_RAY_QUERY_SHADERS "Compile the shader entries that use ray queries (needs DXC)" ON)
set(QUENCH_RAY_QUERY_DXC "" CACHE FILEPATH "dxc executable for ray query entries (empty: the pinned DXC)")
set(QUENCH_TOOL_CACHE "" CACHE PATH "Per-machine cache of host tools built from source (empty: the user's cache)")

set(_quench_dxc_name dxc-1.9.2609-01b62ad)
set(_quench_dxc_script ${CMAKE_CURRENT_LIST_DIR}/BuildDxc.cmake)

function(_quench_tool_cache out)
    if(QUENCH_TOOL_CACHE)
        set(${out} ${QUENCH_TOOL_CACHE} PARENT_SCOPE)
    elseif(CMAKE_HOST_APPLE)
        set(${out} $ENV{HOME}/Library/Caches/Quench PARENT_SCOPE)
    elseif(CMAKE_HOST_WIN32)
        file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Quench" cache)
        set(${out} ${cache} PARENT_SCOPE)
    elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
        set(${out} $ENV{XDG_CACHE_HOME}/quench PARENT_SCOPE)
    else()
        set(${out} $ENV{HOME}/.cache/quench PARENT_SCOPE)
    endif()
endfunction()

# quench_ray_query_dxc(<out>): the dxc for ray query entries, or "" (QUENCH_RAY_QUERY_SHADERS off, or none could be
# had: logged once). Builds the pinned DXC on the first call of a machine.
function(quench_ray_query_dxc out)
    get_property(resolved GLOBAL PROPERTY QUENCH_RAY_QUERY_DXC_RESOLVED SET)
    if(resolved)
        get_property(dxc GLOBAL PROPERTY QUENCH_RAY_QUERY_DXC_RESOLVED)
        set(${out} "${dxc}" PARENT_SCOPE)
        return()
    endif()
    set(dxc "")
    if(NOT QUENCH_RAY_QUERY_SHADERS)
        message(STATUS "Quench: ray query shaders off (QUENCH_RAY_QUERY_SHADERS): ray tracing effects fall back")
    elseif(QUENCH_RAY_QUERY_DXC)
        if(EXISTS ${QUENCH_RAY_QUERY_DXC})
            set(dxc ${QUENCH_RAY_QUERY_DXC})
        else()
            message(WARNING "Quench: QUENCH_RAY_QUERY_DXC=${QUENCH_RAY_QUERY_DXC} does not exist: no ray query shaders")
        endif()
    elseif(CMAKE_HOST_WIN32 AND QUENCH_SHADER_DXC)
        set(dxc ${QUENCH_SHADER_DXC})
    else()
        _quench_tool_cache(cache)
        set(dir ${cache}/${_quench_dxc_name})
        set(exe ${dir}/bin/dxc${CMAKE_HOST_EXECUTABLE_SUFFIX})
        if(NOT EXISTS ${exe})
            file(MAKE_DIRECTORY ${cache})
            # One build per machine even when several configures run at once; the others wait for it.
            file(LOCK ${cache}/${_quench_dxc_name}.lock GUARD FUNCTION TIMEOUT 7200 RESULT_VARIABLE locked)
            if(NOT EXISTS ${exe})
                if(NOT locked STREQUAL "0")
                    message(WARNING "Quench: cannot lock ${cache} (${locked}): no ray query shaders")
                else()
                    message(STATUS "Quench: building DXC v1.9.2609 from source into ${dir} "
                                   "(once per machine, about 2 minutes on an M4 Pro)")
                    set(make_program "")
                    if(CMAKE_GENERATOR STREQUAL "Ninja" AND CMAKE_MAKE_PROGRAM)
                        set(make_program -DNINJA=${CMAKE_MAKE_PROGRAM})
                    endif()
                    execute_process(
                        COMMAND ${CMAKE_COMMAND} -DDXC_DIR=${dir} -DWORK_DIR=${dir}.work ${make_program}
                                -P ${_quench_dxc_script}
                        RESULT_VARIABLE result)
                    if(NOT result EQUAL 0)
                        message(WARNING "Quench: the DXC build failed (above): no ray query shaders; ray tracing "
                                        "effects fall back. QUENCH_RAY_QUERY_DXC can name a dxc instead.")
                    endif()
                endif()
            endif()
            file(LOCK ${cache}/${_quench_dxc_name}.lock RELEASE)
        endif()
        if(EXISTS ${exe})
            set(dxc ${exe})
        endif()
    endif()
    if(dxc)
        message(STATUS "Quench: DXC for ray query shaders: ${dxc}")
    endif()
    set_property(GLOBAL PROPERTY QUENCH_RAY_QUERY_DXC_RESOLVED "${dxc}")
    set(${out} "${dxc}" PARENT_SCOPE)
endfunction()
