#ifndef QUENCH_SHADOWS_HLSLI
#define QUENCH_SHADOWS_HLSLI

// The sun's cascaded shadow maps: one layer of a Texture2DArray per cascade, standard depth (0 near the light). From
// TrailForge's shadowFactor, without its far terrain layer. Each tap gathers the 2x2 footprint and compares in the
// shader with bilinear weights (the result of a LessOrEqual comparison sampler), so the sampler is a plain one:
// MoltenVK cannot bind comparison samplers through SDL GPU.

#include "quench/Frame.hlsli"

// 1 = lit: z <= the depth in the map, filtered over the four nearest texels.
float quenchShadowTap(Texture2DArray<float> sm, SamplerState s, float2 uv, float layer, float z) {
    const float size = 1.0 / quenchFrame.shadowParams.y;
    const float2 t = uv * size - 0.5;
    const float2 f = frac(t);
    // The shared corner of the footprint, so that Gather picks the same four texels as the weights.
    const float4 d = sm.Gather(s, float3((floor(t) + 1.0) / size, layer));
    const float4 lit = step(z, d); // x (0,1), y (1,1), z (1,0), w (0,0)
    return lerp(lerp(lit.w, lit.z, f.x), lerp(lit.x, lit.y, f.x), f.y);
}

// 1 = lit. P and N in world space; maxFilter caps FrameUniforms::shadowParams.z (0: one tap, 1: four, 2: twelve).
float shadowFactor(Texture2DArray<float> sm, SamplerState s, float3 P, float3 N, int maxFilter = 2) {
    const int cascades = int(quenchFrame.shadowParams.x);
    if (cascades <= 0) return 1.0;
    const float viewDepth = quenchViewDepth(P);
    int c = 0;
    for (; c < cascades; ++c)
        if (viewDepth < quenchFrame.cascadeSplits[c]) break;
    if (c >= cascades) return 1.0;
    const float texel = quenchFrame.shadowParams.y;
    // Normal offset scales with the world size of one shadow texel in this cascade.
    const float3 Pn = P + N * quenchFrame.cascadeTexel[c] * quenchFrame.shadowParams.w;
    const float4 sc = mul(quenchFrame.shadowViewProj[c], float4(Pn, 1.0));
    float2 uv = sc.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    const float z = sc.z;
    if (any(uv < 0.0) || any(uv > 1.0) || z > 1.0) return 1.0;
    const float layer = float(c);
    const int filterMode = min(int(quenchFrame.shadowParams.z), maxFilter);
    float lit = 0.0;
    if (filterMode == 0) {
        lit = quenchShadowTap(sm, s, uv, layer, z);
    } else if (filterMode == 1) {
        const float2 o = ((float2)(texel)) * 1.2;
        lit = quenchShadowTap(sm, s, uv + float2(-o.x, -o.y), layer, z) +
              quenchShadowTap(sm, s, uv + float2(o.x, -o.y), layer, z) +
              quenchShadowTap(sm, s, uv + float2(-o.x, o.y), layer, z) +
              quenchShadowTap(sm, s, uv + float2(o.x, o.y), layer, z);
        lit *= 0.25;
    } else {
        const float2 taps[12] = {float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696, 0.457),
                                 float2(-0.203, 0.621),  float2(0.962, -0.195),  float2(0.473, -0.480),
                                 float2(0.519, 0.767),   float2(0.185, -0.893),  float2(0.507, 0.064),
                                 float2(0.896, 0.412),   float2(-0.322, -0.933), float2(-0.792, -0.598)};
        for (int i = 0; i < 12; ++i) lit += quenchShadowTap(sm, s, uv + taps[i] * texel * 2.2, layer, z);
        lit /= 12.0;
    }
    // Fade to lit over the last 15 % of the last cascade.
    const float end = quenchFrame.cascadeSplits[cascades - 1];
    const float fade = saturate((viewDepth - end * 0.85) / (end * 0.15));
    return lerp(lit, 1.0, fade);
}

#endif
