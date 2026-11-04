# Common settings of every engine target: C++20, the public include directory, warnings.
function(quench_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC ${Quench_SOURCE_DIR}/include PRIVATE ${Quench_SOURCE_DIR}/src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra $<$<BOOL:${QUENCH_WERROR}>:-Werror>)
    endif()
endfunction()
