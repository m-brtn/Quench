cmake_minimum_required(VERSION 3.28)
# macOS 14 is the engine's minimum (its metallibs); the iOS presets set 26.0.
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "Minimum macOS version" FORCE)
endif()
project(outpost VERSION 0.1.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The engine at a pinned commit. Working on the engine at the same time: configure with
# -DFETCHCONTENT_SOURCE_DIR_QUENCH=<engine checkout>.
include(FetchContent)
FetchContent_Declare(quench
    GIT_REPOSITORY https://github.com/m-brtn/Quench.git
    GIT_TAG 3c5d68b2256994ec4c25d47d1864d60e991f183c) # 0.15.0
FetchContent_MakeAvailable(quench)

# The host-tools preset builds only the engine's shader compiler and asset cooker (for the iOS builds).
if(QUENCH_HEADLESS)
    return()
endif()

# The models are written by tools/make_models.py (Blender, run by hand) and cooked here.
set(outpost_models base pod_glow walker_body walker_thigh walker_shin walker_foot cell
    base_outline walker_body_outline walker_thigh_outline walker_shin_outline walker_foot_outline cell_outline)
list(TRANSFORM outpost_models PREPEND models/)
list(TRANSFORM outpost_models APPEND .glb)
quench_cook_assets(outpost_cooked SOURCE_DIR assets_src MODELS ${outpost_models} MODEL_OPTIONS --lods 1)

quench_add_game(outpost
    NAME "Outpost"
    BUNDLE_ID com.example.outpost
    ICON icon/icon.png
    ASSETS assets
    COOKED_ASSETS outpost_cooked
    ORIENTATIONS landscape
    SOURCES src/Game.cpp)
target_link_libraries(outpost PRIVATE quench_render quench_navigation)
target_include_directories(outpost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(outpost PRIVATE -Wall -Wextra)
endif()

if(NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_test(NAME smoke COMMAND outpost --window 1280x720 --audio-device null
        --user-dir ${CMAKE_CURRENT_BINARY_DIR}/user --script ${CMAKE_CURRENT_SOURCE_DIR}/scenarios/smoke.txt)
    # A phone's defaults: the frame of an iPhone 17 held sideways.
    add_test(NAME mobile COMMAND outpost --mobile --window 874x402 --frame-size 2622x1206 --audio-device null
        --user-dir ${CMAKE_CURRENT_BINARY_DIR}/user --script ${CMAKE_CURRENT_SOURCE_DIR}/scenarios/mobile.txt)
    set_tests_properties(smoke mobile PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL line")
endif()
