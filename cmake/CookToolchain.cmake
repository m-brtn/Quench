# Build-time asset toolchain (pinned archives + SHA256, like cmake/Dependencies.cmake): the host tool `quench_cook`
# (tools/cook) and its library `quench_cook_lib` (the cook tests link it).
#
# fastgltf reads glTF and GLB; it downloads its two single-file dependencies, simdjson 5.1.0 and simdutf 9.3.3, from
# their GitHub releases with SHA256 checks (fastgltf's cmake/dependencies.cmake). stb_image decodes PNG and JPEG,
# bc7enc.cpp (bc7enc_rdo, declared in cmake/Dependencies.cmake) encodes BC7, meshoptimizer optimizes and simplifies
# meshes. None of them is linked into a game.
include(FetchContent)

# ---------------------------------------------------------------- fastgltf v1.0.0
FetchContent_Declare(fastgltf
    URL https://github.com/spnda/fastgltf/archive/0b800b4a584704f9e9add67c79c4c8732e552787.tar.gz
    URL_HASH SHA256=edf3c48c54c4c1a9aa53843d805eecd339ea75488ef7f5f0ec7701e402acd501
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
    SYSTEM)
set(FASTGLTF_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_DOCS OFF CACHE BOOL "" FORCE)
# The pinned downloads, never an installed simdjson or simdutf of another version.
set(CMAKE_DISABLE_FIND_PACKAGE_simdjson ON)
set(CMAKE_DISABLE_FIND_PACKAGE_simdutf ON)
FetchContent_MakeAvailable(fastgltf)
foreach(_target fastgltf fastgltf_simdjson fastgltf_simdutf)
    if(TARGET ${_target})
        target_compile_options(${_target} PRIVATE -w)
    endif()
endforeach()

# ---------------------------------------------------------------- stb (stb_image.h)
FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5.tar.gz
    URL_HASH SHA256=a6c31950b3bdf05eb92258768ba2a11bd44449490574b1aac67028452288c40d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(stb)

# Built only where something needs it (a quench_cook_assets() call, the cook tests, the host-tools preset).
add_library(quench_cook_lib STATIC EXCLUDE_FROM_ALL
    ${Quench_SOURCE_DIR}/tools/cook/CookAnimation.cpp
    ${Quench_SOURCE_DIR}/tools/cook/CookModel.cpp
    ${Quench_SOURCE_DIR}/tools/cook/Gltf.cpp
    ${Quench_SOURCE_DIR}/tools/cook/CookTexture.cpp
    ${Quench_SOURCE_DIR}/tools/cook/ThirdParty.cpp
    ${bc7enc_SOURCE_DIR}/bc7enc.cpp)
quench_configure_target(quench_cook_lib)
set_source_files_properties(${Quench_SOURCE_DIR}/tools/cook/ThirdParty.cpp ${bc7enc_SOURCE_DIR}/bc7enc.cpp
    PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(quench_cook_lib PUBLIC ${Quench_SOURCE_DIR}/tools/cook)
target_include_directories(quench_cook_lib SYSTEM PRIVATE ${stb_SOURCE_DIR} ${bc7enc_SOURCE_DIR})
target_link_libraries(quench_cook_lib PUBLIC quench_assets quench_animation quench_core
                                      PRIVATE fastgltf::fastgltf quench_meshoptimizer quench_glm)

add_executable(quench_cook EXCLUDE_FROM_ALL ${Quench_SOURCE_DIR}/tools/cook/Cook.cpp)
quench_configure_target(quench_cook)
target_link_libraries(quench_cook PRIVATE quench_cook_lib quench_audio)
target_compile_definitions(quench_cook PRIVATE QUENCH_COOK_VERSION="${PROJECT_VERSION}")
