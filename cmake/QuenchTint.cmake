# Tint for WGSL (web builds): the browser's WebGPU takes WGSL only, and Tint, the shader compiler of Dawn
# (Chrome's WebGPU), translates the SPIR-V that quench_shaderc writes for it (--wgsl-spirv) into WGSL at build time.
#
# The Tint is Dawn's commit 66f8992 (2026-11; BSD-3-Clause), a host tool like DXC:
#   - QUENCH_TINT: a tint executable to use instead (it needs the SPIR-V reader, which Dawn's release binaries lack);
#   - otherwise built from its pinned source archives by cmake/BuildTint.cmake at the first configure that needs it,
#     into the per-machine tool cache (cmake/QuenchToolCache.cmake). About a minute on an M4 Pro, 9 MB kept.
# Without a Tint a web build has no shaders: the configure fails.

include(${CMAKE_CURRENT_LIST_DIR}/QuenchToolCache.cmake)

set(QUENCH_TINT "" CACHE FILEPATH "tint executable with the SPIR-V reader for WGSL shaders (empty: the pinned Tint)")

set(_quench_tint_name tint-66f8992)
set(_quench_tint_script ${CMAKE_CURRENT_LIST_DIR}/BuildTint.cmake)

# quench_tint(<out>): the tint for WGSL shaders. Builds the pinned Tint on the first call of a machine.
function(quench_tint out)
    get_property(resolved GLOBAL PROPERTY QUENCH_TINT_RESOLVED SET)
    if(resolved)
        get_property(tint GLOBAL PROPERTY QUENCH_TINT_RESOLVED)
        set(${out} "${tint}" PARENT_SCOPE)
        return()
    endif()
    set(tint "")
    if(QUENCH_TINT)
        if(NOT EXISTS ${QUENCH_TINT})
            message(FATAL_ERROR "Quench: QUENCH_TINT=${QUENCH_TINT} does not exist")
        endif()
        set(tint ${QUENCH_TINT})
    else()
        _quench_tool_cache(cache)
        set(dir ${cache}/${_quench_tint_name})
        set(exe ${dir}/bin/tint${CMAKE_HOST_EXECUTABLE_SUFFIX})
        if(NOT EXISTS ${exe})
            file(MAKE_DIRECTORY ${cache})
            # One build per machine even when several configures run at once; the others wait for it.
            file(LOCK ${cache}/${_quench_tint_name}.lock GUARD FUNCTION TIMEOUT 7200 RESULT_VARIABLE locked)
            if(NOT EXISTS ${exe})
                if(NOT locked STREQUAL "0")
                    message(FATAL_ERROR "Quench: cannot lock ${cache} (${locked}): no Tint for WGSL shaders")
                endif()
                message(STATUS "Quench: building Tint (Dawn 66f8992) from source into ${dir} "
                               "(once per machine, about a minute on an M4 Pro)")
                set(make_program "")
                if(CMAKE_GENERATOR STREQUAL "Ninja" AND CMAKE_MAKE_PROGRAM)
                    set(make_program -DNINJA=${CMAKE_MAKE_PROGRAM})
                endif()
                execute_process(
                    COMMAND ${CMAKE_COMMAND} -DTINT_DIR=${dir} -DWORK_DIR=${dir}.work ${make_program}
                            -P ${_quench_tint_script}
                    RESULT_VARIABLE result)
                if(NOT result EQUAL 0)
                    message(FATAL_ERROR "Quench: the Tint build failed (above): no WGSL shaders. QUENCH_TINT can name "
                                        "a tint with the SPIR-V reader instead.")
                endif()
            endif()
            file(LOCK ${cache}/${_quench_tint_name}.lock RELEASE)
        endif()
        set(tint ${exe})
    endif()
    message(STATUS "Quench: Tint for WGSL shaders: ${tint}")
    set_property(GLOBAL PROPERTY QUENCH_TINT_RESOLVED "${tint}")
    set(${out} "${tint}" PARENT_SCOPE)
endfunction()
