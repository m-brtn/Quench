# The Assets section's sources (assets_src, procedural: tools/assets/make_demo_assets.py), cooked at build time.
quench_cook_assets(quench_demo_cooked
    SOURCE_DIR assets_src
    TEXTURES
        tiles/tile_0.png tiles/tile_1.png tiles/tile_2.png tiles/tile_3.png
        tiles/tile_4.png tiles/tile_5.png tiles/tile_6.png tiles/tile_7.png
    MODELS
        models/ball.gltf models/crate.gltf models/pillar.glb)

quench_add_game(quench_demo
    NAME "Quench Demo"
    OUTPUT_NAME quench-demo
    BUNDLE_ID org.quench.demo
    ICON icon/icon.png
    ORIENTATIONS all
    ASSETS assets
    COOKED_ASSETS quench_demo_cooked
    SOURCES
        AssetsSection.cpp
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
target_link_libraries(quench_demo PRIVATE quench_render quench_physics)
quench_add_shaders(quench_demo NAME demo_checker SOURCE shaders/checker.hlsl ENTRIES surface_vs:vertex surface_fs:fragment)
quench_add_shaders(quench_demo NAME demo_markers SOURCE shaders/markers.hlsl ENTRIES marker_vs:vertex marker_fs:fragment)
