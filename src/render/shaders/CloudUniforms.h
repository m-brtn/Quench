#ifndef QUENCH_RENDER_CLOUD_UNIFORMS_H
#define QUENCH_RENDER_CLOUD_UNIFORMS_H

// Uniform blocks of the cloud passes (src/render/shaders/clouds.hlsl), read by src/render/Clouds.cpp. The weather
// pass also reads WeatherUniforms (quench/WeatherField.h) in fragment uniform slot 2.

#include "quench/Shared.h"

QUENCH_SHARED_BEGIN

// The diffuse light in a cloud (clouds.hlsl, cloudScatter) and the ground's light under a cover (Clouds.cpp) by the
// same numbers: the share left at an optical depth tau is 1 / (1 + QUENCH_CLOUD_DIFFUSE tau); a cloud's column is its
// thickness (at most QUENCH_CLOUD_DEEP m) times its density, the extinction and QUENCH_CLOUD_COLUMN (the mean density
// along a column over the density at a point inside).
#define QUENCH_CLOUD_DIFFUSE 0.27f
#define QUENCH_CLOUD_DEEP 2000.0f
#define QUENCH_CLOUD_COLUMN 0.6f
// The gain of the sun's light that the first orders of scattering send toward the camera (cloudScatter's octaves,
// the 2D layer's lobe): with it the sunlit side of a thick cloud is about three quarters as bright as a white
// surface facing the sun and about three times its shadow side, which the diffuse light alone fills.
#define QUENCH_CLOUD_SUN 4.0f

// Fragment uniform slot 1 of the cloud passes (slot 0 holds FrameUniforms).
struct QUENCH_ALIGN16 CloudUniforms {
    float4x4 invViewProjStable; // clip -> world without the TAA jitter: the march's rays
    float4x4 prevViewProjStable;
    float4 weatherMap; // xy: world xz of the weather map's corner (m), z: 1 / its size (1/m), w: its size (m)
    float4 shadowMap;  // xy: world xz of the cloud shadow map's corner (m), z: 1 / its size (1/m), w: its plane's y
    float4 layer;      // x: cloud base (m), y: top of the tallest clouds (m), z: cirrus height (m), w: planet radius
    float4 air;        // xy: how far the air has moved (world xz, m), zw: unit wind direction
    float4 shape;      // x: shape noise scale (1/m), y: detail noise scale (1/m), z: evolution time, w: density
    float4 march;      // x: steps, y: light steps, z: longest path through the layer (m), w: unused
    float4 target;     // xy: size of the target drawn (pixels), zw: 1 / it
    // The lights are in the scene's units: what a white surface facing them sends back.
    float4 sun;           // rgb: the sun's light at the clouds, w: 1 when there is a sun
    float4 ambientTop;    // rgb: the clear sky's light on the clouds from above, w: unused
    float4 ambientBottom; // rgb: what the ground under the clouds sends up to their bases, w: unused
    float4 sunDeep;       // rgb: the sun's light on a level top, as it goes deep into a cloud (whitened), w: unused
    float4 temporal;      // x: weight of the new frame (1: no history), y: 1 when the history is valid, zw: where
                          // this frame's march reads its noise (an offset in pixels)
    float4 params;        // x: mode (1 layer, 2 volumetric), y: cube face (sky_cube_fs), z: cube size, w: the highest
                          // cloud top on the map (m): the top of the march
    float4 noise;         // x: slice, y: slices, z: kind (0 shape 3D, 1 detail 3D, 2 layer 2D), w: unused
};

QUENCH_SHARED_END

#endif
