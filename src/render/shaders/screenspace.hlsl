// Screen-space effects of quench::render::Renderer, the fallbacks of the traced ones (docs/render.md, "Screen-space
// effects"): ss_gtao_fs and ss_gtao_blur_fs, ambient occlusion from the depth of the opaque surfaces;
// ss_reflect_fs and ss_reflect_resolve_fs, reflections of what is on the screen.
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
// strength, into alpha of the mask the surfaces read (sampler 6, FrameUniforms::options.w bit 1); rgb 1, or kept by
// the pipeline's write mask where the traced sun's visibility is there.
//
// ss_reflect_fs and ss_reflect_resolve_fs, reflections from the previous frame's image: the march, its refinement
// and the hit's confidence are moved from Ashfall's ssr_fs (github.com/m-brtn/strategy, commit 2d5db57,
// shaders/scene.hlsl; the same author), generalized: per 2 x 2 pixels a ray along the GGX lobe of the G-buffer's
// normal and roughness (Ashfall: the mirror direction only), camera-relative, its steps and the surfaces' thickness
// relative to the view depth (Ashfall: meters for its top-down camera), the hit's color from the previous frame's
// image at the hit's position in that frame (Ashfall composites after its scene pass and reads the same frame's).
// The resolve: the 4 x 4 term texels around the pixel, weighted by the depth gradient as the GTAO blur and by a
// distance falloff that widens with roughness, their colors also by 1 / (1 + brightness) (Karis's average, against
// single bright rays); rgb the reflected radiance, a its confidence, which the surfaces
// multiply into their traced reflections' weight (sampler 7, FrameUniforms::options.w bit 6).
//
// Bindings (fragment): uniform 0 FrameUniforms, uniform 1 ScreenSpaceUniforms. ss_gtao_fs: sampler 0 the prepass
// depth. ss_gtao_blur_fs: sampler 0 the term (r; g: its depth; half resolution), sampler 1 the
// depth. ss_reflect_fs: sampler 0 the prepass depth, sampler 1 the G-buffer's normal and roughness, sampler 2 the
// previous frame's image (linear). ss_reflect_resolve_fs: sampler 0 the term (half resolution), sampler 1 the depth,
// sampler 2 the G-buffer's normal and roughness.

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

#if defined(QUENCH_ENTRY_ss_reflect_fs)
#include "RayCommon.hlsli"

Texture2D<float> depthTex : QUENCH_TEXTURE(0);
SamplerState depthSampler : QUENCH_SAMPLER(0);
Texture2D<float4> normalTex : QUENCH_TEXTURE(1);
SamplerState normalSampler : QUENCH_SAMPLER(1);
Texture2D<float4> historyTex : QUENCH_TEXTURE(2);
SamplerState historySampler : QUENCH_SAMPLER(2);

// Radiance above this is clipped, as the traced reflections' is.
static const float kMaxReflectedRadiance = 64.0;

// The render uv of a camera-relative position under a view-projection, and its clip w (> 0 in front).
float3 ssProject(float4x4 m, float3 X) {
    const float4 c = mul(m, float4(X, 1.0));
    return float3(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5, c.w);
}

// Fades a hit out toward the screen's border.
float ssEdge(float2 uv) { return saturate(min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)) * 12.0); }

float4 ss_reflect_fs(FullscreenOut vin) : SV_Target0 {
    const int2 h = int2(vin.position.xy);
    const int2 p = min(h * 2, int2(quenchFrame.screen.xy) - 1);
    const float d = depthTex.Load(int3(p, 0));
    if (d <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    const float4 g = normalTex.Load(int3(p, 0));
    if (g.w >= ss.reflection.w || dot(g.xyz, g.xyz) <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    const float3 P = ssViewAt(float2(p) + 0.5, d);
    const float3 V = normalize(-P);
    const float3 N = normalize(g.xyz);
    uint seed = rtSeed(uint2(h), uint(ss.reflectionParams.x), 0x55e1u);
    const float2 u = float2(rtRandom(seed), rtRandom(seed));
    float3 R = reflect(-V, rtGgxVisibleNormal(N, V, max(g.w * g.w, 1e-3), u));
    if (dot(R, N) <= 0.0) R = reflect(-V, N);
    const float3 forward = quenchFrame.cameraFwd.xyz;
    const float near = quenchFrame.cameraFwd.w;
    const float z = near / d;
    const float3 origin = P + N * (0.002 * z);
    // The view depth the ray gains per meter: a ray toward the camera only passes behind what is in front of the
    // surfaces it would hit, which the screen does not show.
    const float depthRate = dot(R, forward);
    const float away = saturate(depthRate * 2.0 + 1.0);
    if (away <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    // Steps that grow along the ray, from a jittered start; the first surface the ray passes behind (by less than
    // its thickness, or the depth of two steps) is the hit. Clip coordinates are linear along the ray, and clip w is
    // the view depth.
    const float4 clipOrigin = mul(ss.viewProj, float4(origin, 1.0));
    const float4 clipStep = mul(ss.viewProj, float4(R, 0.0));
    const float2 size = quenchFrame.screen.xy;
    const float steps = ss.reflection.x;
    float stride = ss.reflection.y * z;
    float t = stride * (0.5 + rtRandom(seed)), last = 0.0, i = 0.0;
    bool hit = false;
    float2 hitUv = float2(0.0, 0.0);
    [loop] for (; i < steps; i += 1.0) {
        const float4 c = clipOrigin + clipStep * t;
        if (c.w <= 1e-4) break;
        const float2 uv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
        if (uv.x < 0.0 || uv.x >= 1.0 || uv.y < 0.0 || uv.y >= 1.0) break;
        const int2 q = int2(uv * size);
        const float sd = depthTex.Load(int3(q, 0));
        const float behind = c.w - near / max(sd, 1e-8); // the sky (0) is never in front
        if (behind > 0.0 && behind < max(ss.reflection.z * near / sd, 2.0 * stride * depthRate)) {
            // Not the surface the ray leaves: within a pixel of a grazing plane the ray seems behind it. The ray
            // stays above the tangent plane at its origin, so what it hits does too.
            if (dot(ssViewAt(float2(q) + 0.5, sd) - P, N) > 0.002 * z) {
                hit = true;
                hitUv = uv;
                break;
            }
        }
        last = t;
        t += stride;
        stride *= ss.reflectionParams.z;
    }
    if (!hit) return float4(0.0, 0.0, 0.0, 0.0);
    // Bisection between the last step in front and the first behind.
    float lo = last, hi = t;
    [unroll] for (int j = 0; j < 5; ++j) {
        const float mid = 0.5 * (lo + hi);
        const float4 c = clipOrigin + clipStep * mid;
        const float2 uv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
        const float sd = depthTex.Load(int3(clamp(int2(uv * size), int2(0, 0), int2(size) - 1), 0));
        if (sd > 0.0 && c.w > near / sd) {
            hi = mid;
            hitUv = uv;
        } else {
            lo = mid;
        }
    }
    const float3 Nh = normalTex.Load(int3(clamp(int2(hitUv * quenchFrame.screen.xy), int2(0, 0),
                                                int2(quenchFrame.screen.xy) - 1),
                                          0))
                          .xyz;
    // The back of the hit surface (seen from the ray) is not what the ray would see; rays toward the camera, hits
    // near the screen's border and near the end of the march fade out.
    const float facing = dot(Nh, Nh) > 0.0 ? saturate(-dot(normalize(Nh), R) * 4.0 + 0.4) : 0.0;
    const float far = saturate((steps - i) / (steps * 0.25));
    const float3 previous = ssProject(ss.previousViewProj, origin + R * hi);
    if (previous.z <= 1e-4) return float4(0.0, 0.0, 0.0, 0.0);
    const float confidence = facing * away * far * ssEdge(hitUv) * ssEdge(previous.xy);
    if (confidence <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    float3 color = historyTex.SampleLevel(historySampler, previous.xy, 0.0).rgb;
    const float l = max(max(color.r, color.g), color.b);
    if (l > kMaxReflectedRadiance) color *= kMaxReflectedRadiance / l;
    return float4(max(color, 0.0), confidence);
}
#endif

#if defined(QUENCH_ENTRY_ss_reflect_resolve_fs)
Texture2D<float4> termTex : QUENCH_TEXTURE(0);
SamplerState termSampler : QUENCH_SAMPLER(0);
Texture2D<float> depthTex : QUENCH_TEXTURE(1);
SamplerState depthSampler : QUENCH_SAMPLER(1);
Texture2D<float4> normalTex : QUENCH_TEXTURE(2);
SamplerState normalSampler : QUENCH_SAMPLER(2);

float4 ss_reflect_resolve_fs(FullscreenOut vin) : SV_Target0 {
    const int2 p = int2(vin.position.xy);
    const int2 last = int2(quenchFrame.screen.xy) - 1;
    const float d = depthTex.Load(int3(p, 0));
    if (d <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    const float roughness = normalTex.Load(int3(p, 0)).w;
    if (roughness >= ss.reflection.w) return float4(0.0, 0.0, 0.0, 0.0);
    // The depth gradient, as in ss_gtao_blur_fs.
    const float dr = depthTex.Load(int3(min(p + int2(1, 0), last), 0)) - d;
    const float dl = d - depthTex.Load(int3(max(p - int2(1, 0), int2(0, 0)), 0));
    const float dd = depthTex.Load(int3(min(p + int2(0, 1), last), 0)) - d;
    const float du = d - depthTex.Load(int3(max(p - int2(0, 1), int2(0, 0)), 0));
    const float2 gradient = float2(abs(dr) < abs(dl) ? dr : dl, abs(dd) < abs(du) ? dd : du);
    const float invTolerance = 1.0 / (ss.reflectionParams.y * d);
    // The lobe's spread on the screen grows with roughness (pixels); a mirror's reflection stays sharp.
    const float sigma = 0.8 + 6.0 * roughness;
    const float falloff = -0.5 / (sigma * sigma);
    const int2 first = (p >> 1) - 2 + (p & 1);
    const int2 halfLast = last >> 1;
    // Each ray's color counts less the brighter it is (a Karis average): a ray of a rough surface that finds a lamp
    // does not stand out as a square of the ray's texel.
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float weight = 0.0, colorWeight = 0.0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const int2 t = clamp(first + int2(x, y), int2(0, 0), halfLast);
            const int2 q = min(t * 2, last);
            const float2 offset = float2(q - p);
            const float texelDepth = depthTex.Load(int3(q, 0));
            const float expected = d + dot(offset, gradient);
            const float w = texelDepth > 0.0 ? saturate(1.0 - abs(texelDepth - expected) * invTolerance) *
                                                   exp(dot(offset, offset) * falloff)
                                             : 0.0;
            const float4 term = termTex.Load(int3(t, 0));
            const float k = term.a * w / (1.0 + max(max(term.r, term.g), term.b));
            sum += float4(term.rgb * k, term.a * w);
            colorWeight += k;
            weight += w;
        }
    if (weight <= 0.0 || sum.a <= 1e-5 || colorWeight <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    return float4(sum.rgb / colorWeight, sum.a / weight);
}
#endif
