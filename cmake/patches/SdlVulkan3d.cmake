# `cmake -P` patch for the pinned SDL source (FetchContent PATCH_COMMAND, cmake/Dependencies.cmake); run in the SDL
# source directory. SDL GPU's Vulkan backend creates every 3D texture with VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT,
# which only rendering into a 3D texture's slices needs (2D views of it). MoltenVK's portability subset does not have
# it (imageView2DOn3DImage), so the Khronos validation layer reports an error for any 3D texture there, also one that
# is only sampled (the clouds' noise, src/render/Clouds.cpp). The flag is set for 3D color targets only. Idempotent.
set(_file src/gpu/vulkan/SDL_gpu_vulkan.c)
file(READ ${_file} _src)
set(_marker "Quench patch: 3D textures")
string(FIND "${_src}" "${_marker}" _found)
if(NOT _found EQUAL -1)
    return()
endif()
set(_old "    } else if (createinfo->type == SDL_GPU_TEXTURETYPE_3D) {\n")
string(APPEND _old "        imageCreateFlags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;\n")
string(FIND "${_src}" "${_old}" _at)
if(_at EQUAL -1)
    message(FATAL_ERROR "SdlVulkan3d.cmake: the 3D image flags were not found in ${_file}")
endif()
set(_new "    } else if (createinfo->type == SDL_GPU_TEXTURETYPE_3D &&
               (createinfo->usage & SDL_GPU_TEXTUREUSAGE_COLOR_TARGET)) { // ${_marker}: color targets only
        imageCreateFlags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;\n")
string(REPLACE "${_old}" "${_new}" _src "${_src}")
file(WRITE ${_file} "${_src}")
