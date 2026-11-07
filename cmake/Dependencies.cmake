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

# ---------------------------------------------------------------- MikkTSpace
# The reference implementation (Morten S. Mikkelsen, zlib licence in both source files; the repository's last commit,
# 2020-03-25). quench_assets generates tangents with it: quench_cook for glTF files without TANGENT, games for their
# procedural meshes (assets::generateTangents, render::computeTangents).
FetchContent_Declare(mikktspace
    URL https://github.com/mmikk/MikkTSpace/archive/3e895b49d05ea07e4c2133156cfa94369e19e409.tar.gz
    URL_HASH SHA256=aeba65ddee85a679133d510d71d00b108f603752f90b15b9ecf7777033f6e351
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    # A seed rotation shifted by 32 (undefined behaviour) is masked; the tangents do not change.
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patches/MikkTSpaceRotate.cmake
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(mikktspace)
add_library(quench_mikktspace STATIC ${mikktspace_SOURCE_DIR}/mikktspace.c)
target_include_directories(quench_mikktspace SYSTEM PUBLIC ${mikktspace_SOURCE_DIR})

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

# ---------------------------------------------------------------- Basis Universal
# v2.50 (Apache-2.0). Not a FetchContent archive: cmake/QuenchBasisu.cmake keeps its sources once per machine.
include(${CMAKE_CURRENT_LIST_DIR}/QuenchBasisu.cmake)

# ---------------------------------------------------------------- miniaudio
# 0.11.25 (TrailForge's pin; public domain or MIT-0). quench_audio decodes WAV, FLAC, MP3 and Ogg Vorbis with it (Vorbis
# through stb_vorbis 1.22 from the same archive, extras/stb_vorbis.c, MIT or public domain), and quench_audio_device
# plays through its device API. Built from src/audio/Miniaudio.c (miniaudio and stb_vorbis in one unit) with the
# high-level parts compiled out (engine, node graph, resource manager, waveform generation, encoders); the defines are
# PUBLIC so that the header sees the same configuration. Headless and web builds have no device I/O (and the
# single-threaded web build no threads).
FetchContent_Declare(miniaudio
    URL https://github.com/mackron/miniaudio/archive/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d.tar.gz
    URL_HASH SHA256=1a3a79b80fc6f0b0cc155e28b954a598e0ddfa2db64e2afa8466be88c476fa55
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(miniaudio)
set(_quench_miniaudio_source ${CMAKE_CURRENT_LIST_DIR}/../src/audio/Miniaudio.c)
add_library(quench_miniaudio STATIC ${_quench_miniaudio_source})
target_include_directories(quench_miniaudio SYSTEM PUBLIC ${miniaudio_SOURCE_DIR})
target_compile_definitions(quench_miniaudio PUBLIC
    MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_GENERATION MA_NO_ENCODING)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(quench_miniaudio PRIVATE -w)
endif()
if(QUENCH_HEADLESS OR EMSCRIPTEN)
    target_compile_definitions(quench_miniaudio PUBLIC MA_NO_DEVICE_IO)
    if(EMSCRIPTEN)
        target_compile_definitions(quench_miniaudio PUBLIC MA_NO_THREADING)
    endif()
elseif(APPLE)
    # Core Audio linked at build time instead of loaded at run time (miniaudio's advice for notarization); the null
    # backend is a callback thread without sound (--audio-device null, the tests).
    target_compile_definitions(quench_miniaudio PUBLIC
        MA_NO_RUNTIME_LINKING MA_ENABLE_ONLY_SPECIFIC_BACKENDS MA_ENABLE_COREAUDIO MA_ENABLE_NULL)
    target_link_libraries(quench_miniaudio PUBLIC "-framework CoreFoundation" "-framework CoreAudio"
                                                  "-framework AudioToolbox")
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # The iOS backend uses AVAudioSession (Objective-C).
        set_source_files_properties(${_quench_miniaudio_source} PROPERTIES LANGUAGE OBJC)
        target_link_libraries(quench_miniaudio PUBLIC "-framework AVFoundation" "-framework Foundation")
    endif()
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(quench_miniaudio PUBLIC ${CMAKE_DL_LIBS} Threads::Threads m)
endif()

# ---------------------------------------------------------------- ozz-animation
# 0.17.0 (MIT). quench_animation samples, blends and solves IK with its runtime, and builds skeletons and clips with its
# offline library (raw data, builders, keyframe optimizer, additive clips, motion extraction: quench_cook, tests, games
# that make animation data at run time; a game that does not call the build functions does not link it). ozz's own
# CMake needs 3.30, so the libraries are built here from their sources; its headers stay private to quench_animation.
# On ARM and WebAssembly ozz uses its scalar reference math (its SIMD paths are SSE and AVX only).
FetchContent_Declare(ozz
    URL https://github.com/guillaumeblanc/ozz-animation/archive/744eb9d99f606eda849acb0b1204f7a3dc20bca1.tar.gz
    URL_HASH SHA256=a29a8a014b571487d9a6dabebeed57627fc19bf1f0ebf531b0c76384840c3b74
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(ozz)
add_library(quench_ozz STATIC
    ${ozz_SOURCE_DIR}/src/base/memory/allocator.cc
    ${ozz_SOURCE_DIR}/src/base/platform.cc
    ${ozz_SOURCE_DIR}/src/base/log.cc
    ${ozz_SOURCE_DIR}/src/base/containers/string_archive.cc
    ${ozz_SOURCE_DIR}/src/base/encode/group_varint.cc
    ${ozz_SOURCE_DIR}/src/base/io/archive.cc
    ${ozz_SOURCE_DIR}/src/base/io/stream.cc
    ${ozz_SOURCE_DIR}/src/base/maths/box.cc
    ${ozz_SOURCE_DIR}/src/base/maths/simd_math.cc
    ${ozz_SOURCE_DIR}/src/base/maths/math_archive.cc
    ${ozz_SOURCE_DIR}/src/base/maths/soa_math_archive.cc
    ${ozz_SOURCE_DIR}/src/base/maths/simd_math_archive.cc
    ${ozz_SOURCE_DIR}/src/animation/runtime/animation.cc
    ${ozz_SOURCE_DIR}/src/animation/runtime/animation_utils.cc
    ${ozz_SOURCE_DIR}/src/animation/runtime/blending_job.cc
    ${ozz_SOURCE_DIR}/src/animation/runtime/ik_aim_job.cc
    ${ozz_SOURCE_DIR}/src/animation/runtime/ik_two_bone_job.cc
    ${ozz_SOURCE_DIR}/src/animation/runtime/local_to_model_job.cc
    ${ozz_SOURCE_DIR}/src/animation/runtime/motion_blending_job.cc
    ${ozz_SOURCE_DIR}/src/animation/runtime/sampling_job.cc
    ${ozz_SOURCE_DIR}/src/animation/runtime/skeleton.cc
    ${ozz_SOURCE_DIR}/src/animation/runtime/skeleton_utils.cc
    ${ozz_SOURCE_DIR}/src/animation/runtime/track.cc
    ${ozz_SOURCE_DIR}/src/animation/runtime/track_sampling_job.cc
    ${ozz_SOURCE_DIR}/src/animation/runtime/track_triggering_job.cc)
target_include_directories(quench_ozz SYSTEM PUBLIC ${ozz_SOURCE_DIR}/include PRIVATE ${ozz_SOURCE_DIR}/src)
target_compile_features(quench_ozz PUBLIC cxx_std_20)
set(_ozz_offline ${ozz_SOURCE_DIR}/src/animation/offline)
add_library(quench_ozz_offline STATIC
    ${_ozz_offline}/raw_animation.cc
    ${_ozz_offline}/raw_animation_archive.cc
    ${_ozz_offline}/raw_animation_utils.cc
    ${_ozz_offline}/animation_builder.cc
    ${_ozz_offline}/animation_optimizer.cc
    ${_ozz_offline}/additive_animation_builder.cc
    ${_ozz_offline}/motion_extractor.cc
    ${_ozz_offline}/raw_skeleton.cc
    ${_ozz_offline}/raw_skeleton_archive.cc
    ${_ozz_offline}/skeleton_builder.cc
    ${_ozz_offline}/raw_track.cc
    ${_ozz_offline}/raw_track_utils.cc
    ${_ozz_offline}/track_builder.cc
    ${_ozz_offline}/track_optimizer.cc)
target_include_directories(quench_ozz_offline PRIVATE ${ozz_SOURCE_DIR}/src)
target_link_libraries(quench_ozz_offline PUBLIC quench_ozz)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(quench_ozz PRIVATE -w)
    target_compile_options(quench_ozz_offline PRIVATE -w)
    # The scalar math's reciprocal square root estimate subtracts float bits as int, also in unused lanes that hold
    # negative values: signed overflow, undefined unless it wraps (UBSan reports it in the two-bone IK). Every file that
    # includes ozz's headers gets -fwrapv.
    target_compile_options(quench_ozz PUBLIC -fwrapv)
endif()

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
