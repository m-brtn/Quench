cmake_minimum_required(VERSION 3.28)
# macOS 14 is the engine's minimum (its metallibs); the iOS presets set 26.0.
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "Minimum macOS version" FORCE)
endif()
project(template_game VERSION 0.1.0 LANGUAGES C CXX)

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
    GIT_TAG QUENCH_ENGINE_COMMIT)
FetchContent_MakeAvailable(quench)

# The host-tools preset builds only the engine's shader compiler (for the iOS builds).
if(QUENCH_HEADLESS)
    return()
endif()

quench_add_game(template_game
    NAME "Template Game"
    BUNDLE_ID org.example.template-game
    ICON icon/icon.png
    ASSETS assets
    ORIENTATIONS landscape
    SOURCES src/Game.cpp)
target_include_directories(template_game PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(template_game PRIVATE -Wall -Wextra)
endif()
quench_add_shaders(template_game NAME game SOURCE shaders/game.hlsl ENTRIES game_vs:vertex game_fs:fragment)

if(NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_test(NAME smoke COMMAND template_game --window 640x360 --audio-device null
             --script ${CMAKE_CURRENT_SOURCE_DIR}/scenarios/smoke.txt)
    set_tests_properties(smoke PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL line")
endif()
