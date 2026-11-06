# Common settings of every engine target: C++20, the public include directory, warnings, ARC.
function(quench_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC ${Quench_SOURCE_DIR}/include PRIVATE ${Quench_SOURCE_DIR}/src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra $<$<BOOL:${QUENCH_WERROR}>:-Werror>)
    endif()
    if(APPLE)
        # Objective-C and Objective-C++ sources compile with ARC (each one stops with #error without it). The Xcode
        # generator drops options under $<COMPILE_LANGUAGE:OBJC,OBJCXX> and takes the build setting instead.
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:-fobjc-arc>)
        set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES)
    endif()
endfunction()
