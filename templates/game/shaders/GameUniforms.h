#ifndef TEMPLATE_GAME_UNIFORMS_H
#define TEMPLATE_GAME_UNIFORMS_H

// Uniform blocks shared by src/Game.cpp and shaders/game.hlsl. Members are 16-byte types only (float4, float4x4).

#include "quench/Shared.h"

QUENCH_SHARED_BEGIN

struct QUENCH_ALIGN16 GameFrame {
    float4x4 viewProj;
    float4 lightDir;   // xyz: unit direction towards the light
    float4 lightColor; // rgb, linear
    float4 ambient;    // rgb, linear
    float4 options;    // x: 1 when the target is UNORM (the shader encodes sRGB)
};

struct QUENCH_ALIGN16 GameDraw {
    float4x4 model; // rotation, translation and uniform scale (normals use it too)
    float4 color;   // rgb: linear albedo; w: 1 for the grid ground
};

QUENCH_SHARED_END

#endif
