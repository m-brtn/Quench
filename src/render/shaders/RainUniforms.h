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
    float4 streaks;    // x: how many of them fall in the near box (the first ones), y: exposure (s: a streak's length
                       // is the distance a drop moves in it), z: a drop's diameter in light rain (m), w: in heavy rain
                       // (m)
    float4 lens;       // x: the lens's aperture (m), y: the distance it is focused at (m), z: a sharp streak's
                       // opacity, w: the distance within which the streaks fade out (m)
    float4 nearBox;    // a smaller box of streaks around the camera, denser (the rest fall in the box): xyz its centre
                       // (m), w: its half side (m)
    float4 splashes;   // x: count, y: radius around the camera (m), z: lifetime (s), w: size (m)
    float4 light;      // rgb: the radiance a drop refracts toward the camera (the sky's and the ground's), w: the
                       // focal length (pixels: a metre at 1 m of depth)
    float4 sun;        // rgb: the sun's irradiance (0: none), w: the share a drop sends forward toward the camera
    float4 shafts;     // x: steps, y: the cloud base (m), z: extinction at precipitation 1 (1/m), w: nearest (m)
    float4 weatherMap; // the clouds' weather map: xy its corner (world xz, m), z: 1 / its size (1/m), w: texels / m
    float4 rainbow;    // x: 1 to draw it, y: its strength, z: the frame's number (mod 4096: where the shafts' march
                       // reads its noise), w: the shafts' scale (target pixels per shaft texel along each axis)
    float4 target;     // xy: the size of the target drawn (pixels), zw: 1 / it; for the shafts the HDR image's
    float4 snowFall;   // xyz: the snow's velocity (m/s), w: 1 when the snow is drawn
    float4 flakes;     // x: count, y: the half side of their box around the camera (m), z: a flake's size (m), w: the
                       // share of the sun's irradiance a flake sends toward the camera
    float4 flakeLight; // rgb: the sky's and the ground's light a flake sends toward the camera, w: 1 when the rain is
                       // drawn
    float4 behind;     // x: 1 when the shafts lie behind the clouds along every ray (a camera at the cloud base or
                       // higher): rain_shafts_up_fs dims them by the clouds' transmittance; yzw: unused
};

QUENCH_SHARED_END

#endif
