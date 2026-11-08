#ifndef QUENCH_RENDER_PARTICLE_UNIFORMS_H
#define QUENCH_RENDER_PARTICLE_UNIFORMS_H

// Uniform block of the particles' and trails' draws (src/render/shaders/particles.hlsl), filled by
// src/render/Particles.cpp. The step's block is ParticleUniforms (quench/ParticleField.h).

#include "quench/Shared.h"

// ParticleDrawUniforms::draw.y
#define QUENCH_PARTICLE_SPRITE_GLOW 0
#define QUENCH_PARTICLE_SPRITE_SOFT 1
#define QUENCH_PARTICLE_SPRITE_STAR 2
#define QUENCH_PARTICLE_SPRITE_PUFF 3

QUENCH_SHARED_BEGIN

// Uniform slot 1 of the draws, vertex and fragment (slot 0 holds FrameUniforms).
struct QUENCH_ALIGN16 ParticleDrawUniforms {
    float4x4 viewProj; // world -> clip without the TAA jitter (they are drawn after TAA)
    float4 target;     // xy: the image's size (pixels), zw: 1 / it
    float4 camera;     // xyz: the camera's position (m), w: the near plane (m; reversed depth: distance = near / z)
    float4 right;      // xyz: the camera's right, w: the focal length (pixels: a metre at 1 m of depth)
    float4 up;         // xyz: the camera's up
    // x: the first instance (or trail vertex) of the draw in its buffer, y: the sprite (QUENCH_PARTICLE_SPRITE_*),
    // z: 1 drawn along the motion (else facing the camera), w: the motion's exposure (s); trails: y the strip's
    // vertices
    float4 draw;
    // x: softness (m: fades out where it nears the scene behind it), y: near fade (m), z: least size (pixels),
    // w: 1 alpha-blended (covers what is behind), 0 additive
    float4 fade;
    float4 light; // x: 1 lit by the sun and the sky (alpha-blended only)
};

QUENCH_SHARED_END

#endif
