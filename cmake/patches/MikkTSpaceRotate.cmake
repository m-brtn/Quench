# `cmake -P` patch for the pinned MikkTSpace source (FetchContent PATCH_COMMAND, cmake/Dependencies.cmake); run in its
# source directory. QuickSort and QuickSortEdges rotate their random seed with `(uSeed<<t)|(uSeed>>(32-t))`, a shift by
# 32 when t is 0: undefined behaviour (UndefinedBehaviorSanitizer stops quench_cook in the asan preset). The patch
# masks the right shift, `>>((32-t)&31)`, which gives the rotation the hardware gave (ARM64 and x86 mask the count),
# so the tangents do not change. Marked in the source as an altered version, as its zlib licence asks. Idempotent.
set(_file mikktspace.c)
file(READ ${_file} _src)
set(_marker "Quench patch: no shift by 32")
string(FIND "${_src}" "${_marker}" _found)
if(NOT _found EQUAL -1)
    return()
endif()
set(_old "t=(uSeed<<t)|(uSeed>>(32-t));")
string(REGEX MATCHALL "t=\\(uSeed<<t\\)\\|\\(uSeed>>\\(32-t\\)\\)" _matches "${_src}")
list(LENGTH _matches _count)
if(NOT _count EQUAL 2)
    message(FATAL_ERROR "MikkTSpaceRotate.cmake: expected the seed rotation twice in ${_file}, found ${_count}")
endif()
string(REPLACE "${_old}" "t=(uSeed<<t)|(uSeed>>((32-t)&31)); // ${_marker} (cmake/patches/MikkTSpaceRotate.cmake)"
       _src "${_src}")
file(WRITE ${_file} "${_src}")
