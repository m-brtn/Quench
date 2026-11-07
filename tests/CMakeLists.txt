# The audio tests' decoder files (audio/data), embedded so that the device test app has them too.
set(audio_test_data ${CMAKE_CURRENT_BINARY_DIR}/AudioTestData.cpp)
set(audio_test_data_content "#include <cstdint>\n#include <span>\n\nnamespace quench::test {\n")
foreach(entry tone.flac:toneFlac tone.mp3:toneMp3 loop.ogg:loopOgg)
    string(REPLACE ":" ";" entry "${entry}")
    list(GET entry 0 file)
    list(GET entry 1 function)
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/audio/data/${file} hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(APPEND audio_test_data_content "std::span<const std::uint8_t> ${function}();\n"
           "std::span<const std::uint8_t> ${function}() {\n    static const std::uint8_t bytes[] = {${bytes}};\n"
           "    return bytes;\n}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/audio/data/${file})
endforeach()
string(APPEND audio_test_data_content "} // namespace quench::test\n")
file(CONFIGURE OUTPUT ${audio_test_data} CONTENT "${audio_test_data_content}" @ONLY)

# A device build (iOS on an iPhone or iPad) cannot run executables from CTest: the cases that do not need the build
# tree go into one app, quench_device_tests, installed and launched with devicectl (docs/platforms.md).
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    quench_add_game(quench_device_tests NAME "Quench Tests" OUTPUT_NAME QuenchTests BUNDLE_ID org.quench.device-tests
        ORIENTATIONS all CUSTOM_MAIN
        SOURCES
            harness/TestMain.cpp
            core/AssertTests.cpp
            core/FileSystemTests.cpp
            core/FixedStepClockTests.cpp
            core/JobsTests.cpp
            core/JsonTests.cpp
            core/LogTests.cpp
            core/ResultTests.cpp
            core/ThreadTests.cpp
            physics/PhysicsWorldTests.cpp
            assets/AssetManagerTests.cpp
            assets/ModelTests.cpp
            assets/StreamingGridTests.cpp
            assets/TextureTests.cpp
            audio/AudioTests.cpp
            audio/DeviceTests.cpp
            ${audio_test_data}
            animation/AnimationTests.cpp
            app/DisplayHeadroomTests.cpp
            app/InputTests.cpp
            app/OptionsTests.cpp
            app/ScenarioPointTests.cpp
            app/ScenarioTests.cpp
            app/SdlFileSystemTests.cpp
            app/SdlLogTests.cpp
            app/ThermalStateTests.cpp
            gpu/DeviceTests.cpp
            gpu/GpuTest.cpp
            gpu/RayTracingTests.cpp
            gpu/RenderTests.cpp
            gpu/ShaderReloadTests.cpp
            gpu/WindowTests.cpp
            render/RenderMathTests.cpp
            render/RendererTests.cpp
            render/SceneAssetsTests.cpp)
    quench_configure_target(quench_device_tests)
    target_include_directories(quench_device_tests PRIVATE harness gpu)
    target_link_libraries(quench_device_tests PRIVATE quench_physics quench_assets quench_audio_device quench_render
                                                      quench_animation)
    target_compile_definitions(quench_device_tests PRIVATE
        QUENCH_TEST_SDL_MAIN=1
        QUENCH_TEST_METALLIB=$<BOOL:${QUENCH_SHADER_METAL_OK}>)
    quench_add_shaders(quench_device_tests NAME render SOURCE gpu/shaders/render.hlsl
        ENTRIES color_vs:vertex color_fs:fragment fullscreen_vs:vertex uniform_fs:fragment texture_fs:fragment
                rects_vs:vertex scale_cs:compute ray_fs:fragment:rayquery ray_face_fs:fragment:rayquery)
    quench_add_shaders(quench_device_tests NAME render_test SOURCE render/shaders/custom.hlsl
        ENTRIES surface_vs:vertex surface_fs:fragment surface_shadow_vs:vertex surface_shadow_fs:fragment
                surface_transmittance_fs:fragment)
    quench_add_shaders(quench_device_tests NAME reload_test SOURCE gpu/shaders/reload.hlsl
        ENTRIES reload_vs:vertex reload_fs:fragment)
    return()
endif()

add_library(quench_test_harness STATIC harness/TestMain.cpp)
quench_configure_target(quench_test_harness)
target_include_directories(quench_test_harness PUBLIC harness)
target_compile_definitions(quench_test_harness PRIVATE QUENCH_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

# Every test has the label of the module it checks (ctest -L <label>; tools/check.sh quick picks them from the changed
# files): core, physics, assets, audio, animation, cook, gpu, render, app, hello, demo. The variants on Vulkan through
# MoltenVK also have the label vulkan: tools/check.sh quick leaves them out (-LE vulkan), full and release run them.
# quench_add_test_executable(<name> PREFIX <ctest prefix> LABEL <label> SOURCES <file>... LIBS <target>...
#                            [ENVIRONMENT <NAME=value>...])
function(quench_add_test_executable name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "PREFIX;LABEL" "SOURCES;LIBS;ENVIRONMENT")
    add_executable(${name} ${arg_SOURCES})
    quench_configure_target(${name})
    target_link_libraries(${name} PRIVATE quench_test_harness ${arg_LIBS})
    quench_add_test_variant(${name} PREFIX ${arg_PREFIX} LABEL ${arg_LABEL} ENVIRONMENT ${arg_ENVIRONMENT})
endfunction()

# Registers every case of a test executable under PREFIX, run with the given environment. A second call with another
# prefix runs the same cases again (another GPU driver, for example). A case whose output matches FAIL_REGEX fails.
# quench_add_test_variant(<name> PREFIX <ctest prefix> LABEL <label>... [ENVIRONMENT <NAME=value>...]
#                         [FAIL_REGEX <regex>])
function(quench_add_test_variant name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "PREFIX;FAIL_REGEX" "LABEL;ENVIRONMENT")
    if(CMAKE_CROSSCOMPILING_EMULATOR MATCHES "simctl" AND arg_ENVIRONMENT)
        # simctl passes only SIMCTL_CHILD_ variables to the process, without the prefix.
        list(TRANSFORM arg_ENVIRONMENT PREPEND SIMCTL_CHILD_)
    endif()
    list(JOIN arg_ENVIRONMENT "|" environment)
    list(JOIN arg_LABEL "|" labels)
    set(list_file ${CMAKE_CURRENT_BINARY_DIR}/${arg_PREFIX}_cases.cmake)
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:${name}> -DPREFIX=${arg_PREFIX} -DOUTPUT=${list_file}
                -DWORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR} -DSKIP_RETURN_CODE=77 "-DLABEL=${labels}"
                "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}" "-DENVIRONMENT=${environment}"
                "-DFAIL_REGEX=${arg_FAIL_REGEX}" -P ${CMAKE_CURRENT_SOURCE_DIR}/DiscoverTests.cmake
        VERBATIM)
    set(include_file ${CMAKE_CURRENT_BINARY_DIR}/${arg_PREFIX}_include.cmake)
    file(CONFIGURE OUTPUT ${include_file} CONTENT
"if(EXISTS [=[${list_file}]=])
    include([=[${list_file}]=])
else()
    add_test([=[${arg_PREFIX}_NOT_BUILT]=] [=[${arg_PREFIX}_NOT_BUILT]=])
    set_tests_properties([=[${arg_PREFIX}_NOT_BUILT]=] PROPERTIES LABELS [=[${arg_LABEL}]=])
endif()
")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES ${include_file})
endfunction()

quench_add_test_executable(quench_core_tests PREFIX core LABEL core
    SOURCES
        core/AssertTests.cpp
        core/FileSystemTests.cpp
        core/FixedStepClockTests.cpp
        core/JobsTests.cpp
        core/JsonTests.cpp
        core/LogTests.cpp
        core/ResultTests.cpp
        core/ThreadTests.cpp
    LIBS quench_core)

quench_add_test_executable(quench_physics_tests PREFIX physics LABEL physics
    SOURCES physics/PhysicsWorldTests.cpp
    LIBS quench_physics)

quench_add_test_executable(quench_assets_tests PREFIX assets LABEL assets
    SOURCES
        assets/AssetManagerTests.cpp
        assets/ModelTests.cpp
        assets/StreamingGridTests.cpp
        assets/TangentsTests.cpp
        assets/TextureTests.cpp
    LIBS quench_assets)

quench_add_test_executable(quench_audio_tests PREFIX audio LABEL audio
    SOURCES audio/AudioTests.cpp ${audio_test_data}
    LIBS quench_audio)

quench_add_test_executable(quench_animation_tests PREFIX animation LABEL animation
    SOURCES animation/AnimationTests.cpp
    LIBS quench_animation)

# quench_cook (host builds that build the cook themselves): the library's cases, and the command line run twice on the
# demo's sources and its generated character with byte-identical outputs.
if(TARGET quench_cook_lib)
    quench_add_test_executable(quench_cook_tests PREFIX cook LABEL cook
        SOURCES cook/CookTests.cpp
        LIBS quench_cook_lib quench_glm)
    target_compile_definitions(quench_cook_tests PRIVATE QUENCH_TEST_SOURCE_DIR="${Quench_SOURCE_DIR}")
    find_package(Python3 COMPONENTS Interpreter)
    set(_character_args)
    # The demo's two characters for the retargeting cases, written at build time (the cases skip without Python).
    set(_characters "")
    if(Python3_Interpreter_FOUND)
        set(_character_args -DPYTHON=${Python3_EXECUTABLE}
            -DCHARACTER=${Quench_SOURCE_DIR}/tools/assets/make_demo_character.py)
        set(_characters ${CMAKE_CURRENT_BINARY_DIR}/characters)
        set(_character_files mannequin.gltf mannequin.bin mannequin.anim.json strider.gltf strider.bin
            strider.retarget.json)
        list(TRANSFORM _character_files PREPEND ${_characters}/)
        add_custom_command(
            OUTPUT ${_character_files}
            COMMAND ${Python3_EXECUTABLE} ${Quench_SOURCE_DIR}/tools/assets/make_demo_character.py ${_characters}
            COMMAND ${Python3_EXECUTABLE} ${Quench_SOURCE_DIR}/tools/assets/make_demo_strider.py ${_characters}
            DEPENDS ${Quench_SOURCE_DIR}/tools/assets/make_demo_character.py
                    ${Quench_SOURCE_DIR}/tools/assets/make_demo_strider.py
            COMMENT "Writing the demo's characters for quench_cook_tests"
            VERBATIM)
        add_custom_target(quench_cook_test_characters DEPENDS ${_character_files})
        add_dependencies(quench_cook_tests quench_cook_test_characters)
    endif()
    target_compile_definitions(quench_cook_tests PRIVATE QUENCH_TEST_CHARACTER_DIR="${_characters}")
    add_test(NAME cook_cli COMMAND ${CMAKE_COMMAND} -DCOOK=$<TARGET_FILE:quench_cook>
             -DSOURCES=${Quench_SOURCE_DIR}/examples/demo/assets_src -DSOUNDS=${CMAKE_CURRENT_SOURCE_DIR}/audio/data
             ${_character_args}
             -DOUT=${CMAKE_CURRENT_BINARY_DIR}/tmp/cook_cli -P ${CMAKE_CURRENT_SOURCE_DIR}/CookCli.cmake)
    set_tests_properties(cook_cli PROPERTIES LABELS cook)
    add_dependencies(quench_cook_tests quench_cook)
endif()

quench_add_test_executable(quench_gpu_tests PREFIX gpu LABEL gpu
    SOURCES gpu/ShaderTests.cpp
    LIBS quench_gpu_shaders)
quench_add_shaders(quench_gpu_tests NAME test SOURCE gpu/shaders/test.hlsl
    ENTRIES test_vs:vertex test_fs:fragment test_cs:compute
    INCLUDE_DIRS gpu/shaders/include)
target_compile_definitions(quench_gpu_tests PRIVATE
    QUENCH_TEST_METAL=$<BOOL:${APPLE}>
    QUENCH_TEST_METALLIB=$<BOOL:${QUENCH_SHADER_METAL_OK}>
    QUENCH_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/quench_shaders/test")

if(NOT QUENCH_HEADLESS)
    quench_add_test_executable(quench_app_tests PREFIX app LABEL app
        SOURCES
            app/DisplayHeadroomTests.cpp
            app/InputTests.cpp
            app/OptionsTests.cpp
            app/ScenarioPointTests.cpp
            app/ScenarioTests.cpp
            app/SdlFileSystemTests.cpp
            app/SdlLogTests.cpp
            app/ThermalStateTests.cpp
        LIBS quench_app)

    # The audio device on miniaudio's null backend (no sound), the opt-in default output, the iOS audio session.
    quench_add_test_executable(quench_audio_device_tests PREFIX audio_device LABEL audio
        SOURCES audio/DeviceTests.cpp
        LIBS quench_audio_device)

    # Device tests on the driver SDL picks: Metal on Apple platforms, with the Metal API validation layer.
    set(gpu_environment "")
    if(APPLE)
        set(gpu_environment MTL_DEBUG_LAYER=1 QUENCH_TEST_GPU_DRIVER=metal)
    endif()
    quench_add_test_executable(quench_gpu_device_tests PREFIX gpu_device LABEL gpu
        SOURCES
            gpu/DeviceTests.cpp
            gpu/GpuTest.cpp
            gpu/RayTracingTests.cpp
            gpu/RenderTests.cpp
            gpu/ShaderReloadTests.cpp
            gpu/WindowTests.cpp
        LIBS quench_gpu SDL3::SDL3-static
        ENVIRONMENT ${gpu_environment})
    quench_add_shaders(quench_gpu_device_tests NAME render SOURCE gpu/shaders/render.hlsl
        ENTRIES color_vs:vertex color_fs:fragment fullscreen_vs:vertex uniform_fs:fragment texture_fs:fragment
                rects_vs:vertex scale_cs:compute ray_fs:fragment:rayquery ray_face_fs:fragment:rayquery)
    quench_add_shaders(quench_gpu_device_tests NAME reload_test SOURCE gpu/shaders/reload.hlsl
        ENTRIES reload_vs:vertex reload_fs:fragment)
    target_compile_definitions(quench_gpu_device_tests PRIVATE
        QUENCH_TEST_METALLIB=$<BOOL:${QUENCH_SHADER_METAL_OK}>)
    if(NOT CMAKE_CROSSCOMPILING)
        # The shader hot reload case reads the manifests of this build tree.
        target_compile_definitions(quench_gpu_device_tests PRIVATE QUENCH_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}")
    endif()

    # The same cases on the native Metal backend (src/gpu/metal), with the Metal API validation layer.
    if(APPLE)
        set(metal_native_environment QUENCH_GPU_BACKEND=metal MTL_DEBUG_LAYER=1 QUENCH_TEST_GPU_DRIVER=metal)
        quench_add_test_variant(quench_gpu_device_tests PREFIX gpu_metal_native LABEL gpu
            ENVIRONMENT ${metal_native_environment})
    endif()

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
            quench_add_test_variant(quench_gpu_device_tests PREFIX gpu_vulkan LABEL gpu vulkan
                ENVIRONMENT ${vulkan_environment}
                FAIL_REGEX "Validation (Error|Warning)")
        endif()
    endif()

    # The render module: CPU cases and offscreen renders, on the default driver and on Vulkan through MoltenVK.
    quench_add_test_executable(quench_render_tests PREFIX render LABEL render
        SOURCES
            gpu/GpuTest.cpp
            render/RenderMathTests.cpp
            render/RendererTests.cpp
            render/SceneAssetsTests.cpp
        LIBS quench_render quench_gpu SDL3::SDL3-static
        ENVIRONMENT ${gpu_environment})
    target_include_directories(quench_render_tests PRIVATE gpu)
    quench_add_shaders(quench_render_tests NAME render_test SOURCE render/shaders/custom.hlsl
        ENTRIES surface_vs:vertex surface_fs:fragment surface_shadow_vs:vertex surface_shadow_fs:fragment
                surface_transmittance_fs:fragment)
    if(QUENCH_TEST_VULKAN_LOADER)
        quench_add_test_variant(quench_render_tests PREFIX render_vulkan LABEL render vulkan
            ENVIRONMENT ${vulkan_environment}
            FAIL_REGEX "Validation (Error|Warning)")
    endif()
    if(APPLE)
        quench_add_test_variant(quench_render_tests PREFIX render_metal_native LABEL render
            ENVIRONMENT ${metal_native_environment})
    endif()

    # ---------------------------------------------------------------- examples (host builds)
    if(TARGET quench_hello AND NOT CMAKE_CROSSCOMPILING)
        add_executable(quench_image_compare ${Quench_SOURCE_DIR}/tools/imagecompare/ImageCompare.cpp)
        quench_configure_target(quench_image_compare)
        target_link_libraries(quench_image_compare PRIVATE SDL3::SDL3-static)

        # Golden images: tests/golden/<backend>/<device>/<name>.png; the device folder is named after the GPU.
        if(APPLE)
            execute_process(COMMAND sysctl -n machdep.cpu.brand_string OUTPUT_VARIABLE golden_device
                            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        endif()
        string(TOLOWER "${golden_device}" golden_device)
        string(REGEX REPLACE "[^a-z0-9]+" "-" golden_device "${golden_device}")
        set(QUENCH_GOLDEN_DEVICE "${golden_device}" CACHE STRING
            "Device folder of the golden images (tests/golden/<backend>/<device>)")

        # The apps' checks run offscreen (the default of scripted runs on desktops: a hidden window, no Dock icon, no
        # input from real devices; docs/app.md) and in batches: one app process per GPU backend runs every part of a
        # scenario (`include`), writes each part's capture into its user directory, and a test per part checks the
        # part's lines in the log and compares its capture with the golden image. The batches and the other app runs
        # are registered where the captures are compared (QUENCH_TEST_CAPTURES, Release builds by default); other
        # builds run one smoke run of the demo.
        if(CMAKE_BUILD_TYPE STREQUAL "Release")
            set(captures_default ON)
        else()
            set(captures_default OFF)
        endif()
        option(QUENCH_TEST_CAPTURES "Register the apps' batch runs and golden image checks" ${captures_default})
        set(capture_dir ${CMAKE_CURRENT_BINARY_DIR}/captures)
        # The app runs start after the other tests (CTest starts the costliest first): beside the GPU device and
        # render tests their frames wait for a shared GPU (the Metal batch: 9.6 s beside the headless tests, 21 s
        # beside the GPU tests, 53 s in a full run where it started first; 2026-11-06).
        set(app_run_cost 0.001)

        # quench_add_batch(<prefix> <suffix> APP <target> SCRIPT <file> BACKEND <golden backend> ARGS <arg>...
        #                  [ENVIRONMENT <NAME=value>...] [FAIL_REGEX <regex>] PARTS <file>[:<golden>...]...)
        # registers <prefix>_batch_<suffix> (the run, in captures/<prefix>_batch_<suffix>/) and per part
        # <prefix>_<file>_<suffix>: the part <file>.txt passed its checks and each of its captures <golden>.png
        # matches tests/golden/<backend>/<device>/<golden>.png. All have the label <prefix>, and vulkan for the suffix
        # vulkan.
        function(quench_add_batch prefix suffix)
            cmake_parse_arguments(PARSE_ARGV 2 arg "" "APP;SCRIPT;BACKEND;FAIL_REGEX" "ARGS;ENVIRONMENT;PARTS")
            set(batch ${prefix}_batch_${suffix})
            set(labels ${prefix})
            if(suffix STREQUAL "vulkan")
                list(APPEND labels vulkan)
            endif()
            set(dir ${capture_dir}/${batch})
            list(JOIN arg_ARGS "|" args)
            list(JOIN arg_ENVIRONMENT "|" environment)
            add_test(NAME ${batch} COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:${arg_APP}> "-DARGS=${args}"
                     -DSCRIPT=${arg_SCRIPT} "-DENVIRONMENT=${environment}" -DUSER_DIR=${dir} -DLOG=${dir}.log
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBatch.cmake)
            set_tests_properties(${batch} PROPERTIES FIXTURES_SETUP ${batch} LABELS "${labels}" COST ${app_run_cost})
            if(arg_FAIL_REGEX)
                set_tests_properties(${batch} PROPERTIES FAIL_REGULAR_EXPRESSION "${arg_FAIL_REGEX}")
            endif()
            foreach(part IN LISTS arg_PARTS)
                string(REPLACE ":" ";" part "${part}")
                list(POP_FRONT part file)
                set(capture_args "")
                if(part)
                    set(captures "")
                    set(goldens "")
                    foreach(golden IN LISTS part)
                        list(APPEND captures ${dir}/${golden}.png)
                        list(APPEND goldens
                             ${Quench_SOURCE_DIR}/tests/golden/${arg_BACKEND}/${QUENCH_GOLDEN_DEVICE}/${golden}.png)
                    endforeach()
                    list(JOIN captures "|" captures)
                    list(JOIN goldens "|" goldens)
                    set(capture_args "-DCAPTURE=${captures}" "-DGOLDEN=${goldens}"
                        -DCOMPARE=$<TARGET_FILE:quench_image_compare>)
                endif()
                add_test(NAME ${prefix}_${file}_${suffix} COMMAND ${CMAKE_COMMAND} -DLOG=${dir}.log -DPART=${file}.txt
                         ${capture_args} -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckPart.cmake)
                set_tests_properties(${prefix}_${file}_${suffix} PROPERTIES FIXTURES_REQUIRED ${batch}
                                     LABELS "${labels}" SKIP_REGULAR_EXPRESSION "QUENCH_SKIP")
            endforeach()
        endfunction()

        # The hello example: its frame at 1 s against the golden image, the input check and the perf region check, on
        # SDL GPU's Metal driver, on the native Metal backend (the same golden image) and on Vulkan through MoltenVK.
        if(QUENCH_TEST_CAPTURES)
            set(hello_args --window 640x360 --frame-size 1280x720 --gpu-debug)
            set(hello_parts frame:hello input perf)
            set(hello_script ${Quench_SOURCE_DIR}/examples/hello/scenarios/check.txt)
            quench_add_batch(hello metal APP quench_hello SCRIPT ${hello_script} BACKEND metal ARGS ${hello_args}
                ENVIRONMENT MTL_DEBUG_LAYER=1 PARTS ${hello_parts})
            quench_add_batch(hello metal_native APP quench_hello SCRIPT ${hello_script} BACKEND metal
                ARGS ${hello_args} --gpu-backend metal ENVIRONMENT MTL_DEBUG_LAYER=1 PARTS ${hello_parts})
            if(QUENCH_TEST_VULKAN_LOADER)
                quench_add_batch(hello vulkan APP quench_hello SCRIPT ${hello_script} BACKEND vulkan
                    ARGS ${hello_args} --gpu-driver vulkan ENVIRONMENT ${vulkan_environment}
                    FAIL_REGEX "Validation (Error|Warning)" PARTS ${hello_parts})
            endif()
        endif()
        # Runs without a script start as interactive ones: --offscreen keeps the window and the Dock icon away until
        # the bad flag ends the run.
        add_test(NAME hello_version COMMAND quench_hello --offscreen --version)
        set_tests_properties(hello_version PROPERTIES PASS_REGULAR_EXPRESSION "^Quench Hello ${PROJECT_VERSION}\n$")
        add_test(NAME hello_bad_argument COMMAND quench_hello --offscreen --no-such-option)
        add_test(NAME hello_bad_flag_value COMMAND quench_hello --offscreen --quit-after soon)
        set_tests_properties(hello_bad_argument hello_bad_flag_value PROPERTIES WILL_FAIL TRUE)
        set_tests_properties(hello_version hello_bad_argument hello_bad_flag_value PROPERTIES LABELS hello)

        if(TARGET quench_demo)
            # The engine demo (docs/demo.md, "Checks"): batch.txt (ending in cycle.txt) on SDL GPU's Metal driver and on
            # Vulkan through MoltenVK, batch_native.txt (ray tracing itself instead of its fallbacks, and cycle.txt) on
            # the native Metal backend, which compares with the Metal golden images except for ray tracing's and the
            # tunnel's own (demo_raytracing_native, demo_tunnel_native, demo_tunnel_motion_native: the tunnel in motion,
            # the last part, so that no other capture moves in the frame sequence). The tunnel's path tracer part
            # (tunnel_path) writes its capture without a golden image: 1.3 MB of noise at 256 samples; the render tests
            # check the path tracer's area lights by numbers.
            # A part with a golden name ends in a capture: the sections without the demo's windows, demo_ui and the
            # compact layouts with them.
            set(demo_scenarios ${Quench_SOURCE_DIR}/examples/demo/scenarios)
            set(demo_parts materials:demo_materials shadows:demo_shadows sky:demo_sky post:demo_post hdr:demo_hdr
                hdr_on:demo_hdr_on physics:demo_physics input:demo_input assets:demo_assets audio:demo_audio
                animation:demo_animation animation_blend2d:demo_animation_blend2d animation_lod
                animation_skinning:demo_animation_skinning:demo_animation_skinning_dq
                animation_retarget:demo_animation_retarget animation_blendshapes:demo_animation_blendshapes
                ui:demo_ui compact_portrait:demo_compact_portrait
                compact_landscape:demo_compact_landscape menu audio_tone audio_flyby audio_music audio_reverb
                audio_hrtf)
            set(demo_args --window 960x540 --gpu-debug --hdr-headroom 4)
            if(QUENCH_TEST_CAPTURES)
                quench_add_batch(demo metal APP quench_demo SCRIPT ${demo_scenarios}/batch.txt BACKEND metal
                    ARGS ${demo_args} ENVIRONMENT MTL_DEBUG_LAYER=1
                    PARTS ${demo_parts} raytracing_fallback:demo_raytracing tunnel_fallback:demo_tunnel
                    tunnel_motion_fallback raytracing_maps:demo_raytracing_maps
                    raytracing_screen:demo_raytracing_screen cycle)
                if(APPLE)
                    quench_add_batch(demo metal_native APP quench_demo SCRIPT ${demo_scenarios}/batch_native.txt
                        BACKEND metal ARGS ${demo_args} --gpu-backend metal ENVIRONMENT MTL_DEBUG_LAYER=1
                        PARTS ${demo_parts} raytracing:demo_raytracing_native tunnel:demo_tunnel_native tunnel_path
                        cycle tunnel_motion:demo_tunnel_motion_native)
                endif()
                if(QUENCH_TEST_VULKAN_LOADER)
                    quench_add_batch(demo vulkan APP quench_demo SCRIPT ${demo_scenarios}/batch.txt BACKEND vulkan
                        ARGS ${demo_args} --gpu-driver vulkan ENVIRONMENT ${vulkan_environment}
                        FAIL_REGEX "Validation (Error|Warning)" PARTS ${demo_parts} raytracing_fallback:demo_raytracing
                        tunnel_fallback:demo_tunnel tunnel_motion_fallback raytracing_maps:demo_raytracing_maps
                        raytracing_screen:demo_raytracing_screen cycle)
                endif()
                # The tour (started by its flag), the check mode, the settings and benchmark-all with their files in a
                # fresh user directory (they need runs of their own).
                add_test(NAME demo_tour COMMAND quench_demo --window 960x540 --tour 0.1
                         --script ${demo_scenarios}/tour.txt)
                set_tests_properties(demo_tour PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL line" LABELS demo
                                     COST ${app_run_cost})
                foreach(kind checks settings benchmark)
                    add_test(NAME demo_${kind} COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:quench_demo>
                             -DSCENARIOS=${demo_scenarios} -DUSER_DIR=${CMAKE_CURRENT_BINARY_DIR}/demo_user/${kind}
                             -DKIND=${kind} -P ${CMAKE_CURRENT_SOURCE_DIR}/DemoUserFiles.cmake)
                    set_tests_properties(demo_${kind} PROPERTIES LABELS demo COST ${app_run_cost})
                endforeach()
            else()
                # Every section once, offscreen (with assertions in Debug builds, under the sanitizers in theirs).
                add_test(NAME demo_smoke COMMAND quench_demo --window 960x540 --script ${demo_scenarios}/smoke.txt)
                set_tests_properties(demo_smoke PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL line" LABELS demo
                                     COST ${app_run_cost})
            endif()
            add_test(NAME demo_version COMMAND quench_demo --offscreen --version)
            set_tests_properties(demo_version PROPERTIES PASS_REGULAR_EXPRESSION
                                 "^Quench Demo ${PROJECT_VERSION}\n$")
            add_test(NAME demo_bad_section COMMAND quench_demo --offscreen --section nowhere)
            add_test(NAME demo_bad_flag COMMAND quench_demo --offscreen --tour 2 --checks)
            set_tests_properties(demo_bad_section demo_bad_flag PROPERTIES WILL_FAIL TRUE)
            set_tests_properties(demo_version demo_bad_section demo_bad_flag PROPERTIES LABELS demo)
        endif()
    endif()
endif()

# Every public header compiles on its own and twice in a row (#pragma once).
file(GLOB_RECURSE public_headers CONFIGURE_DEPENDS ${Quench_SOURCE_DIR}/include/quench/*.h)
if(QUENCH_HEADLESS)
    list(FILTER public_headers EXCLUDE REGEX "/quench/(app|render)/")
endif()
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
target_link_libraries(quench_header_check PRIVATE quench_core quench_physics quench_assets quench_audio
                                                  quench_animation quench_gpu_shaders)
if(NOT QUENCH_HEADLESS)
    target_link_libraries(quench_header_check PRIVATE quench_app quench_render)
endif()
