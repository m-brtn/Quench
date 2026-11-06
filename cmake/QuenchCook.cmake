# Build-time asset cooking for the engine and for games: quench_cook_assets().
#
# The host tool `quench_cook` (tools/cook, cmake/CookToolchain.cmake) converts source assets into the formats the
# runtime loads: KTX2 textures with mips (BC7 or RGBA8), .qmodel models (meshes with levels of detail, materials, skins,
# their textures cooked next to them), .qskel skeletons and .qanim clip sets. Cross builds (iOS) cannot run a tool
# built for the target: they take a macOS `quench_cook` through QUENCH_HOST_COOK (the `host-tools` preset builds it).
# The web build cooks nothing.

set(QUENCH_HOST_COOK "" CACHE FILEPATH "quench_cook built for the host (required for cross builds that cook assets)")
if(NOT QUENCH_HOST_COOK AND NOT CMAKE_CROSSCOMPILING AND NOT EMSCRIPTEN)
    include(${CMAKE_CURRENT_LIST_DIR}/CookToolchain.cmake)
endif()

# quench_cook_assets(<name> SOURCE_DIR <dir> [OUTPUT_DIR <dir>]
#                    [TEXTURES <file>...] [DATA_TEXTURES <file>...] [MODELS <file>...] [SOUNDS <file>...]
#                    [SKELETONS <file>...] [ANIMATIONS <file>...] [SKELETON <file>]
#                    [TEXTURE_FORMAT bc7|rgba8] [TEXTURE_MAX_SIZE <n>] [MODEL_OPTIONS <quench_cook option>...])
#
# Files are named relative to SOURCE_DIR and keep that path under OUTPUT_DIR (default
# ${CMAKE_CURRENT_BINARY_DIR}/quench_cooked/<name>)
# with a new extension: TEXTURES (sRGB color) and DATA_TEXTURES (linear: normals, masks) become <path>.ktx2, MODELS
# (glTF, GLB) <path>.qmodel with their textures as <path>_<image>.ktx2. SKELETONS (glTF, GLB) become <path>.qskel
# (the joints of their skins), ANIMATIONS (glTF, GLB) <path>.qanim (their animations as clips, with the settings of
# <path without extension>.anim.json when it exists), for their own skeleton or for SKELETON's (a file under
# SOURCE_DIR). SOUNDS (WAV, FLAC, MP3) are decoded to check them and copied with their extension. The run-time asset
# path of a file is its path under OUTPUT_DIR. Creates the custom target <name>; quench_add_game(... COOKED_ASSETS
# <name>) copies its output into the game's assets. Every file and everything it reads (glTF buffers and images, clip
# settings) is tracked: a change re-cooks it.
function(quench_cook_assets name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "SOURCE_DIR;OUTPUT_DIR;TEXTURE_FORMAT;TEXTURE_MAX_SIZE;SKELETON"
                          "TEXTURES;DATA_TEXTURES;MODELS;SOUNDS;SKELETONS;ANIMATIONS;MODEL_OPTIONS")
    if(NOT arg_SOURCE_DIR)
        message(FATAL_ERROR "quench_cook_assets: SOURCE_DIR is required")
    endif()
    if(QUENCH_HOST_COOK)
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
    if(NOT arg_TEXTURE_FORMAT)
        set(arg_TEXTURE_FORMAT bc7)
    endif()
    if(NOT arg_TEXTURE_FORMAT MATCHES "^(bc7|rgba8)$")
        message(FATAL_ERROR "quench_cook_assets: TEXTURE_FORMAT must be bc7 or rgba8")
    endif()
    set(common --format ${arg_TEXTURE_FORMAT})
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

    set(outputs "")
    foreach(kind TEXTURES DATA_TEXTURES MODELS SOUNDS SKELETONS ANIMATIONS)
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
            elseif(kind STREQUAL "ANIMATIONS")
                set(output ${output_dir}/${stem}.qanim)
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
    add_custom_target(${name} ALL DEPENDS ${outputs})
    set_target_properties(${name} PROPERTIES QUENCH_COOK_OUTPUT_DIR ${output_dir})
endfunction()
