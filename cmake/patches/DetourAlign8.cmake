# `cmake -P` patch for the pinned recastnavigation source (FetchContent PATCH_COMMAND, cmake/Dependencies.cmake); run in
# its source directory. With 64-bit polygon references (DT_POLYREF64) a dtLink holds a uint64_t, but Detour lays out a
# tile's sections with dtAlign4, so the links can start 4 bytes off an 8-byte boundary: undefined behaviour
# (UndefinedBehaviorSanitizer stops quench_navigation's tests in the asan preset; upstream issue #669, the open pull
# request #791 aligns to 8 bytes there). The patch makes dtAlign4 round up to 8 bytes when DT_POLYREF64 is defined;
# every writer and reader of tile and layer data uses it, so the layouts stay consistent. Marked in the source as an
# altered version, as its zlib licence asks. Idempotent.
set(_file Detour/Include/DetourCommon.h)
file(READ ${_file} _src)
set(_marker "Quench patch: 8-byte sections with DT_POLYREF64")
string(FIND "${_src}" "${_marker}" _found)
if(NOT _found EQUAL -1)
    return()
endif()
set(_old "inline int dtAlign4(int x) { return (x+3) & ~3; }")
string(FIND "${_src}" "${_old}" _at)
if(_at EQUAL -1)
    message(FATAL_ERROR "DetourAlign8.cmake: dtAlign4 not found in ${_file}")
endif()
string(REPLACE "${_old}" "// ${_marker} (cmake/patches/DetourAlign8.cmake)
#ifdef DT_POLYREF64
inline int dtAlign4(int x) { return (x+7) & ~7; }
#else
${_old}
#endif" _src "${_src}")
file(WRITE ${_file} "${_src}")
