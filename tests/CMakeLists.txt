# The audio tests' decoder files (audio/data), embedded so that the device test app has them too.
set(audio_test_data ${CMAKE_CURRENT_BINARY_DIR}/AudioTestData.cpp)
set(audio_test_data_content "#include <cstdint>\n#include <span>\n\nnamespace quench::test {\n")
foreach(entry tone.flac:toneFlac tone.mp3:toneMp3)
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
            gpu/DeviceTests.cpp
            gpu/GpuTest.cpp
            gpu/RenderTests.cpp
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
                rects_vs:vertex scale_cs:compute)
    quench_add_shaders(quench_device_tests NAME render_test SOURCE render/shaders/custom.hlsl
        ENTRIES surface_vs:vertex surface_fs:fragment surface_shadow_vs:vertex surface_shadow_fs:fragment
                surface_transmittance_fs:fragment)
    return()
endif()

add_library(quench_test_harness STATIC harness/TestMain.cpp)
quench_configure_target(quench_test_harness)
target_include_directories(quench_test_harness PUBLIC harness)
target_compile_definitions(quench_test_harness PRIVATE QUENCH_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

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
        core/JobsTests.cpp
        core/JsonTests.cpp
        core/LogTests.cpp
        core/ResultTests.cpp
        core/ThreadTests.cpp
    LIBS quench_core)

quench_add_test_executable(quench_physics_tests PREFIX physics
    SOURCES physics/PhysicsWorldTests.cpp
    LIBS quench_physics)

quench_add_test_executable(quench_assets_tests PREFIX assets
    SOURCES
        assets/AssetManagerTests.cpp
        assets/ModelTests.cpp
        assets/StreamingGridTests.cpp
        assets/TextureTests.cpp
    LIBS quench_assets)

quench_add_test_executable(quench_audio_tests PREFIX audio
    SOURCES audio/AudioTests.cpp ${audio_test_data}
    LIBS quench_audio)

quench_add_test_executable(quench_animation_tests PREFIX animation
    SOURCES animation/AnimationTests.cpp
    LIBS quench_animation)

# quench_cook (host builds that build the cook themselves): the library's cases, and the command line run twice on the
# demo's sources and its generated character with byte-identical outputs.
if(TARGET quench_cook_lib)
    quench_add_test_executable(quench_cook_tests PREFIX cook
        SOURCES cook/CookTests.cpp
        LIBS quench_cook_lib quench_glm)
    target_compile_definitions(quench_cook_tests PRIVATE QUENCH_TEST_SOURCE_DIR="${Quench_SOURCE_DIR}")
    find_package(Python3 COMPONENTS Interpreter)
    set(_character_args)
    if(Python3_Interpreter_FOUND)
        set(_character_args -DPYTHON=${Python3_EXECUTABLE}
            -DCHARACTER=${Quench_SOURCE_DIR}/tools/assets/make_demo_character.py)
    endif()
    add_test(NAME cook_cli COMMAND ${CMAKE_COMMAND} -DCOOK=$<TARGET_FILE:quench_cook>
             -DSOURCES=${Quench_SOURCE_DIR}/examples/demo/assets_src -DSOUNDS=${CMAKE_CURRENT_SOURCE_DIR}/audio/data
             ${_character_args}
             -DOUT=${CMAKE_CURRENT_BINARY_DIR}/tmp/cook_cli -P ${CMAKE_CURRENT_SOURCE_DIR}/CookCli.cmake)
    add_dependencies(quench_cook_tests quench_cook)
endif()

quench_add_test_executable(quench_gpu_tests PREFIX gpu
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
    quench_add_test_executable(quench_app_tests PREFIX app
        SOURCES
            app/DisplayHeadroomTests.cpp
            app/InputTests.cpp
            app/OptionsTests.cpp
            app/ScenarioPointTests.cpp
            app/ScenarioTests.cpp
            app/SdlFileSystemTests.cpp
            app/SdlLogTests.cpp
        LIBS quench_app)

    # The audio device on miniaudio's null backend (no sound), the opt-in default output, the iOS audio session.
    quench_add_test_executable(quench_audio_device_tests PREFIX audio_device
        SOURCES audio/DeviceTests.cpp
        LIBS quench_audio_device)

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
                rects_vs:vertex scale_cs:compute)
    target_compile_definitions(quench_gpu_device_tests PRIVATE
        QUENCH_TEST_METALLIB=$<BOOL:${QUENCH_SHADER_METAL_OK}>)

    # The same cases on the native Metal backend (src/gpu/metal), with the Metal API validation layer.
    if(APPLE)
        set(metal_native_environment QUENCH_GPU_BACKEND=metal MTL_DEBUG_LAYER=1 QUENCH_TEST_GPU_DRIVER=metal)
        quench_add_test_variant(quench_gpu_device_tests PREFIX gpu_metal_native
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
            quench_add_test_variant(quench_gpu_device_tests PREFIX gpu_vulkan
                ENVIRONMENT ${vulkan_environment}
                FAIL_REGEX "Validation (Error|Warning)")
        endif()
    endif()

    # The render module: CPU cases and offscreen renders, on the default driver and on Vulkan through MoltenVK.
    quench_add_test_executable(quench_render_tests PREFIX render
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
        quench_add_test_variant(quench_render_tests PREFIX render_vulkan
            ENVIRONMENT ${vulkan_environment}
            FAIL_REGEX "Validation (Error|Warning)")
    endif()
    if(APPLE)
        quench_add_test_variant(quench_render_tests PREFIX render_metal_native ENVIRONMENT ${metal_native_environment})
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

        set(hello_args --window 640x360 --frame-size 1280x720 --capture-at 1 --quit-after 1 --gpu-debug
            --audio-device null)
        set(capture_dir ${CMAKE_CURRENT_BINARY_DIR}/captures)
        # quench_add_capture_test(<test> IMAGE <golden name> BACKEND <backend> APP <target> ARGS <arg>...
        #                         [ENVIRONMENT <NAME=value>...] [OUTPUTS <file>...])
        function(quench_add_capture_test name)
            cmake_parse_arguments(PARSE_ARGV 1 arg "" "IMAGE;BACKEND;APP" "ARGS;ENVIRONMENT;OUTPUTS")
            list(JOIN arg_ARGS "|" args)
            list(JOIN arg_ENVIRONMENT "|" environment)
            list(JOIN arg_OUTPUTS "|" outputs)
            add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:${arg_APP}> "-DARGS=${args}"
                     "-DENVIRONMENT=${environment}" "-DOUTPUTS=${outputs}" -DCAPTURE=${capture_dir}/${name}.png
                     -DGOLDEN=${Quench_SOURCE_DIR}/tests/golden/${arg_BACKEND}/${QUENCH_GOLDEN_DEVICE}/${arg_IMAGE}.png
                     -DCOMPARE=$<TARGET_FILE:quench_image_compare> -P ${CMAKE_CURRENT_SOURCE_DIR}/RunCapture.cmake)
            set_tests_properties(${name} PROPERTIES SKIP_REGULAR_EXPRESSION "QUENCH_SKIP"
                                 FAIL_REGULAR_EXPRESSION "Validation (Error|Warning)")
        endfunction()

        quench_add_capture_test(hello_metal IMAGE hello BACKEND metal APP quench_hello ARGS ${hello_args}
            ENVIRONMENT MTL_DEBUG_LAYER=1)
        # The native Metal backend against the same golden images as SDL GPU's Metal driver.
        quench_add_capture_test(hello_metal_native IMAGE hello BACKEND metal APP quench_hello
            ARGS ${hello_args} --gpu-backend metal ENVIRONMENT MTL_DEBUG_LAYER=1)
        if(QUENCH_TEST_VULKAN_LOADER)
            quench_add_capture_test(hello_vulkan IMAGE hello BACKEND vulkan APP quench_hello ARGS ${hello_args} --gpu-driver vulkan
                ENVIRONMENT ${vulkan_environment})
        endif()

        add_test(NAME hello_input COMMAND quench_hello --window 640x360 --audio-device null --script
                 ${Quench_SOURCE_DIR}/examples/hello/scenarios/input.txt)
        set_tests_properties(hello_input PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL line")
        add_test(NAME hello_version COMMAND quench_hello --version)
        set_tests_properties(hello_version PROPERTIES PASS_REGULAR_EXPRESSION "^Quench Hello ${PROJECT_VERSION}\n$")
        add_test(NAME hello_bad_argument COMMAND quench_hello --no-such-option)
        add_test(NAME hello_bad_flag_value COMMAND quench_hello --quit-after soon)
        set_tests_properties(hello_bad_argument hello_bad_flag_value PROPERTIES WILL_FAIL TRUE)

        if(TARGET quench_demo)
            # The engine demo: per section a scripted check that ends in a golden capture (section:capture time).
            set(demo_scenarios ${Quench_SOURCE_DIR}/examples/demo/scenarios)
            # No test plays sound: the null audio device (a thread that discards the mix), or for the audio section
            # the offline mix written as WAV next to the capture. Immediate present: the deterministic frames are held
            # to their own cap (120 per second) instead of the display's rate where the window is not paced to the
            # display (docs/platforms.md), with the same pixels and the same mix; the hello checks and demo_menu keep
            # the default vsync. The native Metal backend runs a subset, for the wall time: the animation section (compute
            # skinning, shadows, TAA, bloom and the UI) and demo_hdr_on below (HDR output); render_metal_native covers
            # the frame's modules one by one, and every section matched its golden image when the backend was added
            # (docs/architecture.md, "Native Metal backend").
            set(demo_native_sections animation)
            foreach(entry materials:2 shadows:2 sky:2 post:2 hdr:2 physics:2 input:3 assets:1.5 audio:1.65
                  animation:2.4)
                string(REPLACE ":" ";" entry "${entry}")
                list(GET entry 0 section)
                list(GET entry 1 capture_at)
                set(demo_args --window 960x540 --frame-size 960x540 --gpu-debug --present-mode immediate
                    --section ${section} --script ${demo_scenarios}/${section}.txt --capture-at ${capture_at})
                foreach(backend metal vulkan)
                    if(section STREQUAL "audio")
                        set(demo_audio_${backend} --audio-capture ${capture_dir}/demo_audio_${backend}.wav)
                        set(demo_outputs_${backend} ${capture_dir}/demo_audio_${backend}.wav)
                    else()
                        set(demo_audio_${backend} --audio-device null)
                        set(demo_outputs_${backend} "")
                    endif()
                endforeach()
                quench_add_capture_test(demo_${section}_metal IMAGE demo_${section} BACKEND metal APP quench_demo
                    ARGS ${demo_args} ${demo_audio_metal} ENVIRONMENT MTL_DEBUG_LAYER=1
                    OUTPUTS ${demo_outputs_metal})
                if(section IN_LIST demo_native_sections)
                    quench_add_capture_test(demo_${section}_metal_native IMAGE demo_${section} BACKEND metal
                        APP quench_demo ARGS ${demo_args} --audio-device null --gpu-backend metal
                        ENVIRONMENT MTL_DEBUG_LAYER=1)
                endif()
                if(QUENCH_TEST_VULKAN_LOADER)
                    quench_add_capture_test(demo_${section}_vulkan IMAGE demo_${section} BACKEND vulkan
                        APP quench_demo ARGS ${demo_args} ${demo_audio_vulkan} --gpu-driver vulkan
                        ENVIRONMENT ${vulkan_environment} OUTPUTS ${demo_outputs_vulkan})
                endif()
            endforeach()
            # The audio section's checks on the mix itself, split so that they run in parallel, on the default
            # backend only: the offline mix does not depend on the GPU backend, which demo_audio_wav_match checks
            # on the two backends' WAV of the section's capture run.
            file(MAKE_DIRECTORY ${capture_dir})
            foreach(part tone flyby music)
                add_test(NAME demo_audio_${part} COMMAND quench_demo --window 960x540 --frame-size 960x540
                         --section audio --present-mode immediate --script ${demo_scenarios}/audio_${part}.txt
                         --audio-capture ${capture_dir}/demo_audio_${part}.wav)
                set_tests_properties(demo_audio_${part} PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL line")
            endforeach()
            if(QUENCH_TEST_VULKAN_LOADER)
                add_test(NAME demo_audio_wav_match COMMAND ${CMAKE_COMMAND} -E compare_files
                         ${capture_dir}/demo_audio_metal.wav ${capture_dir}/demo_audio_vulkan.wav)
                set_tests_properties(demo_audio_metal demo_audio_vulkan PROPERTIES FIXTURES_SETUP demo_audio_wav)
                set_tests_properties(demo_audio_wav_match PROPERTIES FIXTURES_REQUIRED demo_audio_wav)
            endif()
            # An HDR extended-linear frame with a fixed headroom; the capture is the frame clipped at paper white.
            set(demo_hdr_args --window 960x540 --frame-size 960x540 --gpu-debug --present-mode immediate --section hdr
                --hdr on --hdr-headroom 4 --script ${demo_scenarios}/hdr_on.txt --capture-at 2 --audio-device null)
            quench_add_capture_test(demo_hdr_on_metal IMAGE demo_hdr_on BACKEND metal APP quench_demo
                ARGS ${demo_hdr_args} ENVIRONMENT MTL_DEBUG_LAYER=1)
            quench_add_capture_test(demo_hdr_on_metal_native IMAGE demo_hdr_on BACKEND metal APP quench_demo
                ARGS ${demo_hdr_args} --gpu-backend metal ENVIRONMENT MTL_DEBUG_LAYER=1)
            if(QUENCH_TEST_VULKAN_LOADER)
                quench_add_capture_test(demo_hdr_on_vulkan IMAGE demo_hdr_on BACKEND vulkan APP quench_demo
                    ARGS ${demo_hdr_args} --gpu-driver vulkan ENVIRONMENT ${vulkan_environment})
            endif()
            add_test(NAME demo_menu COMMAND quench_demo --window 960x540 --audio-device null
                     --script ${demo_scenarios}/menu.txt)
            set_tests_properties(demo_menu PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL line")
            add_test(NAME demo_version COMMAND quench_demo --version)
            set_tests_properties(demo_version PROPERTIES PASS_REGULAR_EXPRESSION
                                 "^Quench Demo ${PROJECT_VERSION}\n$")
            add_test(NAME demo_bad_section COMMAND quench_demo --section nowhere)
            set_tests_properties(demo_bad_section PROPERTIES WILL_FAIL TRUE)
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
