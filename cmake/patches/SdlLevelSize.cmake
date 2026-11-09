# `cmake -P` patch for the pinned SDL source (FetchContent PATCH_COMMAND, cmake/Dependencies.cmake); run in the SDL
# source directory. SDL GPU takes a mip level's size as `size >> level`, which is 0 for the last levels of a non-square
# texture (levels 2 and 3 of 8 x 2): in the shader blit (SDL_GPU_BlitCommon: the Direct3D 12 backend's blits and mipmap
# generation, the Metal backend's blits) the source coordinates become NaN, and a render pass into such a level gets an
# empty default viewport and scissor (Direct3D 12, Metal) or framebuffer (Vulkan), so nothing is drawn. A level's size
# is at least 1, as the texture's own is. Idempotent.
set(_marker "Quench patch: level size")

# Replaces `regex` by `replace` in `file`, which must hold `expected` matches.
function(_quench_level_size file regex replace expected)
    file(READ ${file} src)
    string(FIND "${src}" "${_marker}" found)
    if(NOT found EQUAL -1)
        return()
    endif()
    string(REGEX MATCHALL "${regex}" matches "${src}")
    list(LENGTH matches count)
    if(NOT count EQUAL expected)
        message(FATAL_ERROR "SdlLevelSize.cmake: expected ${expected} level sizes in ${file}, found ${count}")
    endif()
    string(REGEX REPLACE "${regex}" "${replace}" src "${src}")
    file(WRITE ${file} "/* ${_marker} (cmake/patches/SdlLevelSize.cmake): levels at least 1 x 1. */\n${src}")
endfunction()

# The blit's source region: its left, top, width and height.
_quench_level_size(src/gpu/SDL_gpu.c "\\((src_header->info\\.(width|height) >> info->source\\.mip_level)\\)"
                   "SDL_max(\\1, 1)" 4)
# The render passes' size: the width and height of the color targets and of the depth target.
set(_pass "= ([A-Za-z]+->header\\.info\\.(width|height) >> [A-Za-z]+(\\[i\\])?(\\.|->)mip_level)")
_quench_level_size(src/gpu/d3d12/SDL_gpu_d3d12.c "${_pass}" "= SDL_max(\\1, 1)" 4)
_quench_level_size(src/gpu/metal/SDL_gpu_metal.m "${_pass}" "= SDL_max(\\1, 1)" 4)
_quench_level_size(src/gpu/vulkan/SDL_gpu_vulkan.c "${_pass}" "= SDL_max(\\1, 1)" 4)
