// GPU execution time on Metal: GPUStartTime and GPUEndTime of the MTLCommandBuffer behind a signalled SDL GPU fence.
// SDL GPU has no timestamp queries, so this reads SDL's internal fence: the struct below mirrors the first member of
// MetalFence in SDL 3.4.18 (src/gpu/metal/SDL_gpu_metal.m), which keeps the command buffer until the last reference
// to the fence is released. Any SDL update must recheck that layout; the native Metal backend (src/gpu/metal) reads
// the command buffer's times directly.

#if !__has_feature(objc_arc)
#error "compile with ARC (-fobjc-arc, set by quench_configure_target)"
#endif

#import <Metal/Metal.h>

typedef struct QuenchSdlMetalFenceView {
    __unsafe_unretained id<MTLCommandBuffer> commandBuffer;
} QuenchSdlMetalFenceView;

int quench_sdlgpu_metal_fence_times(const void* fence, double* start, double* end) {
    if (!fence) return 0;
    id<MTLCommandBuffer> commandBuffer = ((const QuenchSdlMetalFenceView*)fence)->commandBuffer;
    if (commandBuffer == nil || commandBuffer.status != MTLCommandBufferStatusCompleted) return 0;
    // The status can read completed before the GPU times are set; the wait returns once they are.
    [commandBuffer waitUntilCompleted];
    *start = commandBuffer.GPUStartTime;
    *end = commandBuffer.GPUEndTime;
    return 1;
}
