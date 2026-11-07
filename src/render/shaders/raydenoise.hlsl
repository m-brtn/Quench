// The temporal denoiser of the renderer's traced signals (render::RayDenoiser::Temporal; docs/render.md, "Denoising"):
// rt_temporal_fs blends each pixel's new samples into its history, reprojected by the motion vectors and dropped
// where the depth there does not match (disocclusion), with the history clamped to the spread of the new samples
// around the pixel (moving shadows and reflections do not trail); rt_atrous_fs then blurs it in a-trous passes
// (3 x 3 taps at growing steps) weighted by the distance of each tap to the pixel's plane, so edges stay sharp.
// rt_temporal_deviation_fs is rt_temporal_fs that also keeps each pixel's standard deviation over its history and
// clamps to no less than it (the per-pixel indirect light: one ray per pixel). Without variance guidance: the blur
// does not adapt to how noisy each pixel still is.
//
// Bindings (fragment): uniform 0 FrameUniforms, uniform 1 RayDenoiseUniforms. rt_temporal_fs: samplers 0 the new
// signal, 1 its history, 2 the history length (r), 3 the motion vectors, 4 the depth, 5 the previous frame's depth;
// out: the signal and its history length. rt_temporal_deviation_fs: also sampler 6 the history's standard deviation,
// out 2 its new one. rt_atrous_fs: samplers 0 the signal, 1 the depth.

#include "quench/Bindings.hlsli"
#include "quench/Fullscreen.hlsli"
#include "quench/SceneUniforms.h"

#include "RayCommon.hlsli"
#include "RayTracingUniforms.h"

cbuffer RayDenoiseBlock : QUENCH_UNIFORM(1) {
    RayDenoiseUniforms rd;
};

#if defined(QUENCH_ENTRY_rt_temporal_deviation_fs)
#define QUENCH_RT_DEVIATION
#endif

#if defined(QUENCH_ENTRY_rt_temporal_fs) || defined(QUENCH_RT_DEVIATION)
Texture2D<float4> signalTex : QUENCH_TEXTURE(0);
SamplerState pointSampler : QUENCH_SAMPLER(0);
Texture2D<float4> historyTex : QUENCH_TEXTURE(1);
SamplerState linearSampler : QUENCH_SAMPLER(1);
Texture2D<float4> lengthTex : QUENCH_TEXTURE(2);
SamplerState lengthSampler : QUENCH_SAMPLER(2);
Texture2D<float4> motionTex : QUENCH_TEXTURE(3);
SamplerState motionSampler : QUENCH_SAMPLER(3);
Texture2D<float> depthTex : QUENCH_TEXTURE(4);
SamplerState depthSampler : QUENCH_SAMPLER(4);
Texture2D<float> previousDepthTex : QUENCH_TEXTURE(5);
SamplerState previousDepthSampler : QUENCH_SAMPLER(5);
#if defined(QUENCH_RT_DEVIATION)
Texture2D<float4> deviationTex : QUENCH_TEXTURE(6);
SamplerState deviationSampler : QUENCH_SAMPLER(6);
#endif

struct TemporalOut {
    float4 signal : SV_Target0;
    float4 length : SV_Target1;
#if defined(QUENCH_RT_DEVIATION)
    float4 deviation : SV_Target2;
#endif
};

TemporalOut temporal(FullscreenOut vin) {
    const int2 p = int2(vin.position.xy);
    const float4 current = signalTex.Load(int3(p, 0));
    TemporalOut o;
    o.signal = current;
    o.length = float4(1.0, 0.0, 0.0, 1.0);
#if defined(QUENCH_RT_DEVIATION)
    o.deviation = float4(0.0, 0.0, 0.0, 0.0);
#endif
    const float d = depthTex.Load(int3(p, 0));
    if (d <= 0.0 || rd.temporal.w > 0.5) return o;

    // Where the surface was: the motion vectors are this frame's uv minus the previous frame's.
    const float2 uv = vin.position.xy * quenchFrame.screen.zw;
    const float2 previousUv = uv - motionTex.Load(int3(p, 0)).xy;
    if (any(previousUv < 0.0) || any(previousUv > 1.0)) return o;
    const float3 P = rtWorldAt(vin.position.xy, d);
    const float4 clip = mul(quenchFrame.prevViewProj, float4(P, 1.0));
    const float expected = clip.w; // the view distance the previous frame saw it at
    const float previousDepth = previousDepthTex.SampleLevel(depthSampler, previousUv, 0.0);
    if (previousDepth <= 0.0 || abs(rtViewDepth(previousDepth) - expected) > rd.temporal.y * expected) return o;

    // The spread of the new samples around the pixel: the history is clamped to their mean +- gamma sigma.
    const int2 last = int2(quenchFrame.screen.xy) - 1;
    float4 m1 = float4(0.0, 0.0, 0.0, 0.0), m2 = float4(0.0, 0.0, 0.0, 0.0);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            const float4 s = signalTex.Load(int3(clamp(p + int2(x, y), int2(0, 0), last), 0));
            m1 += s;
            m2 += s * s;
        }
    m1 /= 9.0;
    float4 history = historyTex.SampleLevel(linearSampler, previousUv, 0.0);
    const float historyLength = lengthTex.SampleLevel(lengthSampler, previousUv, 0.0).r;
#if defined(QUENCH_RT_DEVIATION)
    // The noise of one sample: the spread of the 3 x 3 new samples, but at least the pixel's own spread over its
    // history once it has a few frames. At one ray per pixel the 9 samples often agree by chance (all of them see the
    // sky, or none does) and their spread collapses; a clamp or anti-lag on it alone then pulls the history to their
    // mean, so the noise never averages out and the mean is biased. A moving edge widens the pixel's own spread too,
    // so the floor lets it trail more: it is for signals whose noise is larger than their changes.
    const float4 deviation = deviationTex.SampleLevel(deviationSampler, previousUv, 0.0);
    const float4 spread = sqrt(max(m2 / 9.0 - m1 * m1, 0.0));
    const float4 sigma = historyLength >= 4.0 ? max(spread, deviation) : spread;
    const float4 slack = 0.01 + 0.02 * abs(m1);
    // Anti-lag: where the neighbourhood's mean is farther from the history than three times its error (sigma / 3 for
    // a mean of 9 samples), the signal changed: the history is cut short, all of it from five errors on.
    const float4 error = sigma / 3.0 + slack;
    const float4 change = (abs(m1 - history) - 3.0 * error) / error;
    const float lag = saturate(max(max(change.r, change.g), max(change.b, change.a)) * 0.5);
    if (rd.filter.w > 0.0)
        history = clamp(history, m1 - rd.filter.w * sigma - slack, m1 + rd.filter.w * sigma + slack);
#else
    const float4 sigma = sqrt(max(m2 / 9.0 - m1 * m1, 0.0));
    // Anti-lag: where the neighbourhood's mean is farther from the history than its noise explains (the error of a
    // mean of 9 samples is sigma / 3), the signal changed (a caster moved, a light turned): the history is cut short.
    // The clamp alone misses it at thin features, whose neighbourhood spans both values.
    const float4 change = abs(m1 - history) - (2.0 / 3.0) * sigma - 0.05;
    const float lag = saturate(max(max(change.r, change.g), max(change.b, change.a)) * 4.0);
    if (rd.filter.w > 0.0) history = clamp(history, m1 - rd.filter.w * sigma, m1 + rd.filter.w * sigma);
#endif

    const float maxLength = rd.temporal.z > 0.5 ? max(rd.temporal.x * current.a, 1.0) : rd.temporal.x;
    const float length = min(historyLength * (1.0 - lag) + 1.0, maxLength);
    o.signal = lerp(history, current, 1.0 / length);
    o.length = float4(length, 0.0, 0.0, 1.0);
#if defined(QUENCH_RT_DEVIATION)
    // The exponentially weighted variance with the same weights as the mean, kept as its square root (precise
    // enough in an 8-bit target).
    const float alpha = 1.0 / length;
    const float4 delta = current - history;
    o.deviation = sqrt((1.0 - alpha) * (deviation * deviation + alpha * delta * delta));
#endif
    return o;
}

#if defined(QUENCH_RT_DEVIATION)
TemporalOut rt_temporal_deviation_fs(FullscreenOut vin) { return temporal(vin); }
#else
TemporalOut rt_temporal_fs(FullscreenOut vin) { return temporal(vin); }
#endif
#endif

#if defined(QUENCH_ENTRY_rt_atrous_fs)
Texture2D<float4> signalTex : QUENCH_TEXTURE(0);
SamplerState pointSampler : QUENCH_SAMPLER(0);
Texture2D<float> depthTex : QUENCH_TEXTURE(1);
SamplerState depthSampler : QUENCH_SAMPLER(1);

float4 rt_atrous_fs(FullscreenOut vin) : SV_Target0 {
    const int2 p = int2(vin.position.xy);
    const float4 center = signalTex.Load(int3(p, 0));
    const float d = depthTex.Load(int3(p, 0));
    if (d <= 0.0) return center;
    const float scale = rd.filter.z > 0.5 ? center.a : 1.0;
    const int step = int(rd.filter.x * scale + 0.5);
    if (step < 1) return center;
    const float3 P = rtWorldAt(vin.position.xy, d);
    const float3 N = rtDepthNormal(depthTex, p, P);
    const float tolerance = rd.filter.y * rtViewDepth(d);
    const int2 last = int2(quenchFrame.screen.xy) - 1;
    const float kernel[3] = {0.25, 0.5, 0.25};
    float4 sum = center * 0.25;
    float weights = 0.25;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            if (x == 0 && y == 0) continue;
            const int2 q = clamp(p + int2(x, y) * step, int2(0, 0), last);
            const float dq = depthTex.Load(int3(q, 0));
            if (dq <= 0.0) continue;
            const float4 s = signalTex.Load(int3(q, 0));
            if (rd.filter.z > 0.5 && s.a <= 0.0) continue; // an untraced pixel (too rough to reflect)
            const float3 Pq = rtWorldAt(float2(q) + 0.5, dq);
            const float w = kernel[x + 1] * kernel[y + 1] * saturate(1.0 - abs(dot(Pq - P, N)) / tolerance);
            sum += s * w;
            weights += w;
        }
    return sum / weights;
}
#endif

#if defined(QUENCH_ENTRY_rt_denoise_inputs_fs)
// The G-buffer as the MetalFX denoising upscaler reads it (Upscaling::TemporalDenoised): the diffuse albedo, the
// specular albedo (the split-sum reflectance the surfaces use) and the roughness. Samplers 0 the depth, 1 the
// G-buffer's normal and roughness, 2 its albedo and metallic.
#include "quench/Brdf.hlsli"

Texture2D<float> depthTex : QUENCH_TEXTURE(0);
SamplerState depthSampler : QUENCH_SAMPLER(0);
Texture2D<float4> normalTex : QUENCH_TEXTURE(1);
SamplerState normalSampler : QUENCH_SAMPLER(1);
Texture2D<float4> albedoTex : QUENCH_TEXTURE(2);
SamplerState albedoSampler : QUENCH_SAMPLER(2);

struct DenoiseInputs {
    float4 diffuse : SV_Target0;
    float4 specular : SV_Target1;
    float4 roughness : SV_Target2;
};

DenoiseInputs rt_denoise_inputs_fs(FullscreenOut vin) {
    const int2 p = int2(vin.position.xy);
    DenoiseInputs o;
    const float d = depthTex.Load(int3(p, 0));
    if (d <= 0.0) { // the sky: lit as it is
        o.diffuse = float4(1.0, 1.0, 1.0, 1.0);
        o.specular = float4(0.0, 0.0, 0.0, 1.0);
        o.roughness = float4(1.0, 0.0, 0.0, 0.0);
        return o;
    }
    const float4 g = normalTex.Load(int3(p, 0));
    const float4 a = albedoTex.Load(int3(p, 0));
    const float3 P = rtWorldAt(vin.position.xy, d);
    const float3 V = normalize(quenchFrame.cameraPos.xyz - P);
    const float NdotV = saturate(dot(normalize(g.xyz), V));
    const float3 F0 = lerp(float3(0.04, 0.04, 0.04), a.rgb, a.a);
    o.diffuse = float4(a.rgb * (1.0 - a.a), 1.0);
    o.specular = float4(envBRDFApprox(F0, g.w, NdotV), 1.0);
    o.roughness = float4(g.w, 0.0, 0.0, 0.0);
    return o;
}
#endif
