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
//             shadow maps (Texture2DArray<float>, read where FrameUniforms::localShadows.x is not 0), sampler 14 the
//             weather maps (Texture2D<float4>: the clouds' shadow, read where FrameUniforms::cloudShadow.w is not 0,
//             and the rain's maps, read where FrameUniforms::rain.x is not 0), sampler 15 the sky cube
//             (TextureCube<float4>, read where FrameUniforms::skyCube.x is not 0)

#include "quench/Shared.h"

// The weather maps of surface sampler 14 (quench/Clouds.hlsli, quench/Rain.hlsli): one texture of
// QUENCH_WEATHER_MAPS_WIDTH x QUENCH_WEATHER_MAPS_HEIGHT texels in three regions. Rows 0 to QUENCH_RAIN_MAP_ROW - 1:
// the clouds' shadow (r); rows QUENCH_RAIN_MAP_ROW to QUENCH_WETNESS_ROW - 1: the rain occlusion map (r: the depth of
// the highest surface along the rain, 0..1 over FrameUniforms::rain.y); rows QUENCH_WETNESS_ROW on, columns 0 to
// QUENCH_WETNESS_GRID - 1: the weather's wetness grid (r: ground wetness, g: standing water, b: precipitation, a: the
// share of it that falls as rain), and columns QUENCH_WETNESS_GRID to 2 * QUENCH_WETNESS_GRID - 1 beside it: the
// snow cover in the same cells (r).
#define QUENCH_WEATHER_MAPS_WIDTH 512
#define QUENCH_WEATHER_MAPS_HEIGHT 1040
#define QUENCH_RAIN_MAP_ROW 512
#define QUENCH_WETNESS_ROW 1024
#define QUENCH_WETNESS_GRID 16

#define QUENCH_MAX_CASCADES 4
#define QUENCH_MAX_LIGHTS 16
#define QUENCH_MAX_AREA_LIGHTS 8 // of QUENCH_MAX_LIGHTS
#define QUENCH_MAX_PROBE_CASCADES 4

// LocalLight::color.w of the area lights.
#define QUENCH_LIGHT_RECT 3
#define QUENCH_LIGHT_DISK 4
#define QUENCH_LIGHT_TUBE 5

// The near plane of an area light's shadow maps (quench/Shadows.hlsli), a share of their far plane.
#define QUENCH_AREA_SHADOW_NEAR 0.01

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
    // The probe grid (quench/Probes.hlsli), per cascade (probeAtlas.z of them): xyz: the cell of its first probe (a
    // probe sits at (cell + 0.5) * spacing), w: spacing (m; each cascade twice the one before).
    float4 probeGrid[QUENCH_MAX_PROBE_CASCADES];
    float4 probeCounts; // xyz: probes per axis in each cascade, w: irradiance texels per probe side (with the border)
    // x: distance texels per probe side (with the border), y: the sampling bias of cascade 0 (m; it doubles with the
    // spacing), z: cascades (1..QUENCH_MAX_PROBE_CASCADES), w: bit 0: probes moved out of geometry, bit 1: probes
    // without surfaces near them left out (with either, the irradiance atlas has a row of data per probe row).
    float4 probeAtlas;
    // x: the roughness above which surfaces reflect the sky instead of the traced (or screen-space) reflections,
    // yzw: unused
    float4 rayLighting;
    // The local lights' shadow maps (sampler 13): x: texels per side, y: 1 / x, z: the normal offset (texels), w: the
    // tangent of half the field of view of a point light's six maps (a little over 1, so that the filter's taps at a
    // map's edge stay inside it).
    float4 localShadows;
    // The clouds (render::RenderSettings::clouds; quench/Clouds.hlsli). Their shadow (sampler 14: the share of the
    // sun's light the clouds above RenderSettings::cloudShadowHeight let through, a top-down map on that plane): xy:
    // the map's corner (world x and z, m) followed along the sun's direction from that plane down to y = 0, where
    // quenchCloudShadow meets a point's line toward the sun, z: 1 / its size (m), w: 1 when it is read.
    float4 cloudShadow;
    // The sky cube (sampler 15: the sky with its clouds, for the sky's reflection): x: 1 when it is read, y: its last
    // mip level.
    float4 skyCube;
    // How overcast it is around the camera: rgb: the radiance of a closed cloud deck's base (in the lights' units:
    // as bright as the light it gives the ground, skyAmbient under a closed cover), w: the cover (0..1). The
    // traced effects' sky (misses, the probe grid, the path tracer) takes it in place of the clouds themselves.
    float4 overcast;
    // The rain (render::RenderSettings::rain; quench/Rain.hlsli). World -> the rain occlusion map: an orthographic
    // view down the direction the rain falls, xy -1..1 over the map, z 0..1 from its top to its bottom.
    float4x4 rainViewProj;
    // x: 1 when the rain's maps are read (sampler 14: with rain or snow), y: the occlusion map's depth range (m), z:
    // the precipitation at the camera (0..1), w: 1 when the rain is drawn (wet surfaces and their ripples)
    float4 rain;
    // The wetness grid (sampler 14's last rows): xy its corner (world xz, m), z: 1 / its size (m), w: unused.
    float4 rainGrid;
    // The snow (render::RenderSettings::snow): x: 1 when it is drawn (the snow cover on surfaces), y: the share of the
    // precipitation at the camera that falls as snow, zw: unused.
    float4 snow;
    // The weather's wind (weather::Weather::windUniforms; all 0 without a weather): the members of WindUniforms
    // (quench/WeatherField.h) in order, read through quenchFrameWind (quench/Wind.hlsli).
    float4 windAir;
    float4 windSpeed;
    float4 windTime;
    // The lightning (render::RenderSettings::lightning): xyz: the unit direction from the camera toward the brightest
    // flash now (in its cloud), w: its glow there (0: none); rgb: the glow's radiance at 1, w: unused.
    float4 lightning;
    float4 lightningColor;
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
    // The rain and snow on it (quench/Rain.hlsli): x: how wet it gets (MaterialDesc::wetness), y: how much standing
    // water it collects (MaterialDesc::puddles), z: 1 for water (MaterialDesc::water), w: how much snow lies on it
    // (MaterialDesc::snow; 0 for water)
    float4 weather;
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
    // half height, tube: radius (m); w: area lights with shadow maps (FrameUniforms::localShadows): the layer of the
    // first map + 1 (0: none)
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
