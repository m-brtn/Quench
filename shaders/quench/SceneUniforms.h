#ifndef QUENCH_SCENE_UNIFORMS_H
#define QUENCH_SCENE_UNIFORMS_H

// Uniform and storage structs of the render module (quench_render), read by its C++ code and by every shader that
// draws in its passes: the engine's passes, surface shaders (quench/Surface.hlsli) and a game's own passes. Members
// are float4 and float4x4 only (quench/Shared.h). Matrices are column-major, shaders multiply with mul(M, v).
//
// Bindings of a surface shader (SDL GPU slots, quench/Bindings.hlsli):
//   vertex:   uniform 0 FrameUniforms, uniform 1 DrawUniforms, storage buffer 0 InstanceData[]
//   fragment: uniform 0 FrameUniforms, uniform 1 LightUniforms, uniform 2 MaterialUniforms,
//             sampler 0 the sun's shadow cascades (Texture2DArray<float>, comparison sampler),
//             samplers 1..4 the material's textures 0..3

#include "quench/Shared.h"

#define QUENCH_MAX_CASCADES 4
#define QUENCH_MAX_LIGHTS 16

QUENCH_SHARED_BEGIN

struct QUENCH_ALIGN16 FrameUniforms {
    float4x4 viewProj;                            // world -> clip with the TAA jitter (reversed depth: near 1, far 0)
    float4x4 invViewProj;                         // inverse of viewProj
    float4x4 viewProjStable;                      // without the jitter
    float4x4 prevViewProj;                        // the previous frame's viewProjStable (this frame's after a cut)
    float4x4 view;                                // world -> view (right-handed, looking down -z)
    float4x4 shadowViewProj[QUENCH_MAX_CASCADES]; // world -> shadow clip per cascade (standard depth)
    float4 cascadeSplits;                         // view distance of the far end of each cascade (m)
    float4 cascadeTexel;                          // world size of one shadow texel per cascade (m)
    float4 shadowParams;  // x: cascades (0: no sun shadows), y: 1 / map size, z: filter (0, 1, 2), w: normal offset
    float4 cameraPos;     // xyz, w: time (s)
    float4 cameraFwd;     // xyz: unit view direction, w: near plane (m)
    float4 sunDir;        // xyz: unit direction towards the sun, w: 1 when the scene has a sun
    float4 sunColor;      // rgb: irradiance of the sun on a surface facing it (linear HDR)
    float4 sunDisc;       // rgb: radiance of the sun disc in the sky, w: cos of the disc's angular radius
    float4 skyZenith;     // rgb: sky radiance overhead
    float4 skyHorizon;    // rgb: sky radiance at the horizon
    float4 skyGround;     // rgb: radiance below the horizon, w: atmosphere (0: gradient only, 1: scattering model)
    float4 sunGlow;       // rgb: glow and haze tint around the sun
    float4 skyAmbient;    // rgb: diffuse irradiance from the sky on a surface facing up
    float4 groundAmbient; // rgb: diffuse irradiance from below on a surface facing down
    float4 fogParams;     // x: density at the base height (1/m), y: height falloff (1/m), z: base height (m),
                          // w: sun scattering (0..1)
    float4 fogColor;      // rgb: fog radiance away from the sun, w: maximum fog amount (0..1)
    float4 screen;        // xy: render size (pixels), zw: 1 / render size
    float4 options;       // x: exposure, y: frame index, z: TAA jitter on (0/1), w: unused
};

// One registered instance in the frame's instance buffer.
struct QUENCH_ALIGN16 InstanceData {
    float4x4 model;        // object -> world
    float4x4 normalMatrix; // inverse transpose of model
    float4 color;          // InstanceDesc::color (multiplies the vertex color)
    float4 custom;         // InstanceDesc::custom, for the surface shader
};

struct QUENCH_ALIGN16 DrawUniforms {
    float4 info; // x: first instance of the draw in the instance buffer, y: shadow cascade (shadow passes)
};

struct QUENCH_ALIGN16 MaterialUniforms {
    float4 baseColor; // rgb: linear albedo, a: opacity
    float4 emissive;  // rgb: linear radiance
    float4 surface;   // x: roughness, y: metallic, z: alpha cutoff (0: off), w: textures set (bit n: texture n)
    float4 params[4]; // MaterialDesc::params, for the surface shader
};

// A light other than the sun: x of `color.w` selects the type (0 directional, 1 point, 2 spot).
struct QUENCH_ALIGN16 LocalLight {
    float4 positionRange; // xyz: position (m), w: range (m)
    float4 color;         // rgb: color times intensity, w: type
    float4 direction;     // xyz: unit direction the light shines in, w: cos of the outer cone angle
    float4 cone;          // x: 1 / (cos inner - cos outer)
};

struct QUENCH_ALIGN16 LightUniforms {
    float4 count; // x: lights in use
    LocalLight lights[QUENCH_MAX_LIGHTS];
};

QUENCH_SHARED_END

#endif
