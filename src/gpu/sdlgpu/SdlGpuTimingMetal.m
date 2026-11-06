// GPU execution time on Metal: GPUStartTime and GPUEndTime of the MTLCommandBuffer behind a signalled SDL GPU fence.
// SDL GPU has no timestamp queries, so this reads SDL's internal fence: the struct below mirrors the first member of
// MetalFence in SDL 3.4.18 (src/gpu/metal/SDL_gpu_metal.m), which keeps the command buffer until the last reference
// to the fence is released. Any SDL update must recheck that layout; the native Metal backend (src/gpu/metal) reads
// the command buffer's times directly.

#import <Metal/Metal.h>

typedef struct QuenchSdlMetalFenceView {
    __unsafe_unretained id<MTLCommandBuffer> commandBuffer;
} QuenchSdlMetalFenceView;

double quench_sdlgpu_metal_fence_ms(const void* fence) {
    if (!fence) return -1.0;
    id<MTLCommandBuffer> commandBuffer = ((const QuenchSdlMetalFenceView*)fence)->commandBuffer;
    if (commandBuffer == nil || commandBuffer.status != MTLCommandBufferStatusCompleted) return -1.0;
    const CFTimeInterval start = commandBuffer.GPUStartTime;
    const CFTimeInterval end = commandBuffer.GPUEndTime;
    return end > start ? (end - start) * 1000.0 : -1.0;
}
