cmake_minimum_required(VERSION 3.28)
# macOS deployment target when the engine is built on its own (a game sets its own before project()). 14.0 matches the
# metallib target. iOS presets set their own target (26.0).
if(NOT DEFINED PROJECT_NAME AND CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS"
   AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "Minimum macOS version" FORCE)
endif()
project(Quench VERSION 0.9.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
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
option(QUENCH_BUILD_EXAMPLES "Build the examples (examples/)" ${PROJECT_IS_TOP_LEVEL})
# Inside a game the engine is built without -Werror: a warning added by a newer compiler must not break the game.
option(QUENCH_WERROR "Treat warnings in engine targets as errors" ${PROJECT_IS_TOP_LEVEL})
set(QUENCH_ASSERTS AUTO CACHE STRING "QUENCH_ASSERT: AUTO (on unless NDEBUG), ON or OFF")
set_property(CACHE QUENCH_ASSERTS PROPERTY STRINGS AUTO ON OFF)
set(QUENCH_ASSET_OVERLAY AUTO CACHE STRING
    "Assets in the user directory's asset_overlay/ replace bundled ones: AUTO (on unless NDEBUG), ON or OFF")
set_property(CACHE QUENCH_ASSET_OVERLAY PROPERTY STRINGS AUTO ON OFF)
# Headless: only the modules without windowing, GPU or SDL (core, physics); SDL is not fetched.
option(QUENCH_HEADLESS "Build only the headless modules" OFF)
if(EMSCRIPTEN)
    # SDL GPU has no WebGPU backend in the pinned SDL.
    set(QUENCH_HEADLESS ON CACHE BOOL "" FORCE)
endif()

include(cmake/QuenchTargets.cmake)
include(cmake/Dependencies.cmake)
include(cmake/QuenchShaders.cmake)
include(cmake/QuenchGame.cmake)

# ------------------------------------------------------------------ core
# Headless: no SDL, no GPU. Result, assertions, logging, fixed-step clock, JSON, file system, number parsing, thread
# priority, the job system (enkiTS).
add_library(quench_core STATIC
    src/core/Assert.cpp
    src/core/FileSystem.cpp
    src/core/Jobs.cpp
    src/core/Json.cpp
    src/core/Log.cpp
    src/core/Parse.cpp
    src/core/Thread.cpp)
quench_configure_target(quench_core)
target_link_libraries(quench_core PRIVATE quench_enkits)
if(QUENCH_ASSERTS STREQUAL "ON")
    target_compile_definitions(quench_core PUBLIC QUENCH_ENABLE_ASSERTS=1)
elseif(QUENCH_ASSERTS STREQUAL "OFF")
    target_compile_definitions(quench_core PUBLIC QUENCH_ENABLE_ASSERTS=0)
endif()

# ------------------------------------------------------------------ physics
# Headless: Jolt Physics wrapper.
add_library(quench_physics STATIC src/physics/JoltJobSystem.cpp src/physics/PhysicsWorld.cpp)
quench_configure_target(quench_physics)
target_link_libraries(quench_physics PUBLIC quench_core Jolt)
if(EMSCRIPTEN)
    # Single-threaded, Jolt runs its jobs nested on the stack of step(): Emscripten's 64 KB default overflows (found by
    # the wasm-node tests). 1 MB is what Jolt links its own WebAssembly programs with.
    target_link_options(quench_physics INTERFACE -sSTACK_SIZE=1048576)
endif()

# ------------------------------------------------------------------ assets
# Headless: asset IDs, the asset manager (cache, budgets, loads on the job system, hot reload), cooked KTX2 textures
# and models, streaming by distance. GPU resources for textures and models are quench_render's (SceneAssets).
add_library(quench_assets STATIC
    src/assets/AssetId.cpp
    src/assets/AssetManager.cpp
    src/assets/Model.cpp
    src/assets/StreamingGrid.cpp
    src/assets/Tangents.cpp
    src/assets/Texture.cpp)
quench_configure_target(quench_assets)
target_link_libraries(quench_assets PUBLIC quench_core quench_glm PRIVATE quench_xxhash quench_meshoptimizer
                                                                          quench_bc7decomp quench_mikktspace)

# ------------------------------------------------------------------ audio
# Headless: the mixer (buses, voices, streams, 3D sound), sounds as assets, WAV writing and signal analysis. The device
# (miniaudio's playback API and the iOS audio session) is quench_audio_device below.
add_library(quench_audio STATIC
    src/audio/Analysis.cpp
    src/audio/AudioSystem.cpp
    src/audio/Decoder.cpp
    src/audio/Hrtf.cpp
    src/audio/Mixer.cpp
    src/audio/Sound.cpp
    src/audio/Spatial.cpp
    src/audio/Wav.cpp)
quench_configure_target(quench_audio)
target_link_libraries(quench_audio PUBLIC quench_core quench_assets quench_glm PRIVATE quench_miniaudio)

# ------------------------------------------------------------------ animation
# Headless: skeletons, clips and graphs as assets, state machines, blending, events, root motion, IK and foot
# placement, skinning palettes; characters updated in parallel on the job system. ozz-animation underneath.
add_library(quench_animation STATIC
    src/animation/AnimationFile.cpp
    src/animation/AnimationSystem.cpp
    src/animation/Assets.cpp
    src/animation/Build.cpp
    src/animation/Character.cpp
    src/animation/Clip.cpp
    src/animation/Graph.cpp
    src/animation/Ik.cpp
    src/animation/Skeleton.cpp)
quench_configure_target(quench_animation)
target_link_libraries(quench_animation PUBLIC quench_core quench_assets quench_glm
                                       PRIVATE quench_ozz quench_ozz_offline quench_xxhash)

# The asset cooker (host tool, tools/cook) and quench_cook_assets().
include(cmake/QuenchCook.cmake)

# ------------------------------------------------------------------ gpu
# Headless: the table type of embedded shaders (quench_add_shaders).
add_library(quench_gpu_shaders INTERFACE)
# shaders/: the engine shader library, also read by C++ for the shared uniform structs (shaders/quench/Shared.h).
target_include_directories(quench_gpu_shaders INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                        ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
target_compile_features(quench_gpu_shaders INTERFACE cxx_std_20)

if(NOT QUENCH_HEADLESS)
    # The GPU layer: the front end (src/gpu), the SDL GPU backend (src/gpu/sdlgpu) and, on Apple platforms, the native
    # Metal backend (src/gpu/metal). Backend types stay out of the public headers.
    add_library(quench_gpu STATIC
        src/gpu/CommandBuffer.cpp
        src/gpu/Device.cpp
        src/gpu/Format.cpp
        src/gpu/ShaderReload.cpp
        src/gpu/sdlgpu/SdlGpuCommands.cpp
        src/gpu/sdlgpu/SdlGpuDevice.cpp)
    if(APPLE)
        # Objective-C++ with ARC (quench_configure_target): the Metal objects are strong members of plain C++ structs.
        target_sources(quench_gpu PRIVATE src/gpu/sdlgpu/SdlGpuTimingMetal.m src/gpu/metal/MetalCommands.mm
                                          src/gpu/metal/MetalDevice.mm)
        target_link_libraries(quench_gpu PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Foundation")
        # MetalFX (upscaling on the native Metal backend); the iOS Simulator's SDK does not have it.
        if(NOT CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
            target_link_libraries(quench_gpu PRIVATE "-framework MetalFX")
        endif()
    endif()
    quench_configure_target(quench_gpu)
    target_link_libraries(quench_gpu PUBLIC quench_core quench_gpu_shaders PRIVATE SDL3::SDL3-static quench_glm)
    if(APPLE)
        # CommandBuffer::blit on the native Metal backend.
        quench_add_shaders(quench_gpu NAME gpu_blit SOURCE src/gpu/shaders/blit.hlsl
            ENTRIES fullscreen_vs:vertex blit_2d_fs:fragment blit_2d_array_fs:fragment)
    endif()
    # The shader formats every quench_add_shaders() set of this build embeds.
    target_compile_definitions(quench_gpu PRIVATE
        QUENCH_SHADERS_METALLIB=$<AND:$<BOOL:${APPLE}>,$<BOOL:${QUENCH_SHADER_METAL_OK}>>
        QUENCH_SHADERS_MSL=$<AND:$<BOOL:${APPLE}>,$<NOT:$<BOOL:${QUENCH_SHADER_METAL_OK}>>>
        QUENCH_SHADERS_DXIL=$<BOOL:${QUENCH_SHADER_DXC}>)

    # ------------------------------------------------------------------ audio device
    # miniaudio's playback device pulling the mix of quench_audio; on iOS also the audio session (AVAudioSession).
    add_library(quench_audio_device STATIC src/audio/Device.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_sources(quench_audio_device PRIVATE src/audio/AudioSessionApple.m)
        target_link_libraries(quench_audio_device PRIVATE "-framework AVFoundation")
    endif()
    quench_configure_target(quench_audio_device)
    target_link_libraries(quench_audio_device PUBLIC quench_audio PRIVATE quench_miniaudio)

    # ------------------------------------------------------------------ app
    # The app shell: the game interface, window, lifecycle, input, ImGui, scenarios, captures, and the SDL backends of
    # the core services (files, log). main() itself is in quench_main, linked by quench_add_game().
    add_library(quench_app STATIC
        src/app/App.cpp
        src/app/AppScenario.cpp
        src/app/Capture.cpp
        src/app/DisplayHeadroom.cpp
        src/app/ImGuiRenderer.cpp
        src/app/Input.cpp
        src/app/Options.cpp
        src/app/Scenario.cpp
        src/app/ScenarioPoint.cpp
        src/app/SdlFileSystem.cpp
        src/app/SdlLog.cpp
        src/app/ThermalState.cpp
        src/app/UiComposite.cpp)
    if(APPLE)
        target_sources(quench_app PRIVATE src/app/ActivationApple.m src/app/DisplayHeadroomApple.m
                                          src/app/ThermalStateApple.m)
        if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
            target_link_libraries(quench_app PRIVATE "-framework UIKit")
        else()
            target_link_libraries(quench_app PRIVATE "-framework AppKit")
        endif()
    endif()
    quench_configure_target(quench_app)
    target_link_libraries(quench_app PUBLIC quench_core quench_assets quench_audio quench_audio_device quench_gpu
                                            quench_glm quench_imgui SDL3::SDL3-static)
    target_compile_definitions(quench_app PRIVATE QUENCH_VERSION="${PROJECT_VERSION}")
    if(QUENCH_ASSET_OVERLAY STREQUAL "ON")
        target_compile_definitions(quench_app PRIVATE QUENCH_ASSET_OVERLAY=1)
    elseif(QUENCH_ASSET_OVERLAY STREQUAL "OFF")
        target_compile_definitions(quench_app PRIVATE QUENCH_ASSET_OVERLAY=0)
    endif()
    quench_add_shaders(quench_app NAME imgui SOURCE src/app/shaders/imgui.hlsl
        ENTRIES imgui_vs:vertex imgui_fs:fragment fullscreen_vs:vertex ui_composite_fs:fragment)

    # ------------------------------------------------------------------ render
    # The retained scene and the standard frame (shadows, HDR pass, sky, fog, TAA, bloom, tone mapping) on quench_gpu.
    add_library(quench_render STATIC
        src/render/Cascades.cpp
        src/render/Daylight.cpp
        src/render/Effects.cpp
        src/render/MeshBuilder.cpp
        src/render/PathTracer.cpp
        src/render/Projection.cpp
        src/render/RayTracing.cpp
        src/render/Renderer.cpp
        src/render/Scene.cpp
        src/render/SceneAssets.cpp
        src/render/TemporalFilter.cpp
        src/render/ToneMap.cpp)
    quench_configure_target(quench_render)
    target_link_libraries(quench_render PUBLIC quench_core quench_assets quench_gpu quench_gpu_shaders quench_glm)
    quench_add_shaders(quench_render NAME render_frame SOURCE src/render/shaders/frame.hlsl
        ENTRIES fullscreen_vs:vertex shadow_vs:vertex shadow_fs:fragment depth_vs:vertex sky_fs:fragment
                velocity_fs:fragment velocity_obj_vs:vertex velocity_skin_vs:vertex velocity_obj_fs:fragment
                velocity_cutout_fs:fragment taa_fs:fragment bloom_down_fs:fragment bloom_up_fs:fragment tonemap_fs:fragment fxaa_fs:fragment)
    quench_add_shaders(quench_render NAME render_standard SOURCE src/render/shaders/standard.hlsl
        ENTRIES surface_vs:vertex surface_fs:fragment surface_basic_fs:fragment surface_local_shadows_fs:fragment
                surface_shadow_fs:fragment surface_transmittance_fs:fragment surface_gbuffer_fs:fragment)
    quench_add_shaders(quench_render NAME render_skin SOURCE src/render/shaders/skin.hlsl ENTRIES skin_cs:compute skin_morph_cs:compute)
    # Ray tracing (gpu::Caps::rayTracing): the rayquery entries need DXC (cmake/QuenchDxc.cmake); without it the sets
    # have the other entries only and the renderer uses the shadow maps and the sky's light.
    quench_add_shaders(quench_render NAME render_raytrace SOURCE src/render/shaders/raytrace.hlsl
        ENTRIES rt_shadow_fs:fragment:rayquery rt_shadow_local_fs:fragment:rayquery rt_denoise_fs:fragment)
    quench_add_shaders(quench_render NAME render_raylight SOURCE src/render/shaders/raylight.hlsl
        ENTRIES rt_reflect_fs:fragment:rayquery rt_indirect_fs:fragment:rayquery rt_probe_trace_fs:fragment:rayquery
                pt_fs:fragment:rayquery rt_reflect_maps_fs:fragment:rayquery rt_indirect_maps_fs:fragment:rayquery
                rt_probe_trace_maps_fs:fragment:rayquery pt_maps_fs:fragment:rayquery
                rt_probe_irradiance_fs:fragment rt_probe_distance_fs:fragment)
    quench_add_shaders(quench_render NAME render_raydenoise SOURCE src/render/shaders/raydenoise.hlsl
        ENTRIES rt_temporal_fs:fragment rt_atrous_fs:fragment rt_denoise_inputs_fs:fragment)

    # SDL main callbacks (src/app/Main.cpp): an object library, so the symbols land in the game executable.
    add_library(quench_main OBJECT src/app/Main.cpp)
    quench_configure_target(quench_main)
    target_link_libraries(quench_main PUBLIC quench_app)
endif()

# ------------------------------------------------------------------ examples and tests
if(QUENCH_BUILD_EXAMPLES AND NOT QUENCH_HEADLESS)
    add_subdirectory(examples)
endif()
if(QUENCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
