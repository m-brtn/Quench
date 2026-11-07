#ifndef QUENCH_SCENE_UNIFORMS_H
#define QUENCH_SCENE_UNIFORMS_H

// Uniform and storage structs of the render module (quench_render), read by its C++ code and by every shader that
// draws in its passes: the engine's passes, surface shaders (quench/Surface.hlsli) and a game's own passes. Members
// are float4 and float4x4 only (quench/Shared.h). Matrices are column-major, shaders multiply with mul(M, v).
//
// Bindings of a surface shader (SDL GPU slots, quench/Bindings.hlsli):
//   vertex:   uniform 0 FrameUniforms, uniform 1 DrawUniforms, storage buffer 0 InstanceData[]
//   fragment: uniform 0 FrameUniforms, uniform 1 LightUniforms, uniform 2 MaterialUniforms,
//             sampler 0 the sun's shadow cascades (Texture2DArray<float>, read with Gather),
//             samplers 1..4 the material's textures 0..3,
//             sampler 5 the cascades' transmittance of translucent casters (Texture2DArray<float4>),
//             samplers 6..9 the ray-traced sun visibility and occlusion, reflections, indirect light and local light
//             shadows at the render size, samplers 10 and 11 the probe grid's irradiance and distance atlases
//             (each read only where FrameUniforms::options.w says so), sampler 12 the table of the area lights
//             (quench/AreaLights.hlsli, read where LightUniforms::count.y is not 0), sampler 13 the local lights'
//             shadow maps (Texture2DArray<float>, read where FrameUniforms::localShadows.x is not 0)

#include "quench/Shared.h"

#define QUENCH_MAX_CASCADES 4
#define QUENCH_MAX_LIGHTS 16
#define QUENCH_MAX_AREA_LIGHTS 8 // of QUENCH_MAX_LIGHTS

// LocalLight::color.w of the area lights.
#define QUENCH_LIGHT_RECT 3
#define QUENCH_LIGHT_DISK 4
#define QUENCH_LIGHT_TUBE 5

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
    // x: exposure, y: frame index, z: TAA jitter on (0/1), w: what the ray-traced textures hold (0: none is read):
    // bit 0: sampler 6's rgb is the sun's visibility (in place of the shadow maps), bit 1: its alpha is the ambient
    // occlusion, bit 2: sampler 9 holds the shadows of the local lights whose LocalLight::cone.y is non-zero, bit 3:
    // sampler 7 the reflections (rgb radiance, a: weight), bit 4: sampler 8 the indirect light (rgb, irradiance /
    // pi), bit 5: the probe grid (samplers 10 and 11, probeGrid) lights what bit 4 does not, bit 6: sampler 7 the
    // screen-space reflections (rgb radiance, a: confidence, which scales their weight).
    float4 options;
    // Translucent casters (render::ShadowMode::Translucent) in the transmittance maps: x: the cascades that have them
    // (bit n: cascade n; 0: none, the maps are not read), y: 1 / transmittance map size, zw: unused.
    float4 shadowTransmittance;
    float4 transmittanceBias; // per cascade: how far (shadow depth) a receiver lies behind a caster before it is tinted
    // The probe grid (quench/Probes.hlsli): xyz: the cell of its first probe (a probe sits at cell * spacing), w:
    // spacing (m).
    float4 probeGrid;
    float4 probeCounts; // xyz: probes per axis, w: irradiance texels per probe side (with a one-texel border)
    float4 probeAtlas;  // x: distance texels per probe side (with the border), y: the sampling bias (m), zw: unused
    // x: the roughness above which surfaces reflect the sky instead of the traced (or screen-space) reflections,
    // yzw: unused
    float4 rayLighting;
    // The local lights' shadow maps (sampler 13): x: texels per side, y: 1 / x, z: the normal offset (texels), w: the
    // tangent of half the field of view of a point light's six maps (a little over 1, so that the filter's taps at a
    // map's edge stay inside it).
    float4 localShadows;
};

// One registered instance in the frame's instance buffer.
struct QUENCH_ALIGN16 InstanceData {
    float4x4 model;        // object -> world
    float4x4 normalMatrix; // inverse transpose of model
    float4 color;          // InstanceDesc::color (multiplies the vertex color)
    float4 custom;         // InstanceDesc::custom, for the surface shader
};

struct QUENCH_ALIGN16 DrawUniforms {
    // x: first instance of the draw in the instance buffer, y: shadow cascade (shadow passes), z: 1 in shadow passes
    // (surface_vs projects with the cascade instead of the camera)
    float4 info;
};

struct QUENCH_ALIGN16 MaterialUniforms {
    float4 baseColor; // rgb: linear albedo, a: opacity
    float4 emissive;  // rgb: linear radiance, w: 1 in the depth pass of a cascade when the opacity is dithered into it
    float4 surface;   // x: roughness, y: metallic, z: alpha cutoff (0: off), w: textures set (bit n: texture n)
    float4 params[4]; // MaterialDesc::params, for the surface shader
    // x: anisotropy (0: isotropic), yz: cos and sin of its rotation, w: the shape of an area light, shown with its
    // emissive radiance only: 1 on the front face (the back is black), 2 on both
    float4 shading;
};

// A light other than the sun: `color.w` selects the type (0 directional, 1 point, 2 spot, 3 rect, 4 disk, 5 tube).
struct QUENCH_ALIGN16 LocalLight {
    float4 positionRange; // xyz: position (area lights: the center, m), w: range (m)
    float4 color;         // rgb: color times intensity (area lights: the radiance of the shape), w: type
    // xyz: unit direction the light shines in (rect, disk: the normal), w: spot: cos of the outer cone angle; rect,
    // disk: 1 when two-sided
    float4 direction;
    // x: spot: 1 / (cos inner - cos outer); rect: half width, disk: radius, tube: half length (m); y: the channel of
    // its ray-traced shadow or of its shadow maps + 1 (0: none); z: point, spot: radius of the emitting sphere; rect:
    // half height, tube: radius (m)
    float4 cone;
    // Area lights: xyz the unit tangent (rect: along the width, tube: the axis), w: 1 + the light's id (the id its
    // shape's RayInstanceData refers to). Point and spot lights with shadow maps (FrameUniforms::localShadows): x: the
    // layer of the first map + 1 (0: none), y: a spot light's one map: the tangent of half its field of view, 0: six
    // maps (+x, -x, +y, -y, +z, -z), z: the maps' near plane (m), w: unused.
    float4 shape;
};

struct QUENCH_ALIGN16 LightUniforms {
    float4 count; // x: lights in use, y: area lights among them
    LocalLight lights[QUENCH_MAX_LIGHTS];
};

QUENCH_SHARED_END

#endif
