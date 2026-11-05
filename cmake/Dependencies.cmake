# Runtime dependencies, pinned to an archive of a specific commit + SHA256 (no floating branches). The FetchContent
# names and pins match the games' (cmake/Dependencies.cmake in TrailForge and Ashfall), so whichever project declares a
# dependency first, the same sources are used. GitHub serves commit archives deterministically; if a hash ever
# changes on GitHub's side, the build fails with an explicit URL_HASH error.
include(FetchContent)

# ---------------------------------------------------------------- Jolt Physics
# v5.6.0
set(DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
set(GENERATE_DEBUG_SYMBOLS ON CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED ON CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(JoltPhysics
    URL https://github.com/jrouwe/JoltPhysics/archive/e77f175595e64cb44218cc9d9d56fc365ad0e36a.tar.gz
    URL_HASH SHA256=1f32328fb763135de10a244568d6ccb2ed9b1e6593fafe6dc6db5b2719d330bd
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR Build
    SYSTEM
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(JoltPhysics)

# ---------------------------------------------------------------- GLM
# 1.0.3, header-only (SOURCE_SUBDIR points to a folder without CMakeLists.txt, so only the sources are fetched).
FetchContent_Declare(glm
    URL https://github.com/g-truc/glm/archive/8d1fd52e5ab5590e2c81768ace50c72bae28f2ed.tar.gz
    URL_HASH SHA256=36eefbae41503e6822e8d9e1b4cc85c6a6621f17676f6258aaf0a64c2de0a09c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR header-only-no-cmake)
FetchContent_MakeAvailable(glm)
add_library(quench_glm INTERFACE)
target_include_directories(quench_glm SYSTEM INTERFACE ${glm_SOURCE_DIR})
target_compile_definitions(quench_glm INTERFACE
    GLM_FORCE_DEPTH_ZERO_TO_ONE # clip-space z in [0, 1] (Metal, Vulkan, Direct3D)
    GLM_FORCE_RADIANS)

# ---------------------------------------------------------------- enkiTS
# v1.12, the task scheduler under quench::JobSystem. Built here from its one source file (no C interface, no examples);
# its headers stay private to quench_core and quench_physics.
FetchContent_Declare(enkits
    URL https://github.com/dougbinks/enkiTS/archive/0289cf6ffce86697e1f46cb4eeca1c946bffec71.tar.gz
    URL_HASH SHA256=c1bcf45e359d2097045d43efa68084b8e0e2d8dc6bfeec532099cc4569e79fb4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(enkits)
add_library(quench_enkits STATIC ${enkits_SOURCE_DIR}/src/TaskScheduler.cpp)
target_include_directories(quench_enkits SYSTEM PUBLIC ${enkits_SOURCE_DIR}/src)
target_compile_definitions(quench_enkits PUBLIC ENKITS_TASK_PRIORITIES_NUM=3)
target_compile_features(quench_enkits PUBLIC cxx_std_20)
if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_link_libraries(quench_enkits PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------- xxHash
# v0.8.4, header-only use (XXH_INLINE_ALL in src/assets/AssetId.cpp): XXH64 of asset paths.
FetchContent_Declare(xxhash
    URL https://github.com/Cyan4973/xxHash/archive/c87183a77d67f7d37e3d2d1b7eaac5e7c695e4f0.tar.gz
    URL_HASH SHA256=ed9fa05b338a5b308eca0c25e93fe4ba8e3adfaa6f94bedf1f39b0a0d1010751
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(xxhash)
add_library(quench_xxhash INTERFACE)
target_include_directories(quench_xxhash SYSTEM INTERFACE ${xxhash_SOURCE_DIR})

# ---------------------------------------------------------------- meshoptimizer
# v1.3 (TrailForge's pin). The runtime decodes the cooked vertex and index streams with it; quench_cook optimizes,
# simplifies and encodes with it.
FetchContent_Declare(meshoptimizer
    URL https://github.com/zeux/meshoptimizer/archive/9e1f07b159d3cb777f1c67ed31fc11fd117986f4.tar.gz
    URL_HASH SHA256=e753b45fbc410031bd69cb278180ae31fa0d5430d0c3e662e3a3b100467ce0b3
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(meshoptimizer)
file(GLOB _meshopt_sources ${meshoptimizer_SOURCE_DIR}/src/*.cpp)
add_library(quench_meshoptimizer STATIC ${_meshopt_sources})
target_include_directories(quench_meshoptimizer SYSTEM PUBLIC ${meshoptimizer_SOURCE_DIR}/src)
target_compile_features(quench_meshoptimizer PUBLIC cxx_std_20)

# ---------------------------------------------------------------- bc7enc_rdo
# TrailForge's pin. bc7decomp.cpp (MIT or public domain) decodes BC7 at run time on devices without BC textures;
# quench_cook also encodes with bc7enc.cpp (cmake/CookToolchain.cmake). bc7e.ispc (Apache-2.0) is not used.
FetchContent_Declare(bc7enc
    URL https://github.com/richgel999/bc7enc_rdo/archive/b9438627eef73a1157e84201b6fa6eb2ffd6d9f0.tar.gz
    URL_HASH SHA256=bbb33d1dbb6178a3a2c044956c3cc0b8ea9fc2f2903824379dd86f87d53b64c6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(bc7enc)
add_library(quench_bc7decomp STATIC ${bc7enc_SOURCE_DIR}/bc7decomp.cpp)
target_include_directories(quench_bc7decomp SYSTEM PUBLIC ${bc7enc_SOURCE_DIR})
target_compile_features(quench_bc7decomp PUBLIC cxx_std_20)

if(QUENCH_HEADLESS)
    return()
endif()

# ---------------------------------------------------------------- SDL 3
# release-3.4.18
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
# The archive has no REVISION.txt, and git describe would find the enclosing repository instead.
set(SDL_REVISION "SDL-3.4.18-829a65d7" CACHE STRING "" FORCE)
FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/archive/829a65d769d935c4852f8159e964312c0957260a.tar.gz
    URL_HASH SHA256=48e15cf1a6928453d77afa955d1fdfbbca9169e2bc3d76d111acb9b8bb076d66
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    # iOS Simulator only (TARGET_OS_SIMULATOR): accept the Simulator's Metal device in SDL GPU.
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patches/SdlIosSimulator.cmake
    SYSTEM
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(SDL3)

# ---------------------------------------------------------------- Dear ImGui
# v1.92.9b. No CMake of its own: quench_imgui is built here with the SDL3 platform backend only. The engine draws ImGui
# through quench::gpu (src/app/ImGuiRenderer.cpp), so ImGui's SDL GPU renderer backend, and TrailForge's iOS patch for
# it, are not used.
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/f1cc2ae15e53a861a874c3034aae6798fde194ab.tar.gz
    URL_HASH SHA256=f30be7f3e2136615b1848a9afb72f6dfd8a60820ddf1c1a85997bf3b722e7cfc
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)
add_library(quench_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp)
target_include_directories(quench_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(quench_imgui PUBLIC SDL3::SDL3-static)
target_compile_features(quench_imgui PUBLIC cxx_std_20)
