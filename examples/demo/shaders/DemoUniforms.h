#ifndef QUENCH_DEMO_UNIFORMS_H
#define QUENCH_DEMO_UNIFORMS_H

// Uniform blocks of the demo's own pass (markers.hlsl), read by MaterialsSection.cpp.

#include "quench/Shared.h"

#define DEMO_MAX_MARKERS 8

QUENCH_SHARED_BEGIN

// Vertex uniform slot 1 of marker_vs: camera-facing discs drawn additively into the HDR image.
struct QUENCH_ALIGN16 MarkerUniforms {
    float4 right;                      // xyz: camera right (world)
    float4 up;                         // xyz: camera up (world)
    float4 position[DEMO_MAX_MARKERS]; // xyz: center, w: radius (m)
    float4 color[DEMO_MAX_MARKERS];    // rgb: linear HDR radiance
};

QUENCH_SHARED_END

#endif
