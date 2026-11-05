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
