cmake_minimum_required(VERSION 3.28)
# macOS deployment target when the engine is built on its own (a game sets its own before project()). 14.0 matches the
# metallib target. iOS presets set their own target (26.0).
if(NOT DEFINED PROJECT_NAME AND CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS"
   AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "Minimum macOS version" FORCE)
endif()
project(Quench VERSION 0.0.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++20 modules in the engine or its dependencies: skip the dependency scan.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(QUENCH_BUILD_TESTS "Build the engine tests" ${PROJECT_IS_TOP_LEVEL})
# Inside a game the engine is built without -Werror: a warning added by a newer compiler must not break the game.
option(QUENCH_WERROR "Treat warnings in engine targets as errors" ${PROJECT_IS_TOP_LEVEL})
set(QUENCH_ASSERTS AUTO CACHE STRING "QUENCH_ASSERT: AUTO (on unless NDEBUG), ON or OFF")
set_property(CACHE QUENCH_ASSERTS PROPERTY STRINGS AUTO ON OFF)
# Headless: only the modules without windowing, GPU or SDL (core, physics); SDL is not fetched.
option(QUENCH_HEADLESS "Build only the headless modules" OFF)
if(EMSCRIPTEN)
    # SDL GPU has no WebGPU backend in the pinned SDL.
    set(QUENCH_HEADLESS ON CACHE BOOL "" FORCE)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    # iOS test executables run only through simctl (the ios-sim-tests preset).
    set(QUENCH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
endif()

include(cmake/QuenchTargets.cmake)
include(cmake/Dependencies.cmake)
include(cmake/QuenchShaders.cmake)

# ------------------------------------------------------------------ core
# Headless: no SDL, no GPU. Result, assertions, logging, fixed-step clock, JSON, file system, thread priority.
add_library(quench_core STATIC
    src/core/Assert.cpp
    src/core/FileSystem.cpp
    src/core/Json.cpp
    src/core/Log.cpp
    src/core/Thread.cpp)
quench_configure_target(quench_core)
if(QUENCH_ASSERTS STREQUAL "ON")
    target_compile_definitions(quench_core PUBLIC QUENCH_ENABLE_ASSERTS=1)
elseif(QUENCH_ASSERTS STREQUAL "OFF")
    target_compile_definitions(quench_core PUBLIC QUENCH_ENABLE_ASSERTS=0)
endif()

# ------------------------------------------------------------------ physics
# Headless: Jolt Physics wrapper.
add_library(quench_physics STATIC src/physics/PhysicsWorld.cpp)
quench_configure_target(quench_physics)
target_link_libraries(quench_physics PUBLIC quench_core Jolt)
if(EMSCRIPTEN)
    # Single-threaded, Jolt runs its jobs nested on the stack of step(): Emscripten's 64 KB default overflows (found by
    # the wasm-node tests). 1 MB is what Jolt links its own WebAssembly programs with.
    target_link_options(quench_physics INTERFACE -sSTACK_SIZE=1048576)
endif()

# ------------------------------------------------------------------ gpu
# Headless: the table type of embedded shaders (quench_add_shaders).
add_library(quench_gpu_shaders INTERFACE)
target_include_directories(quench_gpu_shaders INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(quench_gpu_shaders INTERFACE cxx_std_20)

if(NOT QUENCH_HEADLESS)
    # The GPU layer over SDL GPU (src/gpu/sdlgpu). SDL GPU types stay out of the public headers.
    add_library(quench_gpu STATIC
        src/gpu/Format.cpp
        src/gpu/sdlgpu/SdlGpuCommands.cpp
        src/gpu/sdlgpu/SdlGpuDevice.cpp)
    if(APPLE)
        target_sources(quench_gpu PRIVATE src/gpu/sdlgpu/SdlGpuTimingMetal.m)
        target_link_libraries(quench_gpu PRIVATE "-framework Metal")
    endif()
    quench_configure_target(quench_gpu)
    target_link_libraries(quench_gpu PUBLIC quench_core quench_gpu_shaders PRIVATE SDL3::SDL3-static)
    # The shader formats every quench_add_shaders() set of this build embeds.
    target_compile_definitions(quench_gpu PRIVATE
        QUENCH_SHADERS_METALLIB=$<AND:$<BOOL:${APPLE}>,$<BOOL:${QUENCH_SHADER_METAL_OK}>>
        QUENCH_SHADERS_MSL=$<AND:$<BOOL:${APPLE}>,$<NOT:$<BOOL:${QUENCH_SHADER_METAL_OK}>>>
        QUENCH_SHADERS_DXIL=$<BOOL:${QUENCH_SHADER_DXC}>)

    # ------------------------------------------------------------------ app
    # The SDL backends of the core services (files, log). Stage 3 adds the main callbacks and the game interface.
    add_library(quench_app STATIC
        src/app/SdlFileSystem.cpp
        src/app/SdlLog.cpp)
    quench_configure_target(quench_app)
    target_link_libraries(quench_app PUBLIC quench_core SDL3::SDL3-static)
endif()

# ------------------------------------------------------------------ tests
if(QUENCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
