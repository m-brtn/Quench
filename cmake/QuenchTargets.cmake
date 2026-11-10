# Common settings of every engine target: C++20, the public include directory, warnings, ARC.
function(quench_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC ${Quench_SOURCE_DIR}/include PRIVATE ${Quench_SOURCE_DIR}/src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra $<$<BOOL:${QUENCH_WERROR}>:-Werror>)
        if(EMSCRIPTEN)
            # Emscripten's Clang is newer than Xcode's: its -Wextra reports designated initializers that leave the
            # other members to their defaults, which the engine does on purpose.
            target_compile_options(${target} PRIVATE -Wno-missing-designated-field-initializers)
        endif()
    elseif(MSVC)
        # The sources are UTF-8; /Zc:__cplusplus reports C++20 in __cplusplus; /bigobj for the largest units. C4324
        # (a structure padded for an alignas member) is what alignas is there for: cache lines, uniform blocks.
        target_compile_options(${target} PRIVATE /W4 $<$<BOOL:${QUENCH_WERROR}>:/WX> /wd4324 /utf-8
                                                 /Zc:__cplusplus /Zc:preprocessor /bigobj)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(APPLE)
        # Objective-C and Objective-C++ sources compile with ARC (each one stops with #error without it). The Xcode
        # generator drops options under $<COMPILE_LANGUAGE:OBJC,OBJCXX> and takes the build setting instead.
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:-fobjc-arc>)
        set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES)
    endif()
endfunction()
