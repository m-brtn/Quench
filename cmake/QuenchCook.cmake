# Build-time asset cooking for the engine and for games: quench_cook_assets().
#
# The host tool `quench_cook` (tools/cook, cmake/CookToolchain.cmake) converts source assets into the formats the
# runtime loads: KTX2 textures with mips (BC7, ASTC 4x4, RGBA8 or Basis Universal), .qmodel models (meshes with levels
# of detail, materials, skins, their textures cooked next to them), .qskel skeletons, .qanim clip sets and .qnav
# navmeshes. Cross builds (iOS) cannot run a tool built for the target: they take a macOS `quench_cook` through
# QUENCH_HOST_COOK (the `host-tools` preset builds it). The web build cooks nothing.

set(QUENCH_HOST_COOK "" CACHE FILEPATH "quench_cook built for the host (required for cross builds that cook assets)")
if(NOT QUENCH_HOST_COOK AND NOT CMAKE_CROSSCOMPILING AND NOT EMSCRIPTEN)
    include(${CMAKE_CURRENT_LIST_DIR}/CookToolchain.cmake)
endif()

# quench_cook_assets(<name> SOURCE_DIR <dir> [OUTPUT_DIR <dir>]
#                    [TEXTURES <file>...] [DATA_TEXTURES <file>...] [MODELS <file>...] [SOUNDS <file>...]
#                    [SKELETONS <file>...] [ANIMATIONS <file>...] [SKELETON <file>] [RETARGET <file>]
#                    [NAVMESHES <file>...]
#                    [FILES <file>...]
#                    [TEXTURE_FORMAT auto|bc7|astc|rgba8|basis] [BASIS_CODEC auto|etc1s|uastc]
#                    [TEXTURE_MAX_SIZE <n>] [MODEL_OPTIONS <quench_cook option>...])
#
# TEXTURE_FORMAT auto (the default) is ASTC 4x4 for iOS builds and BC7 elsewhere; astc is ASTC 4x4; basis is Basis
# Universal, one file for every device, transcoded at load time to what the device samples (docs/assets.md, "Basis
# Universal"). BASIS_CODEC picks its codec: etc1s (small files, lossier), uastc (about BC7's quality) or auto (the
# default: uastc for DATA_TEXTURES and a model's linear textures, etc1s for color).
#
# Files are named relative to SOURCE_DIR and keep that path under OUTPUT_DIR (default
# ${CMAKE_CURRENT_BINARY_DIR}/quench_cooked/<name>)
# with a new extension: TEXTURES (sRGB color) and DATA_TEXTURES (linear: normals, masks) become <path>.ktx2, MODELS
# (glTF, GLB) <path>.qmodel with their textures as <path>_<image>.ktx2. SKELETONS (glTF, GLB) become <path>.qskel
# (the joints of their skins), ANIMATIONS (glTF, GLB) <path>.qanim (their animations as clips, with the settings of
# <path without extension>.anim.json when it exists), for their own skeleton or for SKELETON's (a file under
# SOURCE_DIR). With RETARGET (a bone map under SOURCE_DIR, docs/animation.md "Retargeting") the clips are retargeted
# onto SKELETON's skeleton and become <path>.<SKELETON's name without extension>.qanim. NAVMESHES (glTF, GLB, .qmodel)
# become <path>.qnav (navmeshes of their triangles, with the settings of <path without extension>.nav.json when it
# exists; docs/navigation.md, "Cooking"). SOUNDS (WAV, FLAC, MP3, Ogg
# Vorbis) are decoded to check them and copied with their extension; FILES
# (texts, settings, licences) are copied as they are (a call with FILES only needs no quench_cook). The run-time asset
# path of a file is its path under OUTPUT_DIR. Creates the custom target <name>; quench_add_game(... COOKED_ASSETS
# <name>) copies its output into the game's assets. Every file and everything it reads (glTF buffers and images, clip
# settings) is tracked: a change re-cooks it.
function(quench_cook_assets name)
    cmake_parse_arguments(PARSE_ARGV 1 arg ""
                          "SOURCE_DIR;OUTPUT_DIR;TEXTURE_FORMAT;BASIS_CODEC;TEXTURE_MAX_SIZE;SKELETON;RETARGET"
                          "TEXTURES;DATA_TEXTURES;MODELS;SOUNDS;SKELETONS;ANIMATIONS;NAVMESHES;FILES;MODEL_OPTIONS")
    if(NOT arg_SOURCE_DIR)
        message(FATAL_ERROR "quench_cook_assets: SOURCE_DIR is required")
    endif()
    set(cook "")
    if(NOT (arg_TEXTURES OR arg_DATA_TEXTURES OR arg_MODELS OR arg_SOUNDS OR arg_SKELETONS OR arg_ANIMATIONS OR
            arg_NAVMESHES))
        # FILES only: nothing to cook.
    elseif(QUENCH_HOST_COOK)
        if(NOT EXISTS ${QUENCH_HOST_COOK})
            message(FATAL_ERROR "Quench: QUENCH_HOST_COOK=${QUENCH_HOST_COOK} does not exist. Build it first: "
                                "cmake --preset host-tools && cmake --build --preset host-tools")
        endif()
        set(cook ${QUENCH_HOST_COOK})
    elseif(TARGET quench_cook)
        set(cook quench_cook)
    else()
        message(FATAL_ERROR "Quench: cross builds need QUENCH_HOST_COOK (cmake --preset host-tools && "
                            "cmake --build --preset host-tools)")
    endif()
    get_filename_component(source_dir ${arg_SOURCE_DIR} ABSOLUTE)
    if(arg_OUTPUT_DIR)
        get_filename_component(output_dir ${arg_OUTPUT_DIR} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_BINARY_DIR})
    else()
        set(output_dir ${CMAKE_CURRENT_BINARY_DIR}/quench_cooked/${name})
    endif()
    if(NOT arg_TEXTURE_FORMAT OR arg_TEXTURE_FORMAT STREQUAL "auto")
        if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
            set(arg_TEXTURE_FORMAT astc)
        else()
            set(arg_TEXTURE_FORMAT bc7)
        endif()
    endif()
    if(NOT arg_TEXTURE_FORMAT MATCHES "^(bc7|astc|rgba8|basis)$")
        message(FATAL_ERROR "quench_cook_assets: TEXTURE_FORMAT must be auto, bc7, astc, rgba8 or basis")
    endif()
    set(common --format ${arg_TEXTURE_FORMAT})
    if(arg_BASIS_CODEC)
        if(NOT arg_BASIS_CODEC MATCHES "^(auto|etc1s|uastc)$")
            message(FATAL_ERROR "quench_cook_assets: BASIS_CODEC must be auto, etc1s or uastc")
        endif()
        if(NOT arg_TEXTURE_FORMAT STREQUAL "basis")
            message(FATAL_ERROR "quench_cook_assets: BASIS_CODEC needs TEXTURE_FORMAT basis")
        endif()
        list(APPEND common --basis-codec ${arg_BASIS_CODEC})
    endif()
    if(arg_TEXTURE_MAX_SIZE)
        list(APPEND common --max-size ${arg_TEXTURE_MAX_SIZE})
    endif()
    set(dep_dir ${CMAKE_CURRENT_BINARY_DIR}/quench_cook/${name})
    file(MAKE_DIRECTORY ${output_dir} ${dep_dir})

    set(skeleton_args "")
    set(skeleton_depends "")
    if(arg_SKELETON)
        if(IS_ABSOLUTE ${arg_SKELETON} OR arg_SKELETON MATCHES "(^|/)\\.\\.(/|$)")
            message(FATAL_ERROR "quench_cook_assets: SKELETON '${arg_SKELETON}' must be relative to SOURCE_DIR")
        endif()
        set(skeleton_args --skeleton ${source_dir}/${arg_SKELETON})
        set(skeleton_depends ${source_dir}/${arg_SKELETON})
    endif()
    set(animation_suffix "")
    if(arg_RETARGET)
        if(NOT arg_SKELETON)
            message(FATAL_ERROR "quench_cook_assets: RETARGET needs SKELETON (the skeleton the clips are retargeted onto)")
        endif()
        if(IS_ABSOLUTE ${arg_RETARGET} OR arg_RETARGET MATCHES "(^|/)\\.\\.(/|$)")
            message(FATAL_ERROR "quench_cook_assets: RETARGET '${arg_RETARGET}' must be relative to SOURCE_DIR")
        endif()
        list(APPEND skeleton_args --retarget ${source_dir}/${arg_RETARGET})
        list(APPEND skeleton_depends ${source_dir}/${arg_RETARGET})
        get_filename_component(target_name ${arg_SKELETON} NAME_WLE)
        set(animation_suffix .${target_name})
    endif()

    set(outputs "")
    foreach(kind TEXTURES DATA_TEXTURES MODELS SOUNDS SKELETONS ANIMATIONS NAVMESHES)
        foreach(file IN LISTS arg_${kind})
            if(IS_ABSOLUTE ${file} OR file MATCHES "(^|/)\\.\\.(/|$)")
                message(FATAL_ERROR "quench_cook_assets: '${file}' must be relative to SOURCE_DIR, without '..'")
            endif()
            set(input ${source_dir}/${file})
            set(depends ${input})
            cmake_path(REMOVE_EXTENSION file LAST_ONLY OUTPUT_VARIABLE stem)
            string(MAKE_C_IDENTIFIER ${kind}_${file} dep_name)
            if(kind STREQUAL "SOUNDS")
                set(output ${output_dir}/${file})
                set(args sound ${input} ${output})
            elseif(kind STREQUAL "MODELS")
                set(output ${output_dir}/${stem}.qmodel)
                set(args model ${input} ${output} --asset-root ${output_dir} ${common} ${arg_MODEL_OPTIONS})
            elseif(kind STREQUAL "SKELETONS")
                set(output ${output_dir}/${stem}.qskel)
                set(args skeleton ${input} ${output})
            elseif(kind STREQUAL "NAVMESHES")
                set(output ${output_dir}/${stem}.qnav)
                set(args navmesh ${input} ${output})
            elseif(kind STREQUAL "ANIMATIONS")
                set(output ${output_dir}/${stem}${animation_suffix}.qanim)
                set(args animation ${input} ${output} ${skeleton_args})
                list(APPEND depends ${skeleton_depends})
            else()
                set(output ${output_dir}/${stem}.ktx2)
                set(args texture ${input} ${output} ${common})
                if(kind STREQUAL "DATA_TEXTURES")
                    list(APPEND args --linear)
                endif()
            endif()
            add_custom_command(
                OUTPUT ${output}
                COMMAND ${cook} ${args} --depfile ${dep_dir}/${dep_name}.d
                DEPENDS ${depends} ${cook}
                DEPFILE ${dep_dir}/${dep_name}.d
                COMMENT "Cooking ${file}"
                VERBATIM)
            list(APPEND outputs ${output})
        endforeach()
    endforeach()
    foreach(file IN LISTS arg_FILES)
        if(IS_ABSOLUTE ${file} OR file MATCHES "(^|/)\\.\\.(/|$)")
            message(FATAL_ERROR "quench_cook_assets: '${file}' must be relative to SOURCE_DIR, without '..'")
        endif()
        get_filename_component(file_dir ${output_dir}/${file} DIRECTORY)
        file(MAKE_DIRECTORY ${file_dir})
        add_custom_command(
            OUTPUT ${output_dir}/${file}
            COMMAND ${CMAKE_COMMAND} -E copy ${source_dir}/${file} ${output_dir}/${file}
            DEPENDS ${source_dir}/${file}
            COMMENT "Copying ${file}"
            VERBATIM)
        list(APPEND outputs ${output_dir}/${file})
    endforeach()
    add_custom_target(${name} ALL DEPENDS ${outputs})
    set_target_properties(${name} PROPERTIES QUENCH_COOK_OUTPUT_DIR ${output_dir})
endfunction()

# quench_weather_sounds(<name>): the engine's default weather sounds (audio::WeatherAudio; docs/audio.md, "Weather
# sounds"), written at build time by tools/assets/make_weather_sounds.py (procedural, needs Python 3) and cooked like
# any sound as quench/weather/rain_light.wav, rain_heavy.wav, rain_far.wav, rain_cover.wav, wind_light.wav,
# wind_strong.wav, thunder_near.wav and thunder_far.wav. A game that plays them
# lists <name> in quench_add_game(... COOKED_ASSETS); one with its own sounds names them instead.
# A cache variable: the function is called from a game's directories, which do not see this file's scope.
set(_QUENCH_COOK_TOOLS_DIR ${CMAKE_CURRENT_LIST_DIR}/../tools CACHE INTERNAL "")
function(quench_weather_sounds name)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(script ${_QUENCH_COOK_TOOLS_DIR}/assets/make_weather_sounds.py)
    set(source_dir ${CMAKE_CURRENT_BINARY_DIR}/quench_weather_sounds/${name})
    set(sounds rain_light.wav rain_heavy.wav rain_far.wav rain_cover.wav wind_light.wav wind_strong.wav thunder_near.wav
               thunder_far.wav)
    list(TRANSFORM sounds PREPEND ${source_dir}/quench/weather/ OUTPUT_VARIABLE written)
    add_custom_command(
        OUTPUT ${written}
        COMMAND ${Python3_EXECUTABLE} ${script} ${source_dir}/quench/weather
        DEPENDS ${script}
        COMMENT "Writing the default weather sounds"
        VERBATIM)
    list(TRANSFORM sounds PREPEND quench/weather/)
    quench_cook_assets(${name} SOURCE_DIR ${source_dir} SOUNDS ${sounds})
endfunction()
