#ifndef QUENCH_SCREEN_SPACE_UNIFORMS_H
#define QUENCH_SCREEN_SPACE_UNIFORMS_H

// Uniforms of the renderer's screen-space effects (screenspace.hlsl), read by src/render/ScreenSpace.cpp.

#include "quench/Shared.h"

QUENCH_SHARED_BEGIN

struct QUENCH_ALIGN16 ScreenSpaceUniforms {
    // Ambient occlusion: x: slices per pixel (1..8), y: steps per side of a slice, z: the effect's radius (m),
    // w: strength (0..1)
    float4 occlusion;
    // x: the noise's frame (0: the same noise every frame), y: the longest screen radius (pixels), z: depth
    // tolerance of the blur (relative to the view depth), w: unused
    float4 occlusionParams;
    // The camera rays (xyz): the position at reversed depth 1 of the point at render pixel coordinates (x, y) (pixel
    // centers at .5), relative to the camera, is rayOrigin + x * rayX + y * rayY; at depth d it is that divided by d
    // (the projection is infinite: the view depth is near / d).
    float4 rayOrigin;
    float4 rayX;
    float4 rayY;
};

QUENCH_SHARED_END

#endif
