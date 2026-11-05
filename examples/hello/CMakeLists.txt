quench_add_game(quench_hello
    NAME "Quench Hello"
    OUTPUT_NAME hello
    BUNDLE_ID org.quench.hello
    ICON icon/icon.png
    ORIENTATIONS all
    SOURCES Hello.cpp)
quench_configure_target(quench_hello)
quench_add_shaders(quench_hello NAME hello SOURCE shaders/hello.hlsl ENTRIES hello_vs:vertex hello_fs:fragment)
