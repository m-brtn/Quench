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

quench_add_game(quench_demo
    NAME "Quench Demo"
    OUTPUT_NAME quench-demo
    BUNDLE_ID org.quench.demo
    ICON icon/icon.png
    ORIENTATIONS all
    ASSETS assets
    COOKED_ASSETS quench_demo_cooked quench_demo_sounds quench_demo_character
    SOURCES
        AnimationSection.cpp
        AssetsSection.cpp
        AudioSection.cpp
        Common.cpp
        Demo.cpp
        HdrSection.cpp
        InputSection.cpp
        MaterialsSection.cpp
        PhysicsSection.cpp
        PostSection.cpp
        ShadowsSection.cpp
        SkySection.cpp)
quench_configure_target(quench_demo)
target_link_libraries(quench_demo PRIVATE quench_render quench_physics quench_animation)
quench_add_shaders(quench_demo NAME demo_checker SOURCE shaders/checker.hlsl ENTRIES surface_vs:vertex surface_fs:fragment)
quench_add_shaders(quench_demo NAME demo_markers SOURCE shaders/markers.hlsl ENTRIES marker_vs:vertex marker_fs:fragment)
