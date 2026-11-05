#ifndef QUENCH_HELLO_UNIFORMS_H
#define QUENCH_HELLO_UNIFORMS_H

// Uniform blocks of the hello example, read by Hello.cpp and hello.hlsl.

#include "quench/Shared.h"

QUENCH_SHARED_BEGIN

struct QUENCH_ALIGN16 HelloFrame {
    float4x4 viewProj;
    float4 cameraPos;  // xyz
    float4 lightDir;   // xyz: unit direction towards the light
    float4 lightColor; // rgb, linear
    float4 fogColor;   // rgb, linear; w: density (1/m)
    float4 options;    // x: 1 when the target is UNORM (the shader encodes sRGB)
};

struct QUENCH_ALIGN16 HelloDraw {
    float4x4 model; // rotation and translation only (normals use it too)
    float4 color;   // rgb: linear albedo; w: 1 for the checkered ground
    float4 shadow;  // xz: center of the blob shadow on the ground, w: radius (m)
};

QUENCH_SHARED_END

#endif
