#ifndef QUENCH_SCREEN_SPACE_UNIFORMS_H
#define QUENCH_SCREEN_SPACE_UNIFORMS_H

// Uniforms of the renderer's screen-space effects (screenspace.hlsl), read by src/render/ScreenSpace.cpp.

#include "quench/Shared.h"

QUENCH_SHARED_BEGIN

struct QUENCH_ALIGN16 ScreenSpaceUniforms {
    // Ambient occlusion: x: slices per pixel (1..8), y: steps per side of a slice, z: the effect's radius (m),
    // w: strength (0..1)
    float4 occlusion;
    // x: the noise's frame, y: the longest screen radius (pixels), z: depth tolerance of the blur and of the history
    // (relative to the view depth), w: the longest history (frames; 0: the frame has none to read)
    float4 occlusionParams;
    // The camera rays (xyz): the position at reversed depth 1 of the point at render pixel coordinates (x, y) (pixel
    // centers at .5), relative to the camera, is rayOrigin + x * rayX + y * rayY; at depth d it is that divided by d
    // (the projection is infinite: the view depth is near / d).
    float4 rayOrigin;
    float4 rayX;
    float4 rayY;
    // Reflections: x: march steps, y: the first step (relative to the view depth; each next one reflectionParams.z
    // times longer), z: the thickness a surface is taken to have behind its depth (relative to its view depth), w: the
    // roughness cutoff (RenderSettings::reflectionRoughness)
    float4 reflection;
    // x: the noise's frame, y: depth tolerance of the resolve (relative to the view depth), z: the growth of the
    // march's steps, w: unused
    float4 reflectionParams;
    float4x4 viewProj;         // camera-relative position -> clip of this frame (with the TAA jitter)
    float4x4 previousViewProj; // camera-relative position (this frame's camera) -> clip of the previous frame's image
    // The ambient occlusion's history: the same position -> clip of this frame and of the frame the history is from,
    // both without the TAA jitter, so that a still camera reads each texel's own history.
    float4x4 occlusionViewProj;
    float4x4 occlusionHistoryViewProj;
};

QUENCH_SHARED_END

#endif
