# `cmake -P` patch for the pinned SDL source (FetchContent PATCH_COMMAND, cmake/Dependencies.cmake); run in the SDL
# source directory. SDL GPU's Vulkan driver defragments its memory at a submit (every submit when no window is
# claimed, else the one that acquired a swapchain texture): it copies the textures and buffers of a fragmented
# allocation into new ones at the end of the submitted command buffer and points their containers at the copies. It
# skips allocations with pending transfers, but not resources that another command buffer, acquired and not yet
# submitted, has already bound: that command buffer still uses the old image, which the defragmentation left in
# TRANSFER_SRC_OPTIMAL (VUID-vkCmdDraw-None-09600 at its submit), or writes to the old buffer after the copy. The
# engine submits small command buffers while a frame's is open (uploads, lazy initialization). The patch counts the
# command buffers that are open and defragments only when no other one is. Idempotent. Reported to SDL:
# https://github.com/libsdl-org/SDL/issues/16685 (docs/dependencies.md).
set(_file src/gpu/vulkan/SDL_gpu_vulkan.c)
set(_marker "Quench patch: defragmentation")
file(READ ${_file} _src)
string(FIND "${_src}" "${_marker}" _found)
if(NOT _found EQUAL -1)
    return()
endif()

# Replaces `old` by `new` in _src; `old` must occur exactly once.
function(_quench_defrag old new)
    string(FIND "${_src}" "${old}" first)
    string(FIND "${_src}" "${old}" last REVERSE)
    if(first EQUAL -1 OR NOT first EQUAL last)
        message(FATAL_ERROR "SdlVulkanDefrag.cmake: expected one match in ${_file} of:\n${old}")
    endif()
    string(REPLACE "${old}" "${new}" result "${_src}")
    set(_src "${result}" PARENT_SCOPE)
endfunction()

_quench_defrag("    Uint8 defragInProgress;\n"
               "    Uint8 defragInProgress;\n    SDL_AtomicInt openCommandBufferCount; // ${_marker}: acquired, not yet submitted or cancelled\n")
_quench_defrag("    if (!VULKAN_INTERNAL_BeginCommandBuffer(renderer, commandBuffer)) {
        return NULL;
    }

    return (SDL_GPUCommandBuffer *)commandBuffer;
}"
"    if (!VULKAN_INTERNAL_BeginCommandBuffer(renderer, commandBuffer)) {
        return NULL;
    }

    SDL_AddAtomicInt(&renderer->openCommandBufferCount, 1);
    return (SDL_GPUCommandBuffer *)commandBuffer;
}")
_quench_defrag("    if (performCleanups &&
        renderer->allocationsToDefragCount > 0 &&
        !renderer->defragInProgress) {"
"    // Resources that another open command buffer has bound must not move.
    const bool otherCommandBuffersOpen = SDL_AddAtomicInt(&renderer->openCommandBufferCount, -1) > 1;
    if (performCleanups &&
        renderer->allocationsToDefragCount > 0 &&
        !renderer->defragInProgress &&
        !otherCommandBuffersOpen) {")
_quench_defrag("    vulkanCommandBuffer = (VulkanCommandBuffer *)commandBuffer;
    renderer = vulkanCommandBuffer->renderer;

    result = renderer->vkResetCommandBuffer("
"    vulkanCommandBuffer = (VulkanCommandBuffer *)commandBuffer;
    renderer = vulkanCommandBuffer->renderer;
    SDL_AddAtomicInt(&renderer->openCommandBufferCount, -1);

    result = renderer->vkResetCommandBuffer(")
file(WRITE ${_file} "${_src}")
