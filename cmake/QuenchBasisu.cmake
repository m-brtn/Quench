# Basis Universal v2.50 (BinomialLLC/basis_universal, commit 9bebe16, Apache-2.0; its zstd/ is Zstandard 1.5, BSD
# 3-clause): quench_basisu_transcoder, the KTX2 transcoder in quench_assets, and quench_basisu_encoder, the encoder in
# quench_cook (cmake/CookToolchain.cmake).
#
# The release archive is 197 MB (mostly test images and web demos) for 13 MB of sources, so it is not fetched into
# every build tree: the first configure of a machine downloads it, checks its SHA256, keeps transcoder/, encoder/,
# zstd/ and the licence files in the tool cache (cmake/QuenchToolCache.cmake; ~/Library/Caches/Quench/basisu-2.50-9bebe16
# on macOS) and deletes the archive. QUENCH_BASISU_DIR names a source tree of the same commit to use instead.
#
# The transcoder is built without the targets the engine never asks for (PVRTC1/2, ATC, BC1, BC4, FXT1, EAC R11/RG11):
# it adds 0.96 MB to a stripped arm64 program instead of 1.32 MB with every target. The KTX2 container, ETC1S, UASTC LDR 4x4
# and its Zstandard supercompression, and the outputs BC7, ASTC 4x4, ETC1/ETC2 and RGBA32 stay. Its Zstandard decoder
# (zstddeclib.c) gets prefixed symbol names (src/assets/BasisZstdNames.h), so that the cook can link the full
# Zstandard for the encoder beside it and a game its own.
include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/QuenchToolCache.cmake)

set(QUENCH_BASISU_DIR "" CACHE PATH "Basis Universal source tree (empty: the pinned archive, kept in the tool cache)")

set(_quench_basisu_commit 9bebe16726b3a61c8c213eeee3b7cffb462ef34e)
set(_quench_basisu_sha256 8f4397a8d547ab07736c34a8d835e783cd019c3a0bd83d343c1bc4ebe7c570df)
set(_quench_basisu_name basisu-2.50-9bebe16)

# _quench_basisu_sources(<out>): the source tree, downloaded and trimmed on the first call of a machine.
function(_quench_basisu_sources out)
    if(QUENCH_BASISU_DIR)
        if(NOT EXISTS ${QUENCH_BASISU_DIR}/transcoder/basisu_transcoder.cpp)
            message(FATAL_ERROR "Quench: QUENCH_BASISU_DIR=${QUENCH_BASISU_DIR} has no transcoder/basisu_transcoder.cpp")
        endif()
        set(${out} ${QUENCH_BASISU_DIR} PARENT_SCOPE)
        return()
    endif()
    _quench_tool_cache(cache)
    set(dir ${cache}/${_quench_basisu_name})
    set(marker ${dir}/transcoder/basisu_transcoder.cpp)
    if(NOT EXISTS ${marker})
        file(MAKE_DIRECTORY ${cache})
        # One download per machine even when several configures run at once; the others wait for it.
        file(LOCK ${cache}/${_quench_basisu_name}.lock GUARD FUNCTION TIMEOUT 1800 RESULT_VARIABLE locked)
        if(NOT locked STREQUAL "0")
            message(FATAL_ERROR "Quench: cannot lock ${cache} for Basis Universal (${locked})")
        endif()
        if(NOT EXISTS ${marker})
            message(STATUS "Quench: downloading Basis Universal v2.50 (197 MB, once per machine) into ${dir}")
            set(archive ${cache}/${_quench_basisu_name}.tar.gz)
            file(DOWNLOAD https://github.com/BinomialLLC/basis_universal/archive/${_quench_basisu_commit}.tar.gz
                 ${archive} EXPECTED_HASH SHA256=${_quench_basisu_sha256} STATUS status)
            list(GET status 0 code)
            if(NOT code EQUAL 0)
                file(REMOVE ${archive})
                list(GET status 1 reason)
                message(FATAL_ERROR "Quench: downloading Basis Universal failed: ${reason}")
            endif()
            set(top basis_universal-${_quench_basisu_commit})
            set(stage ${dir}.partial)
            file(REMOVE_RECURSE ${stage})
            file(ARCHIVE_EXTRACT INPUT ${archive} DESTINATION ${stage}
                 PATTERNS ${top}/transcoder/* ${top}/encoder/* ${top}/zstd/* ${top}/LICENSE ${top}/NOTICE
                          ${top}/LICENSES/* ${top}/.reuse/*)
            file(REMOVE ${archive})
            file(REMOVE_RECURSE ${dir})
            file(RENAME ${stage}/${top} ${dir})
            file(REMOVE_RECURSE ${stage})
        endif()
        file(LOCK ${cache}/${_quench_basisu_name}.lock RELEASE)
    endif()
    set(${out} ${dir} PARENT_SCOPE)
endfunction()

_quench_basisu_sources(QUENCH_BASISU_SOURCE_DIR)
message(STATUS "Quench: Basis Universal sources: ${QUENCH_BASISU_SOURCE_DIR}")

set(_quench_basisu_wrappers ${CMAKE_CURRENT_LIST_DIR}/../src/assets)
add_library(quench_basisu_transcoder STATIC ${_quench_basisu_wrappers}/BasisTranscoder.cpp
                                            ${_quench_basisu_wrappers}/BasisZstd.c)
target_include_directories(quench_basisu_transcoder SYSTEM PUBLIC ${QUENCH_BASISU_SOURCE_DIR}/transcoder
                                                    PRIVATE ${QUENCH_BASISU_SOURCE_DIR}/zstd)
# PUBLIC: the encoder includes the transcoder's headers and must see the same configuration.
target_compile_definitions(quench_basisu_transcoder PUBLIC
    BASISD_SUPPORT_KTX2=1 BASISD_SUPPORT_KTX2_ZSTD=1 BASISD_SUPPORT_PVRTC1=0 BASISD_SUPPORT_PVRTC2=0
    BASISD_SUPPORT_ATC=0 BASISD_SUPPORT_DXT1=0 BASISD_SUPPORT_DXT5A=0 BASISD_SUPPORT_FXT1=0
    BASISD_SUPPORT_ETC2_EAC_RG11=0)
target_compile_features(quench_basisu_transcoder PUBLIC cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # Upstream builds with -fno-strict-aliasing.
    target_compile_options(quench_basisu_transcoder PRIVATE -w -fno-strict-aliasing)
endif()
