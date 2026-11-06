#ifndef QUENCH_RAY_TRACING_UNIFORMS_H
#define QUENCH_RAY_TRACING_UNIFORMS_H

// Uniform and storage structs of the renderer's ray-traced passes (raytrace.hlsl), read by src/render/RayTracing.cpp.

#include "quench/Shared.h"

#define QUENCH_RT_ALPHA_TEXTURES 8

QUENCH_SHARED_BEGIN

struct QUENCH_ALIGN16 RayTracingUniforms {
    // x: rays per pixel toward the sun (0: no sun rays), y: tan of the sun cone's half angle, z: ray length (m),
    // w: origin offset along the normal per meter of view distance
    float4 shadow;
    // x: rays per pixel of ambient occlusion (0: none), y: occlusion radius (m), z: strength (0..1), w: unused
    float4 occlusion;
    // Denoise: x: radius of the sun's visibility (rgb) in pixels (0: as traced), y: depth tolerance (relative to
    // the view depth), z: radius of the ambient occlusion (a) in pixels, w: unused
    float4 denoise;
};

// A non-opaque instance in the top level (cut-out, translucent or dithered caster), at its instance id. The
// geometry indices are asuint() of the floats.
struct QUENCH_ALIGN16 RayInstanceData {
    // x: first index in the index buffer, y: first vertex in the vertex buffer, z: mode (1 cut-out, 2 translucent,
    // 3 dithered), w: alpha texture slot (-1: none)
    float4 geometry;
    float4 color;  // base color times the instance color (opacity in a)
    float4 params; // x: alpha cutoff (0: none), yzw: unused
};

QUENCH_SHARED_END

#endif
