# Tracy profiling (QUENCH_TRACY, docs/performance.md "Profiling with Tracy"; pins and licences in
# docs/dependencies.md). Off by default: nothing is fetched and the engine's profile macros compile to nothing
# (include/quench/core/Profile.h).
#
# On: quench_tracy is Tracy's client (public/TracyClient.cpp, one translation unit) in on-demand mode, so a program
# records only while a Tracy server is connected; desktops listen on localhost only (no firewall prompt), iOS on its
# network without the UDP broadcast that would need the multicast entitlement. QUENCH_TRACY_TOOLS adds Tracy's
# command line tools tracy-capture and tracy-csvexport (host builds), built from Tracy's sources with their
# dependencies pinned here instead of the CPM downloads of Tracy's own build (tags, no hashes).
include(FetchContent)

# ---------------------------------------------------------------- Tracy
# v0.15.0, BSD-3-Clause.
FetchContent_Declare(tracy
    URL https://github.com/wolfpld/tracy/archive/f2a4f2529e7e71844c9e434b877770a21ecf7587.tar.gz
    URL_HASH SHA256=12e847c3a50e4dba3f7d05d3b0abce4c8d635fec979b6ba9395ce8e863d96dec
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(tracy)

find_package(Threads REQUIRED)
add_library(quench_tracy STATIC ${tracy_SOURCE_DIR}/public/TracyClient.cpp)
target_include_directories(quench_tracy SYSTEM PUBLIC ${tracy_SOURCE_DIR}/public)
target_compile_definitions(quench_tracy PUBLIC TRACY_ENABLE TRACY_ON_DEMAND)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_compile_definitions(quench_tracy PUBLIC TRACY_NO_BROADCAST)
else()
    target_compile_definitions(quench_tracy PUBLIC TRACY_ONLY_LOCALHOST)
endif()
target_compile_options(quench_tracy PRIVATE -w)
target_link_libraries(quench_tracy PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

if(NOT QUENCH_TRACY_TOOLS)
    return()
endif()

# ---------------------------------------------------------------- tracy-capture, tracy-csvexport
# The versions Tracy v0.15.0's cmake/vendor.cmake names, pinned by commit and hash.
function(quench_tracy_tools)
    # capstone 6.0.0-Alpha11 (BSD-3-Clause): the disassembler of Tracy's server code, with the architectures and
    # options Tracy's build sets.
    set(CAPSTONE_ARCHITECTURE_DEFAULT OFF)
    set(CAPSTONE_ARM_SUPPORT ON)
    set(CAPSTONE_AARCH64_SUPPORT ON)
    set(CAPSTONE_X86_SUPPORT ON)
    set(CAPSTONE_X86_ATT_DISABLE ON)
    set(CAPSTONE_BUILD_MACOS_THIN ON)
    set(CAPSTONE_BUILD_SHARED_LIBS OFF)
    set(CAPSTONE_BUILD_CSTOOL OFF)
    set(CAPSTONE_BUILD_LEGACY_TESTS OFF)
    set(CAPSTONE_INSTALL OFF)
    FetchContent_Declare(tracy_capstone
        URL https://github.com/capstone-engine/capstone/archive/b2bf6327b5c7dc43829130b7ccdb02ef9a65a990.tar.gz
        URL_HASH SHA256=f0f4c867df07b2d15fb5165e891a79bfa92b8098434b43a837d70f12b7b88cd9
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(tracy_capstone)
    # capstone builds with -Werror; a newer compiler's warning must not stop the profile build.
    target_compile_options(capstone PRIVATE -w)

    # Zstandard v1.5.7 (BSD-3-Clause): the compression of .tracy files. Its library sources, without assembly.
    FetchContent_Declare(tracy_zstd
        URL https://github.com/facebook/zstd/archive/f8745da6ff1ad1e7bab384bd1f9d742439278e99.tar.gz
        URL_HASH SHA256=4b0bd1f0cfb25e61b9103c35f27395530ff5b4c0d2513a00fd745849e85ea52c
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR no-cmake)
    FetchContent_MakeAvailable(tracy_zstd)
    file(GLOB zstd_sources CONFIGURE_DEPENDS ${tracy_zstd_SOURCE_DIR}/lib/common/*.c
         ${tracy_zstd_SOURCE_DIR}/lib/compress/*.c ${tracy_zstd_SOURCE_DIR}/lib/decompress/*.c
         ${tracy_zstd_SOURCE_DIR}/lib/dictBuilder/*.c)
    add_library(quench_tracy_zstd STATIC EXCLUDE_FROM_ALL ${zstd_sources})
    target_include_directories(quench_tracy_zstd SYSTEM PUBLIC ${tracy_zstd_SOURCE_DIR}/lib)
    target_compile_definitions(quench_tracy_zstd PRIVATE ZSTD_DISABLE_ASM)
    target_compile_options(quench_tracy_zstd PRIVATE -w)

    # PPQSort 1.0.6 (MIT), header-only: the server's parallel sort, with the two patches Tracy applies.
    FetchContent_Declare(tracy_ppqsort
        URL https://github.com/GabTux/PPQSort/archive/21c5a20ca3daf572caaf0a8522db32e27e28fa1d.tar.gz
        URL_HASH SHA256=b9b3a1b226bc19fffac2d080d1775b5350573856a61893e444f5ffa7cc982242
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/TracyPpqsort.cmake
        SOURCE_SUBDIR no-cmake)
    FetchContent_MakeAvailable(tracy_ppqsort)

    # Tracy's server library as its cmake/server.cmake lists it, with statistics (csvexport needs them).
    set(common ${tracy_SOURCE_DIR}/public/common)
    set(server ${tracy_SOURCE_DIR}/server)
    add_library(quench_tracy_server STATIC EXCLUDE_FROM_ALL
        ${common}/tracy_lz4.cpp ${common}/tracy_lz4hc.cpp ${common}/TracySocket.cpp ${common}/TracyStackFrames.cpp
        ${common}/TracySystem.cpp ${server}/TracyBroadcast.cpp ${server}/TracyMemory.cpp ${server}/TracyLocks.cpp
        ${server}/TracyMmap.cpp ${server}/TracyPrint.cpp ${server}/TracySysUtil.cpp ${server}/TracyTaskDispatch.cpp
        ${server}/TracyTextureCompression.cpp ${server}/TracyThreadCompress.cpp ${server}/TracyWorker.cpp
        ${tracy_SOURCE_DIR}/getopt/getopt.c)
    target_include_directories(quench_tracy_server SYSTEM PUBLIC ${common} ${server} ${tracy_SOURCE_DIR}/getopt
                               ${tracy_SOURCE_DIR}/util ${tracy_capstone_SOURCE_DIR}/include/capstone
                               ${tracy_ppqsort_SOURCE_DIR}/include ${CMAKE_CURRENT_BINARY_DIR}/tracy_gitref)
    target_link_libraries(quench_tracy_server PUBLIC capstone_static quench_tracy_zstd Threads::Threads)
    target_compile_options(quench_tracy_server PUBLIC -w)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        # PPQSort's <execution> (as Tracy's cmake/config.cmake sets it).
        target_compile_options(quench_tracy_server PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>)
    endif()
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/tracy_gitref/GitRef.hpp
         "#pragma once\nnamespace tracy { static inline const char* GitRef = \"f2a4f25\"; }\n")

    add_executable(quench_tracy_capture ${tracy_SOURCE_DIR}/capture/src/capture.cpp
                   ${tracy_SOURCE_DIR}/util/CaptureConsole.cpp ${tracy_SOURCE_DIR}/util/CaptureFileBackup.cpp
                   ${tracy_SOURCE_DIR}/util/CaptureOutput.cpp)
    add_executable(quench_tracy_csvexport ${tracy_SOURCE_DIR}/csvexport/src/csvexport.cpp)
    foreach(tool capture csvexport)
        target_link_libraries(quench_tracy_${tool} PRIVATE quench_tracy_server)
        set_target_properties(quench_tracy_${tool} PROPERTIES OUTPUT_NAME tracy-${tool}
                              RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    endforeach()
endfunction()
quench_tracy_tools()
