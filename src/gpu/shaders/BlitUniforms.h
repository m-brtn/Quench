#ifndef QUENCH_BLIT_UNIFORMS_H
#define QUENCH_BLIT_UNIFORMS_H

#include "quench/Shared.h"

QUENCH_SHARED_BEGIN

// CommandBuffer::blit on the native Metal backend (src/gpu/metal): the source region in normalized coordinates of
// its mip level, as SDL GPU's blit shaders take it.
struct QUENCH_ALIGN16 BlitUniforms {
    float4 region; // xy: left top, zw: width height
    float4 source; // x: mip level, y: array layer
};

QUENCH_SHARED_END

#endif
