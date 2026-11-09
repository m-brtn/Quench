# `cmake -P` patch for the pinned SDL source (FetchContent PATCH_COMMAND, cmake/Dependencies.cmake); run in the SDL
# source directory. SDL GPU's shader blit (SDL_GPU_BlitCommon: the Direct3D 12 backend's blits and mipmap generation,
# the Metal backend's blits) divides by a level's size taken as `width >> mip_level`, which is 0 for the last levels
# of a non-square texture (level 2 of 8 x 2): the source coordinates become NaN and the level stays empty. The size is
# at least 1, as a level's is. Idempotent.
set(_file src/gpu/SDL_gpu.c)
file(READ ${_file} _src)
set(_marker "Quench patch: blit level size")
string(FIND "${_src}" "${_marker}" _found)
if(NOT _found EQUAL -1)
    return()
endif()
string(LENGTH "${_src}" _before)
foreach(axis width height)
    string(REPLACE "(src_header->info.${axis} >> info->source.mip_level)"
                   "SDL_max(src_header->info.${axis} >> info->source.mip_level, 1)" _src "${_src}")
endforeach()
string(LENGTH "${_src}" _after)
# Each replacement adds 10 characters: the left, top, width and height of the source region.
math(EXPR _count "(${_after} - ${_before}) / 10")
if(NOT _count EQUAL 4)
    message(FATAL_ERROR "SdlBlitLevelSize.cmake: expected 4 level sizes in ${_file}, found ${_count}")
endif()
set(_line "    blit_fragment_uniforms.left = ")
string(REPLACE "${_line}" "    // ${_marker} (cmake/patches/SdlBlitLevelSize.cmake): at least 1 x 1.\n${_line}" _src
               "${_src}")
file(WRITE ${_file} "${_src}")
