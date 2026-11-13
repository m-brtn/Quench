quench_add_game(quench_hello
    NAME "Quench Hello"
    OUTPUT_NAME hello
    BUNDLE_ID org.quench.hello
    ICON icon/icon.png
    ORIENTATIONS all
    # Two small text files for the check of deferred assets on the web (scenarios/deferred_web.txt): there they are
    # beside the page in hello.assets/, everywhere else in the bundle.
    ASSETS assets
    DEFERRED_ASSETS deferred
    SOURCES Hello.cpp)
quench_configure_target(quench_hello)
quench_add_shaders(quench_hello NAME hello SOURCE shaders/hello.hlsl ENTRIES hello_vs:vertex hello_fs:fragment)
