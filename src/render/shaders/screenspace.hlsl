// Screen-space effects of quench::render::Renderer, the fallbacks of the traced ones (docs/render.md, "Screen-space
// effects"): ss_gtao_fs and ss_gtao_blur_fs, ambient occlusion from the depth of the opaque surfaces.
//
// ss_gtao_fs is ground-truth ambient occlusion (Jimenez et al. 2016, "Practical Real-Time Strategies for Accurate
// Indirect Occlusion", Algorithm 1) adapted from XeGTAO's XeGTAO_MainPass (https://github.com/GameTechDev/XeGTAO,
// commit a5b1686, XeGTAO.hlsli):
//
//   Copyright (C) 2016-2021, Intel Corporation
//   SPDX-License-Identifier: MIT
//
// (the licence text is in THIRD_PARTY_LICENSES). Changes from it: at half resolution (the top-left pixel of every
// 2 x 2 block, as Ashfall's screen-space effects do), positions from the reversed depth along the camera rays of
// ScreenSpaceUniforms, normals from the depth, no depth MIP chain (full-resolution loads, the screen radius capped
// instead), a 4 x 4 noise tile over the R2 sequence in place of the Hilbert curve, no bent normals, the term in 16 bits
// without XeGTAO's packing. XeGTAO's default constants: radius multiplier 1.457 (applied by the renderer),
// falloff range 0.615, sample distribution power 2, final power 2.2; its fast acos (Lagarde's).
//
// ss_gtao_blur_fs: at full resolution, the 4 x 4 term texels around the pixel (one period of the noise tile), each
// weighted by how well its depth (kept next to the term) continues the pixel's depth gradient (other surfaces do not bleed in), with the
// strength, into alpha of the mask the surfaces read (sampler 6, FrameUniforms::options.w bit 2); rgb 1, or kept by
// the pipeline's write mask where the traced sun's visibility is there.
//
// Bindings (fragment): uniform 0 FrameUniforms, uniform 1 ScreenSpaceUniforms. ss_gtao_fs: sampler 0 the prepass
// depth. ss_gtao_blur_fs: sampler 0 the term (r; g: its depth; half resolution), sampler 1 the
// depth.

#include "quench/Bindings.hlsli"
#include "quench/Common.hlsli"
#include "quench/Frame.hlsli"
#include "quench/Fullscreen.hlsli"
#include "quench/SceneUniforms.h"

#include "ScreenSpaceUniforms.h"

cbuffer ScreenSpaceBlock : QUENCH_UNIFORM(1) {
    ScreenSpaceUniforms ss;
};

// The position at render pixel coordinates `pixel` and reversed depth d, relative to the camera.
float3 ssViewAt(float2 pixel, float d) {
    return (ss.rayOrigin.xyz + pixel.x * ss.rayX.xyz + pixel.y * ss.rayY.xyz) / d;
}

#if defined(QUENCH_ENTRY_ss_gtao_fs)
Texture2D<float> depthTex : QUENCH_TEXTURE(0);
SamplerState depthSampler : QUENCH_SAMPLER(0);

static const float kPi = 3.14159265;
static const float kHalfPi = 1.57079633;

// The position at pixel q (clamped to the target), the sky as far away.
float3 ssPositionAt(int2 q) {
    q = clamp(q, int2(0, 0), int2(quenchFrame.screen.xy) - 1);
    return ssViewAt(float2(q) + 0.5, max(depthTex.Load(int3(q, 0)), 1e-7));
}

// XeGTAO_FastACos: input [-1, 1], output [0, pi].
float ssAcos(float x) {
    const float a = min(abs(x), 1.0);
    const float r = (-0.156583 * a + kHalfPi) * sqrt(1.0 - a);
    return x >= 0.0 ? r : kPi - r;
}

float4 ss_gtao_fs(FullscreenOut vin) : SV_Target0 {
    const int2 h = int2(vin.position.xy);
    const int2 p = min(h * 2, int2(quenchFrame.screen.xy) - 1);
    const float d = depthTex.Load(int3(p, 0));
    if (d <= 0.0) return float4(1.0, 0.0, 0.0, 1.0);
    const float3 surface = ssViewAt(float2(p) + 0.5, d);
    // The normal from the depth: the smaller difference on each axis keeps edges from bending it.
    const float3 R = ssPositionAt(p + int2(1, 0)) - surface, L = surface - ssPositionAt(p - int2(1, 0));
    const float3 D = ssPositionAt(p + int2(0, 1)) - surface, U = surface - ssPositionAt(p - int2(0, 1));
    const float3 dx = dot(R, R) < dot(L, L) && dot(R, R) > 0.0 ? R : L;
    const float3 dy = dot(D, D) < dot(U, U) && dot(D, D) > 0.0 ? D : U;
    float3 N = cross(dy, dx);
    N *= rsqrt(max(dot(N, N), 1e-30));
    if (dot(N, surface) > 0.0) N = -N;
    // XeGTAO moves the center a little toward the camera (its 0.99999 for 32-bit depth): samples on its own plane
    // stay under the horizon.
    const float3 P = surface * 0.99999;
    const float3 V = normalize(-P);
    const float sliceCount = ss.occlusion.x, stepsPerSlice = ss.occlusion.y;
    const float effectRadius = ss.occlusion.z;
    const float falloffRange = 0.615 * effectRadius;
    const float falloffFrom = effectRadius * (1.0 - 0.615);
    const float falloffMul = -1.0 / falloffRange;
    const float falloffAdd = falloffFrom / falloffRange + 1.0;
    // A pixel's size in the world at the center's depth.
    const float pixelSize = length(ss.rayX.xyz) / d;
    const float screenRadius = min(effectRadius / max(pixelSize, 1e-6), ss.occlusionParams.y);
    // Fade out for small screen radii.
    float visibility = saturate((10.0 - screenRadius) / 100.0) * 0.5;
    // The closest step leaves the center pixel.
    const float minS = 1.3 / screenRadius;
    const uint tile = uint(h.x & 3) | (uint(h.y & 3) << 2u);
    const float2 noise = frac(0.5 + float(tile + 16u * uint(ss.occlusionParams.x)) * float2(0.7548776662, 0.5698402910));

    for (float slice = 0.0; slice < sliceCount; slice += 1.0) {
        const float phi = (slice + noise.x) / sliceCount * kPi;
        const float2 omega = float2(cos(phi), sin(phi)); // pixels, y down
        // The slice's direction in the world: toward the pixel one step along omega at the center's depth.
        const float3 directionVec = normalize(omega.x * ss.rayX.xyz + omega.y * ss.rayY.xyz);
        const float3 orthoDirectionVec = directionVec - dot(directionVec, V) * V;
        const float3 axisVec = normalize(cross(orthoDirectionVec, V));
        const float3 projectedNormalVec = N - axisVec * dot(N, axisVec);
        const float signNorm = dot(orthoDirectionVec, projectedNormalVec) < 0.0 ? -1.0 : 1.0;
        float projectedNormalVecLength = length(projectedNormalVec);
        const float cosNorm = saturate(dot(projectedNormalVec, V) / max(projectedNormalVecLength, 1e-6));
        const float n = signNorm * ssAcos(cosNorm);
        const float sinN = sin(n);
        // cos(n + pi / 2) and cos(n - pi / 2)
        const float lowHorizonCos0 = -sinN;
        const float lowHorizonCos1 = sinN;
        float horizonCos0 = lowHorizonCos0;
        float horizonCos1 = lowHorizonCos1;
        for (float step = 0.0; step < stepsPerSlice; step += 1.0) {
            // R1 sequence.
            const float stepNoise = frac(noise.y + (slice + step * stepsPerSlice) * 0.6180339887);
            float s = (step + stepNoise) / stepsPerSlice;
            s = s * s + minS;
            const int2 offset = int2(round(s * screenRadius * omega));
            const float3 delta0 = ssPositionAt(p + offset) - P;
            const float3 delta1 = ssPositionAt(p - offset) - P;
            const float dist0 = length(delta0), dist1 = length(delta1);
            // Samples beyond the radius fade out of the horizon.
            const float weight0 = saturate(dist0 * falloffMul + falloffAdd);
            const float weight1 = saturate(dist1 * falloffMul + falloffAdd);
            const float shc0 = lerp(lowHorizonCos0, dot(delta0, V) / max(dist0, 1e-6), weight0);
            const float shc1 = lerp(lowHorizonCos1, dot(delta1, V) / max(dist1, 1e-6), weight1);
            horizonCos0 = max(horizonCos0, shc0);
            horizonCos1 = max(horizonCos1, shc1);
        }
        // XeGTAO's fudge for the slight overdarkening on high slopes.
        projectedNormalVecLength = lerp(projectedNormalVecLength, 1.0, 0.05);
        const float h0 = -ssAcos(horizonCos1);
        const float h1 = ssAcos(horizonCos0);
        const float iarc0 = (cosNorm + 2.0 * h0 * sinN - cos(2.0 * h0 - n)) / 4.0;
        const float iarc1 = (cosNorm + 2.0 * h1 * sinN - cos(2.0 * h1 - n)) / 4.0;
        visibility += projectedNormalVecLength * (iarc0 + iarc1);
    }
    visibility /= sliceCount;
    visibility = pow(saturate(visibility), 2.2);
    return float4(max(0.03, visibility), d, 0.0, 1.0);
}
#endif

#if defined(QUENCH_ENTRY_ss_gtao_blur_fs)
Texture2D<float4> termTex : QUENCH_TEXTURE(0);
SamplerState termSampler : QUENCH_SAMPLER(0);
Texture2D<float> depthTex : QUENCH_TEXTURE(1);
SamplerState depthSampler : QUENCH_SAMPLER(1);

float4 ss_gtao_blur_fs(FullscreenOut vin) : SV_Target0 {
    const int2 p = int2(vin.position.xy);
    const int2 last = int2(quenchFrame.screen.xy) - 1;
    const float d = depthTex.Load(int3(p, 0));
    if (d <= 0.0) return float4(1.0, 1.0, 1.0, 1.0);
    // A reversed depth is linear over the screen on a plane: its gradient (the smaller difference on each axis) says
    // what a texel on the pixel's surface would hold.
    const float dr = depthTex.Load(int3(min(p + int2(1, 0), last), 0)) - d;
    const float dl = d - depthTex.Load(int3(max(p - int2(1, 0), int2(0, 0)), 0));
    const float dd = depthTex.Load(int3(min(p + int2(0, 1), last), 0)) - d;
    const float du = d - depthTex.Load(int3(max(p - int2(0, 1), int2(0, 0)), 0));
    const float2 gradient = float2(abs(dr) < abs(dl) ? dr : dl, abs(dd) < abs(du) ? dd : du);
    const float invTolerance = 1.0 / (ss.occlusionParams.z * d);
    // The 4 x 4 texels around the pixel (the half-resolution texel of p is the second or third of each row).
    const int2 first = (p >> 1) - 2 + (p & 1);
    const int2 halfLast = last >> 1;
    float sum = 0.0, weight = 0.0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const int2 t = clamp(first + int2(x, y), int2(0, 0), halfLast);
            const float2 term = termTex.Load(int3(t, 0)).rg;
            const float expected = d + dot(float2(min(t * 2, last) - p), gradient);
            const float w = term.g > 0.0 ? saturate(1.0 - abs(term.g - expected) * invTolerance) : 0.0;
            sum += term.r * w;
            weight += w;
        }
    const float occlusion = weight > 0.0 ? sum / weight : termTex.Load(int3(p >> 1, 0)).r;
    return float4(1.0, 1.0, 1.0, lerp(1.0, occlusion, ss.occlusion.w));
}
#endif
