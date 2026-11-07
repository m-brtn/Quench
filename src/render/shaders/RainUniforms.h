#ifndef QUENCH_RENDER_RAIN_UNIFORMS_H
#define QUENCH_RENDER_RAIN_UNIFORMS_H

// Uniform block of the rain passes (src/render/shaders/rain.hlsl), read by src/render/Rain.cpp.

#include "quench/Shared.h"

QUENCH_SHARED_BEGIN

// Uniform slot 1 of the rain passes, vertex and fragment (slot 0 holds FrameUniforms).
struct QUENCH_ALIGN16 RainUniforms {
    float4x4 viewProjStable;    // world -> clip without the TAA jitter: the streaks and splashes, drawn after TAA
    float4x4 invViewProjStable; // the shafts' rays
    float4x4 invRainViewProj;   // rain occlusion map (uv, depth as clip) -> world: where the splashes land
    float4 box;                 // xyz: the centre of the box of streaks around the camera (m), w: its half side (m)
    float4 fall;                // xyz: the rain's velocity (m/s), w: the time of the streaks (s, from the last cut)
    float4 streaks;    // x: count, y: exposure (s: a streak's length is the distance a drop falls in it), z: width
                       // (m), w: opacity of a streak at full rain
    float4 splashes;   // x: count, y: radius around the camera (m), z: lifetime (s), w: size (m)
    float4 light;      // rgb: the radiance a drop refracts toward the camera (the sky's and the ground's), w: the
                       // focal length (pixels: a metre at 1 m of depth)
    float4 sun;        // rgb: the sun's irradiance (0: none), w: the share a drop sends forward toward the camera
    float4 shafts;     // x: steps, y: the cloud base (m), z: extinction at precipitation 1 (1/m), w: nearest (m)
    float4 weatherMap; // the clouds' weather map: xy its corner (world xz, m), z: 1 / its size (1/m), w: texels / m
    float4 rainbow;    // x: 1 to draw it, y: its strength, z: the shafts' jitter this frame (0..1), w: the shafts'
                       // scale (target pixels per shaft texel along each axis)
    float4 target;     // xy: the size of the target drawn (pixels), zw: 1 / it; for the shafts the HDR image's
};

QUENCH_SHARED_END

#endif
