#ifndef QUENCH_RENDER_POST_UNIFORMS_H
#define QUENCH_RENDER_POST_UNIFORMS_H

// Uniform blocks of the render module's own passes (src/render/shaders/frame.hlsl), read by Renderer.cpp.

#include "quench/Shared.h"

QUENCH_SHARED_BEGIN

// Fragment uniform slot 1 of the fullscreen passes (slot 0 holds FrameUniforms).
struct QUENCH_ALIGN16 PostUniforms {
    float4 texel;   // xy: 1 / source size (pixels), zw: per pass (taa: the source size)
    float4 params;  // per pass (frame.hlsl)
    float4 params2; // per pass
    float4 params3; // per pass
};

// One entry of the storage buffer of velocity_obj_vs: an instance that moved since the previous frame.
struct QUENCH_ALIGN16 VelocityInstance {
    float4x4 model;
    float4x4 prevModel;
};

QUENCH_SHARED_END

#endif
