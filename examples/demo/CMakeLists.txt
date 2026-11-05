quench_add_game(quench_demo
    NAME "Quench Demo"
    OUTPUT_NAME quench-demo
    BUNDLE_ID org.quench.demo
    ICON icon/icon.png
    ORIENTATIONS all
    SOURCES
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
