# The Assets section's sources (assets_src, procedural: tools/assets/make_demo_assets.py), cooked at build time.
quench_cook_assets(quench_demo_cooked
    SOURCE_DIR assets_src
    TEXTURES
        tiles/tile_0.png tiles/tile_1.png tiles/tile_2.png tiles/tile_3.png
        tiles/tile_4.png tiles/tile_5.png tiles/tile_6.png tiles/tile_7.png
    MODELS
        models/ball.gltf models/crate.gltf models/pillar.glb)

# The Audio section's sounds: procedural WAV files written at build time (tools/assets/make_demo_sounds.py), checked
# and copied by quench_cook into the demo's assets/sounds.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(demo_sounds motor.wav siren.wav ping.wav step.wav music_a.wav music_b.wav)
set(demo_sounds_dir ${CMAKE_CURRENT_BINARY_DIR}/sounds_src)
list(TRANSFORM demo_sounds PREPEND ${demo_sounds_dir}/sounds/ OUTPUT_VARIABLE demo_sound_files)
add_custom_command(
    OUTPUT ${demo_sound_files}
    COMMAND ${Python3_EXECUTABLE} ${Quench_SOURCE_DIR}/tools/assets/make_demo_sounds.py ${demo_sounds_dir}/sounds
    DEPENDS ${Quench_SOURCE_DIR}/tools/assets/make_demo_sounds.py
    COMMENT "Writing the demo's sounds"
    VERBATIM)
list(TRANSFORM demo_sounds PREPEND sounds/)
quench_cook_assets(quench_demo_sounds SOURCE_DIR ${demo_sounds_dir} SOUNDS ${demo_sounds})

# The Animation section's character: a procedural glTF with its clips and their settings, written at build time
# (tools/assets/make_demo_character.py) and cooked into a skinned model, a skeleton and a clip set.
set(demo_character_dir ${CMAKE_CURRENT_BINARY_DIR}/character_src)
set(demo_character_files mannequin.gltf mannequin.bin mannequin.anim.json)
list(TRANSFORM demo_character_files PREPEND ${demo_character_dir}/characters/)
add_custom_command(
    OUTPUT ${demo_character_files}
    COMMAND ${Python3_EXECUTABLE} ${Quench_SOURCE_DIR}/tools/assets/make_demo_character.py
            ${demo_character_dir}/characters
    DEPENDS ${Quench_SOURCE_DIR}/tools/assets/make_demo_character.py
    COMMENT "Writing the demo's character"
    VERBATIM)
quench_cook_assets(quench_demo_character SOURCE_DIR ${demo_character_dir}
    MODELS characters/mannequin.gltf
    SKELETONS characters/mannequin.gltf
    ANIMATIONS characters/mannequin.gltf)
# The retargeting pair's second character (tools/assets/make_demo_strider.py): its model and skeleton, and the
# mannequin's clips retargeted onto it through its bone map (characters/mannequin.strider.qanim).
set(demo_strider_files strider.gltf strider.bin strider.retarget.json)
list(TRANSFORM demo_strider_files PREPEND ${demo_character_dir}/characters/)
add_custom_command(
    OUTPUT ${demo_strider_files}
    COMMAND ${Python3_EXECUTABLE} ${Quench_SOURCE_DIR}/tools/assets/make_demo_strider.py
            ${demo_character_dir}/characters
    DEPENDS ${Quench_SOURCE_DIR}/tools/assets/make_demo_strider.py
            ${Quench_SOURCE_DIR}/tools/assets/make_demo_character.py
    COMMENT "Writing the demo's second character"
    VERBATIM)
quench_cook_assets(quench_demo_strider SOURCE_DIR ${demo_character_dir}
    MODELS characters/strider.gltf
    SKELETONS characters/strider.gltf)
quench_cook_assets(quench_demo_retargeted SOURCE_DIR ${demo_character_dir}
    ANIMATIONS characters/mannequin.gltf
    SKELETON characters/strider.gltf
    RETARGET characters/strider.retarget.json)
# The blend shape creature (tools/assets/make_demo_blob.py): a skinned model with four blend shapes, its skeleton and
# its clip.
set(demo_blob_files blob.gltf blob.bin blob.anim.json)
list(TRANSFORM demo_blob_files PREPEND ${demo_character_dir}/characters/)
add_custom_command(
    OUTPUT ${demo_blob_files}
    COMMAND ${Python3_EXECUTABLE} ${Quench_SOURCE_DIR}/tools/assets/make_demo_blob.py ${demo_character_dir}/characters
    DEPENDS ${Quench_SOURCE_DIR}/tools/assets/make_demo_blob.py
            ${Quench_SOURCE_DIR}/tools/assets/make_demo_character.py
    COMMENT "Writing the demo's blend shape creature"
    VERBATIM)
quench_cook_assets(quench_demo_blob SOURCE_DIR ${demo_character_dir}
    MODELS characters/blob.gltf
    SKELETONS characters/blob.gltf
    ANIMATIONS characters/blob.gltf)

# The Tunnel section's textures: procedural PNG files written at build time (tools/assets/make_demo_tunnel.py) and
# cooked into KTX2: colors in sRGB, normal and metallic-roughness maps linear.
set(demo_tunnel_dir ${CMAKE_CURRENT_BINARY_DIR}/tunnel_src)
set(demo_tunnel_colors panel_color.png asphalt_color.png puddle.png)
set(demo_tunnel_data panel_normal.png panel_mr.png asphalt_normal.png asphalt_mr.png plastic_normal.png
    rubber_normal.png brushed_mr.png)
list(TRANSFORM demo_tunnel_colors PREPEND tunnel/)
list(TRANSFORM demo_tunnel_data PREPEND tunnel/)
set(demo_tunnel_files ${demo_tunnel_colors} ${demo_tunnel_data})
list(TRANSFORM demo_tunnel_files PREPEND ${demo_tunnel_dir}/)
add_custom_command(
    OUTPUT ${demo_tunnel_files}
    COMMAND ${Python3_EXECUTABLE} ${Quench_SOURCE_DIR}/tools/assets/make_demo_tunnel.py ${demo_tunnel_dir}/tunnel
    DEPENDS ${Quench_SOURCE_DIR}/tools/assets/make_demo_tunnel.py
    COMMENT "Writing the demo's tunnel textures"
    VERBATIM)
quench_cook_assets(quench_demo_tunnel SOURCE_DIR ${demo_tunnel_dir}
    TEXTURES ${demo_tunnel_colors}
    DATA_TEXTURES ${demo_tunnel_data})

# The licences the About panel shows, copied into the demo's assets.
quench_cook_assets(quench_demo_licences SOURCE_DIR ${Quench_SOURCE_DIR} FILES LICENSE THIRD_PARTY_LICENSES)

# The commit the About panel shows: demo_build_info.h, written at build time and only when the commit changes.
set(demo_build_info_dir ${CMAKE_CURRENT_BINARY_DIR}/build_info)
add_custom_target(quench_demo_build_info
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${Quench_SOURCE_DIR} -DOUTPUT=${demo_build_info_dir}/demo_build_info.h
            -P ${CMAKE_CURRENT_SOURCE_DIR}/BuildInfo.cmake
    BYPRODUCTS ${demo_build_info_dir}/demo_build_info.h
    COMMENT "Checking the demo's build info"
    VERBATIM)

quench_add_game(quench_demo
    NAME "Quench Demo"
    OUTPUT_NAME quench-demo
    BUNDLE_ID org.quench.demo
    ICON icon/icon.png
    ORIENTATIONS all
    ASSETS assets
    COOKED_ASSETS quench_demo_cooked quench_demo_sounds quench_demo_character quench_demo_strider
                  quench_demo_retargeted quench_demo_blob quench_demo_tunnel quench_demo_licences
    SOURCES
        Camera.cpp
        Checks.cpp
        Common.cpp
        Demo.cpp
        GraphicsPanel.cpp
        Modes.cpp
        Settings.cpp
        Ui.cpp
        sections/AnimationSection.cpp
        sections/AssetsSection.cpp
        sections/AudioSection.cpp
        sections/HdrSection.cpp
        sections/InputSection.cpp
        sections/MaterialsSection.cpp
        sections/PhysicsSection.cpp
        sections/PostSection.cpp
        sections/RayTracingSection.cpp
        sections/ShadowsSection.cpp
        sections/SkySection.cpp
        sections/TunnelSection.cpp)
quench_configure_target(quench_demo)
target_link_libraries(quench_demo PRIVATE quench_render quench_physics quench_animation)
target_include_directories(quench_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${demo_build_info_dir})
target_compile_definitions(quench_demo PRIVATE QUENCH_DEMO_CONFIG="$<CONFIG>")
add_dependencies(quench_demo quench_demo_build_info)
quench_add_shaders(quench_demo NAME demo_checker SOURCE shaders/checker.hlsl
    ENTRIES surface_vs:vertex surface_fs:fragment surface_basic_fs:fragment surface_local_shadows_fs:fragment)
quench_add_shaders(quench_demo NAME demo_markers SOURCE shaders/markers.hlsl ENTRIES marker_vs:vertex marker_fs:fragment)
