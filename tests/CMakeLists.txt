add_library(quench_test_harness STATIC harness/TestMain.cpp)
quench_configure_target(quench_test_harness)
target_include_directories(quench_test_harness PUBLIC harness)

# quench_add_test_executable(<name> PREFIX <ctest prefix> SOURCES <file>... LIBS <target>... [ENVIRONMENT <NAME=value>...])
function(quench_add_test_executable name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "PREFIX" "SOURCES;LIBS;ENVIRONMENT")
    add_executable(${name} ${arg_SOURCES})
    quench_configure_target(${name})
    target_link_libraries(${name} PRIVATE quench_test_harness ${arg_LIBS})
    quench_add_test_variant(${name} PREFIX ${arg_PREFIX} ENVIRONMENT ${arg_ENVIRONMENT})
endfunction()

# Registers every case of a test executable under PREFIX, run with the given environment. A second call with another
# prefix runs the same cases again (another GPU driver, for example). A case whose output matches FAIL_REGEX fails.
# quench_add_test_variant(<name> PREFIX <ctest prefix> [ENVIRONMENT <NAME=value>...] [FAIL_REGEX <regex>])
function(quench_add_test_variant name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "PREFIX;FAIL_REGEX" "ENVIRONMENT")
    if(CMAKE_CROSSCOMPILING_EMULATOR MATCHES "simctl" AND arg_ENVIRONMENT)
        # simctl passes only SIMCTL_CHILD_ variables to the process, without the prefix.
        list(TRANSFORM arg_ENVIRONMENT PREPEND SIMCTL_CHILD_)
    endif()
    list(JOIN arg_ENVIRONMENT "|" environment)
    set(list_file ${CMAKE_CURRENT_BINARY_DIR}/${arg_PREFIX}_cases.cmake)
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:${name}> -DPREFIX=${arg_PREFIX} -DOUTPUT=${list_file}
                -DWORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR} -DSKIP_RETURN_CODE=77
                "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}" "-DENVIRONMENT=${environment}"
                "-DFAIL_REGEX=${arg_FAIL_REGEX}" -P ${CMAKE_CURRENT_SOURCE_DIR}/DiscoverTests.cmake
        VERBATIM)
    set(include_file ${CMAKE_CURRENT_BINARY_DIR}/${arg_PREFIX}_include.cmake)
    file(CONFIGURE OUTPUT ${include_file} CONTENT
"if(EXISTS [=[${list_file}]=])
    include([=[${list_file}]=])
else()
    add_test([=[${arg_PREFIX}_NOT_BUILT]=] [=[${arg_PREFIX}_NOT_BUILT]=])
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
    LIBS quench_gpu_shaders)
quench_add_shaders(quench_gpu_tests NAME test SOURCE gpu/shaders/test.hlsl
    ENTRIES test_vs:vertex test_fs:fragment
    INCLUDE_DIRS gpu/shaders/include)
target_compile_definitions(quench_gpu_tests PRIVATE
    QUENCH_TEST_METAL=$<BOOL:${APPLE}>
    QUENCH_TEST_METALLIB=$<BOOL:${QUENCH_SHADER_METAL_OK}>
    QUENCH_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/quench_shaders/test")

if(NOT QUENCH_HEADLESS)
    quench_add_test_executable(quench_app_tests PREFIX app
        SOURCES
            app/SdlFileSystemTests.cpp
            app/SdlLogTests.cpp
        LIBS quench_app)
    target_compile_definitions(quench_app_tests PRIVATE QUENCH_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

    # Device tests on the driver SDL picks: Metal on Apple platforms, with the Metal API validation layer.
    set(gpu_environment "")
    if(APPLE)
        set(gpu_environment MTL_DEBUG_LAYER=1 QUENCH_TEST_GPU_DRIVER=metal)
    endif()
    quench_add_test_executable(quench_gpu_device_tests PREFIX gpu_device
        SOURCES
            gpu/DeviceTests.cpp
            gpu/GpuTest.cpp
            gpu/RenderTests.cpp
            gpu/WindowTests.cpp
        LIBS quench_gpu SDL3::SDL3-static
        ENVIRONMENT ${gpu_environment})
    quench_add_shaders(quench_gpu_device_tests NAME render SOURCE gpu/shaders/render.hlsl
        ENTRIES color_vs:vertex color_fs:fragment fullscreen_vs:vertex uniform_fs:fragment texture_fs:fragment
                rects_vs:vertex)
    target_compile_definitions(quench_gpu_device_tests PRIVATE
        QUENCH_TEST_METALLIB=$<BOOL:${QUENCH_SHADER_METAL_OK}>)

    # The same cases on Vulkan through MoltenVK (macOS with Homebrew's vulkan-loader and molten-vk): checks the SPIR-V
    # path, not a real Vulkan driver. SDL looks the loader up by bare names, which dlopen does not search for in
    # Homebrew's lib directory.
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        find_library(QUENCH_TEST_VULKAN_LOADER NAMES vulkan.1 PATHS /opt/homebrew/lib /usr/local/lib NO_DEFAULT_PATH)
        find_library(QUENCH_TEST_VULKAN_VALIDATION NAMES VkLayer_khronos_validation
            PATHS /opt/homebrew/lib /usr/local/lib NO_DEFAULT_PATH)
        if(QUENCH_TEST_VULKAN_LOADER)
            set(vulkan_environment SDL_GPU_DRIVER=vulkan SDL_VULKAN_LIBRARY=${QUENCH_TEST_VULKAN_LOADER}
                                   QUENCH_TEST_GPU_DRIVER=vulkan)
            if(QUENCH_TEST_VULKAN_VALIDATION)
                # SDL's debug mode enables the Khronos validation layer when its manifest is installed
                # (vulkan-validationlayers); the manifest names the library without a path, and device creation fails
                # if dlopen cannot find it.
                cmake_path(GET QUENCH_TEST_VULKAN_VALIDATION PARENT_PATH layer_dir)
                list(APPEND vulkan_environment DYLD_FALLBACK_LIBRARY_PATH=${layer_dir}:/usr/local/lib:/usr/lib)
            endif()
            # Without a debug messenger (SDL registers none) the layer prints its messages to stdout.
            quench_add_test_variant(quench_gpu_device_tests PREFIX gpu_vulkan
                ENVIRONMENT ${vulkan_environment}
                FAIL_REGEX "Validation (Error|Warning)")
        endif()
    endif()
endif()

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
target_link_libraries(quench_header_check PRIVATE quench_core quench_physics quench_gpu_shaders)
if(NOT QUENCH_HEADLESS)
    target_link_libraries(quench_header_check PRIVATE quench_app)
endif()
