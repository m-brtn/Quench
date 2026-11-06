#ifndef QUENCH_RAY_TRACING_UNIFORMS_H
#define QUENCH_RAY_TRACING_UNIFORMS_H

// Uniform and storage structs of the renderer's ray-traced passes (raytrace.hlsl, raylight.hlsl, raydenoise.hlsl,
// pathtrace.hlsl), read by src/render/RayTracing.cpp and the classes around it.

#include "quench/Shared.h"

#define QUENCH_RT_ALPHA_TEXTURES 8
#define QUENCH_RT_LOCAL_SHADOWS 4

// Instance masks of the top level.
#define QUENCH_RT_MASK_CASTER 1u      // casts opaque or cut-out shadows
#define QUENCH_RT_MASK_TRANSLUCENT 2u // casts translucent shadows (tints the light)
#define QUENCH_RT_MASK_VISIBLE 4u     // seen by reflection, indirect light and path rays (opaque and cut-out)
#define QUENCH_RT_MASK_SEE_THROUGH 8u // translucent, seen by path rays

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
    // x: local lights with ray-traced shadows (LocalLight::cone.y = channel + 1), yzw: unused
    float4 local;
};

// Every instance in the top level, at its instance id: where its triangles are in the tables and what a hit reads.
// The geometry indices are asuint() of the floats.
struct QUENCH_ALIGN16 RayInstanceData {
    // x: first index in the index table, y: first vertex in the vertex table (the bind pose), z: 0, or 1 + the first
    // vertex in the frame's skinned vertex buffer (the pose: hit shading reads it), w: mode (0 opaque, 1 cut-out,
    // 2 translucent)
    float4 geometry;
    float4 color;    // base color times the instance color (opacity in a)
    float4 emissive; // rgb: emissive radiance, w: alpha cutoff (0: none)
    float4 surface;  // x: roughness, y: metallic, z: alpha texture slot of the shadow pass (-1: none), w: unused
    // Layers of the hit texture arrays (-1: none): x base color, y metallic-roughness (linear array), z emissive
    float4 layers;
    float4 normal0; // rows of the normal matrix (inverse transpose of the instance's 3 x 3), w: unused
    float4 normal1;
    float4 normal2;
};

// The passes that shade what rays hit (reflections, indirect light, probes, the path tracer).
struct QUENCH_ALIGN16 RayHitUniforms {
    // x: reflection roughness cutoff, y: origin offset along the normal per meter of view distance, z: ray length
    // (m), w: 1 when hits read the probe grid for their indirect light (else the sky's ambient)
    float4 params;
    // Probe rays: x: rays per probe, y: the longest distance the distance maps keep (m), z: hysteresis (0..1), w: 1
    // to restart the grid (no history)
    float4 probes;
    // Probe update: xyz: the cell of the grid's first probe in the previous frame, w: unused
    float4 previousGrid;
    // The path tracer: x: bounces, y: samples so far, z: 1 on the first sample, w: unused
    float4 path;
    float4x4 rotation; // a random rotation of the probe rays, new every frame
};

// The temporal denoiser (raydenoise.hlsl).
struct QUENCH_ALIGN16 RayDenoiseUniforms {
    // x: the longest history (frames), y: depth tolerance (relative), z: 1: the history scales with the signal's
    // alpha (reflections: rough ones keep more), w: 1: no history (a camera cut, a new target)
    float4 temporal;
    // x: step of this a-trous pass (pixels), y: plane distance tolerance (relative to the view depth), z: 1: the
    // radius scales with the signal's alpha, w: unused
    float4 filter;
};

QUENCH_SHARED_END

#endif
