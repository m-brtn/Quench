// The local lights' shadow maps read in screen space (docs/render.md, "Local light shadow maps"), in frames where an
// area light has maps: the soft filter of quenchAreaShadow is too costly per surface fragment. local_shadow_fs: per
// 2 x 2 pixels (the top-left one, as the screen-space effects' terms), the shadow of every light with maps in its
// channel (LocalLight::cone.y = channel + 1), at the prepass depth's position and its normal from the depth.
// local_shadow_blur_fs: at full resolution, the 3 x 3 term texels around the pixel's own, each weighted by how well
// its depth continues the pixel's depth gradient (as the GTAO blur, screenspace.hlsl) and by its distance, into the
// target the surfaces read as they read traced local shadows (sampler 9, FrameUniforms::options.w bit 2). The taps
// turn per texel by interleaved gradient noise, which a 3 x 3 average evens out (and per frame under TAA).
//
// Bindings (fragment): uniform 0 FrameUniforms, uniform 1 LightUniforms. local_shadow_fs: sampler 0 the prepass
// depth, sampler 1 the shadow maps. local_shadow_blur_fs: sampler 0 the term (half resolution), sampler 1 the depth.

#include "quench/Bindings.hlsli"
#include "quench/Frame.hlsli"
#include "quench/Fullscreen.hlsli"
#include "quench/SceneUniforms.h"

#include "RayCommon.hlsli"

#if defined(QUENCH_ENTRY_local_shadow_fs)
#include "quench/Shadows.hlsli"

cbuffer LocalShadowLightBlock : QUENCH_UNIFORM(1) {
    LightUniforms lights;
};
Texture2D<float> depthTex : QUENCH_TEXTURE(0);
SamplerState depthSampler : QUENCH_SAMPLER(0);
Texture2DArray<float> mapsTex : QUENCH_TEXTURE(1);
SamplerState mapsSampler : QUENCH_SAMPLER(1);

float4 local_shadow_fs(FullscreenOut vin) : SV_Target0 {
    const int2 h = int2(vin.position.xy);
    const int2 p = min(h * 2, int2(quenchFrame.screen.xy) - 1);
    const float d = depthTex.Load(int3(p, 0));
    if (d <= 0.0) return float4(1.0, 1.0, 1.0, 1.0);
    const float3 P = rtWorldAt(float2(p) + 0.5, d);
    const float3 N = rtDepthNormal(depthTex, p, P);
    float4 lit = float4(1.0, 1.0, 1.0, 1.0);
    const int count = int(lights.count.x);
    for (int k = 0; k < count; ++k) {
        const LocalLight light = lights.lights[k];
        if (light.cone.y <= 0.0) continue;
        const int c = int(light.cone.y) - 1;
        float v;
        [branch] if (int(light.color.w) >= QUENCH_LIGHT_RECT) {
            v = quenchAreaShadow(mapsTex, mapsSampler, light, P, N, float2(h));
        } else {
            v = quenchLocalShadow(mapsTex, mapsSampler, light, P, N);
        }
        lit = min(lit, float4(c == 0 ? v : 1.0, c == 1 ? v : 1.0, c == 2 ? v : 1.0, c == 3 ? v : 1.0));
    }
    return lit;
}
#endif

#if defined(QUENCH_ENTRY_local_shadow_blur_fs)
Texture2D<float4> termTex : QUENCH_TEXTURE(0);
SamplerState termSampler : QUENCH_SAMPLER(0);
Texture2D<float> depthTex : QUENCH_TEXTURE(1);
SamplerState depthSampler : QUENCH_SAMPLER(1);

// The depth tolerance relative to the reversed depth (the GTAO blur's), and the spread of the distance weights (pixels).
static const float kTolerance = 0.02;
static const float kSigma = 2.0;

float4 local_shadow_blur_fs(FullscreenOut vin) : SV_Target0 {
    const int2 p = int2(vin.position.xy);
    const int2 last = int2(quenchFrame.screen.xy) - 1;
    const float d = depthTex.Load(int3(p, 0));
    if (d <= 0.0) return float4(1.0, 1.0, 1.0, 1.0);
    const float dr = depthTex.Load(int3(min(p + int2(1, 0), last), 0)) - d;
    const float dl = d - depthTex.Load(int3(max(p - int2(1, 0), int2(0, 0)), 0));
    const float dd = depthTex.Load(int3(min(p + int2(0, 1), last), 0)) - d;
    const float du = d - depthTex.Load(int3(max(p - int2(0, 1), int2(0, 0)), 0));
    const float2 gradient = float2(abs(dr) < abs(dl) ? dr : dl, abs(dd) < abs(du) ? dd : du);
    const float invTolerance = 1.0 / (kTolerance * d);
    const float falloff = -0.5 / (kSigma * kSigma);
    const int2 first = (p >> 1) - 1;
    const int2 halfLast = last >> 1;
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float weight = 0.0;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x) {
            const int2 t = clamp(first + int2(x, y), int2(0, 0), halfLast);
            const int2 q = min(t * 2, last);
            const float2 offset = float2(q - p);
            const float texelDepth = depthTex.Load(int3(q, 0));
            const float expected = d + dot(offset, gradient);
            const float w = texelDepth > 0.0 ? saturate(1.0 - abs(texelDepth - expected) * invTolerance) *
                                                   exp(dot(offset, offset) * falloff)
                                             : 0.0;
            sum += termTex.Load(int3(t, 0)) * w;
            weight += w;
        }
    return weight > 0.0 ? sum / weight : termTex.Load(int3(p >> 1, 0));
}
#endif
