# Build-time asset toolchain (pinned archives + SHA256, like cmake/Dependencies.cmake): the host tool `quench_cook`
# (tools/cook) and its library `quench_cook_lib` (the cook tests link it).
#
# fastgltf reads glTF and GLB; it downloads its two single-file dependencies, simdjson 5.1.0 and simdutf 9.3.3, from
# their GitHub releases with SHA256 checks (fastgltf's cmake/dependencies.cmake). stb_image decodes PNG and JPEG,
# bc7enc.cpp (bc7enc_rdo, declared in cmake/Dependencies.cmake) encodes BC7, astc-encoder encodes (and, for the
# tests, decodes) ASTC, meshoptimizer optimizes and simplifies meshes. None of them is linked into a game. Navmeshes
# are built by quench_navigation itself.
include(FetchContent)

# Third-party sources build without warnings: -w, or /w with MSVC (its options have no strict aliasing or contraction
# switches to turn off).
if(MSVC)
    set(_quench_cook_no_warnings /w)
    set(_quench_cook_no_aliasing "")
    set(_quench_cook_no_contract "")
else()
    set(_quench_cook_no_warnings -w)
    set(_quench_cook_no_aliasing -fno-strict-aliasing)
    set(_quench_cook_no_contract -ffp-contract=off)
endif()

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
        target_compile_options(${_target} PRIVATE ${_quench_cook_no_warnings})
    endif()
endforeach()

# ---------------------------------------------------------------- stb (stb_image.h)
FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5.tar.gz
    URL_HASH SHA256=a6c31950b3bdf05eb92258768ba2a11bd44449490574b1aac67028452288c40d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(stb)

# ---------------------------------------------------------------- astc-encoder 5.7.1
# Its library sources only (not its CMake, which builds one library per instruction set and the command line tool).
# The instruction set comes from the compiler's own macros (NEON on arm64, SSE2 or better on x86-64);
# -ffp-contract=off is upstream's default "invariance" setting: the same output for any build type.
FetchContent_Declare(astcenc
    URL https://github.com/ARM-software/astc-encoder/archive/0affb51aaf0e0de3a8b038fa32487d59754a1eed.tar.gz
    URL_HASH SHA256=2dab18a2d89609d45b83b0395b5b0865358edcf65dba440ff7ebb2c74909a682
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(astcenc)
set(_astcenc_sources "")
foreach(_name averages_and_directions block_sizes color_quantize color_unquantize compress_symbolic compute_variance
              decompress_symbolic diagnostic_trace entry find_best_partitioning ideal_endpoints_and_weights image
              integer_sequence mathlib mathlib_softfloat partition_tables percentile_tables
              pick_best_endpoint_format quantization symbolic_physical weight_align weight_quant_xfer_tables)
    list(APPEND _astcenc_sources ${astcenc_SOURCE_DIR}/Source/astcenc_${_name}.cpp)
endforeach()

# ---------------------------------------------------------------- Basis Universal v2.50 encoder
# The sources cmake/QuenchBasisu.cmake keeps (upstream's encoder library list, without OpenCL, SSE and its astcenc
# fork), against the engine's transcoder (quench_basisu_transcoder) and with the full Zstandard (zstd/zstd.c, for the
# Zstandard supercompression of UASTC files). Its job pool gets one thread (the calling one): the cook runs textures
# in parallel on its own job system.
set(_basisu_encoder_sources ${QUENCH_BASISU_SOURCE_DIR}/encoder/jpgd.cpp
    ${QUENCH_BASISU_SOURCE_DIR}/encoder/pvpngreader.cpp
    ${QUENCH_BASISU_SOURCE_DIR}/encoder/3rdparty/android_astc_decomp.cpp
    ${QUENCH_BASISU_SOURCE_DIR}/zstd/zstd.c)
foreach(_name backend basis_file comp enc etc frontend gpu_texture pvrtc1_4 resampler resample_filters ssim uastc_enc
              bc7e_scalar dds_export bc7enc kernels_sse bc15_spmd bc15_spmd_sse opencl uastc_hdr_4x4_enc
              astc_hdr_6x6_enc astc_hdr_common astc_ldr_common astc_ldr_encode astc_ldr_fencode xbc7_encode tinyexr)
    list(APPEND _basisu_encoder_sources ${QUENCH_BASISU_SOURCE_DIR}/encoder/basisu_${_name}.cpp)
endforeach()
add_library(quench_basisu_encoder STATIC EXCLUDE_FROM_ALL ${_basisu_encoder_sources})
target_include_directories(quench_basisu_encoder SYSTEM PUBLIC ${QUENCH_BASISU_SOURCE_DIR}/encoder)
target_compile_definitions(quench_basisu_encoder PUBLIC BASISU_SUPPORT_SSE=0 BASISU_SUPPORT_OPENCL=0
                                                        BASISU_SUPPORT_ASTCENC=0)
target_compile_options(quench_basisu_encoder PRIVATE ${_quench_cook_no_warnings} ${_quench_cook_no_aliasing})
find_package(Threads REQUIRED)
target_link_libraries(quench_basisu_encoder PUBLIC quench_basisu_transcoder Threads::Threads)

# Built only where something needs it (a quench_cook_assets() call, the cook tests, the host-tools preset).
add_library(quench_cook_lib STATIC EXCLUDE_FROM_ALL
    ${Quench_SOURCE_DIR}/tools/cook/CookAnimation.cpp
    ${Quench_SOURCE_DIR}/tools/cook/CookBasis.cpp
    ${Quench_SOURCE_DIR}/tools/cook/CookModel.cpp
    ${Quench_SOURCE_DIR}/tools/cook/CookNavigation.cpp
    ${Quench_SOURCE_DIR}/tools/cook/Gltf.cpp
    ${Quench_SOURCE_DIR}/tools/cook/CookTexture.cpp
    ${Quench_SOURCE_DIR}/tools/cook/Retarget.cpp
    ${Quench_SOURCE_DIR}/tools/cook/ThirdParty.cpp
    ${bc7enc_SOURCE_DIR}/bc7enc.cpp
    ${_astcenc_sources})
quench_configure_target(quench_cook_lib)
set_source_files_properties(${Quench_SOURCE_DIR}/tools/cook/ThirdParty.cpp ${bc7enc_SOURCE_DIR}/bc7enc.cpp
    PROPERTIES COMPILE_OPTIONS "${_quench_cook_no_warnings}")
set_source_files_properties(${_astcenc_sources} PROPERTIES
    COMPILE_OPTIONS "${_quench_cook_no_warnings};${_quench_cook_no_contract}")
target_include_directories(quench_cook_lib PUBLIC ${Quench_SOURCE_DIR}/tools/cook)
target_include_directories(quench_cook_lib SYSTEM PRIVATE ${stb_SOURCE_DIR} ${bc7enc_SOURCE_DIR}
                                                          ${astcenc_SOURCE_DIR}/Source)
target_link_libraries(quench_cook_lib PUBLIC quench_assets quench_animation quench_navigation quench_core
                                      PRIVATE fastgltf::fastgltf fastgltf_simdjson quench_meshoptimizer quench_glm
                                              quench_basisu_encoder)

add_executable(quench_cook EXCLUDE_FROM_ALL ${Quench_SOURCE_DIR}/tools/cook/Cook.cpp)
quench_configure_target(quench_cook)
target_link_libraries(quench_cook PRIVATE quench_cook_lib quench_audio)
target_compile_definitions(quench_cook PRIVATE QUENCH_COOK_VERSION="${PROJECT_VERSION}")
