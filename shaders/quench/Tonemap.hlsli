#ifndef QUENCH_TONEMAP_HLSLI
#define QUENCH_TONEMAP_HLSLI

// Tone curves from linear HDR to display values. aces was identical in TrailForge and Ashfall; agx and the reversible
// tmap / itmap are TrailForge's.

#include "quench/Common.hlsli"

// The fitted ACES curve (Narkowicz 2015). Returns linear display values in 0..1; it expects scene values scaled by
// about 0.6 (QUENCH_ACES_PRE_EXPOSURE).
static const float QUENCH_ACES_PRE_EXPOSURE = 0.6;
float3 aces(float3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// AgX (Sobotka) with the "punchy" look, polynomial fit of the base contrast curve (Wrensch). Returns display values
// (encoded, about gamma 2.2).
float3 agxContrast(float3 x) {
    const float3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 agx(float3 c) {
    // HLSL float3x3 rows are the columns of the reference GLSL mat3, so mul(v, M) == M * v there.
    const float3x3 inset = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                    0.0784335999999992, 0.878468636469772, 0.0784336,
                                    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 outset = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                     -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                     -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    float3 v = mul(max(c, 1e-10), inset);
    v = (clamp(log2(v), minEv, maxEv) - minEv) / (maxEv - minEv);
    v = saturate(agxContrast(v));
    v = pow(v, ((float3)(1.35)));
    const float l = luminance(v);
    v = l + 1.4 * (v - l);
    return saturate(mul(v, outset));
}

// ---- HDR output (quench/render/ToneMap.h has the same functions in C++, tested against these)

// Linear display value where the HDR shoulder leaves the SDR curve (kHdrKnee).
static const float QUENCH_HDR_KNEE = 0.6;

// Linear display values 0..1 of the SDR curve (agxCurve > 0.5: AgX, else ACES) for an exposed scene color.
float3 quenchToneMapSdr(float3 c, float agxCurve) {
    return agxCurve > 0.5 ? pow(agx(c), ((float3)(2.2))) : aces(c * QUENCH_ACES_PRE_EXPOSURE);
}

float quenchMax3(float3 c) { return max(c.r, max(c.g, c.b)); }

// Linear display values for an extended-linear target, paper white at 1, at most `headroom`: the SDR curve while the
// largest channel is below the knee (kneeScene), above it an exponential shoulder that starts with the curve's slope
// there (kneeSlope) and approaches the headroom; the color is scaled by the shoulder over the SDR curve, so hue and
// saturation stay the curve's.
float3 quenchToneMapHdr(float3 c, float agxCurve, float headroom, float kneeScene, float kneeSlope) {
    const float3 sdr = quenchToneMapSdr(c, agxCurve);
    const float s = quenchMax3(c);
    if (headroom <= 1.0 || s <= kneeScene) return sdr;
    const float room = headroom - QUENCH_HDR_KNEE;
    const float target = QUENCH_HDR_KNEE + room * (1.0 - exp(-kneeSlope * (s - kneeScene) / room));
    const float curve = quenchMax3(quenchToneMapSdr(((float3)(s)), agxCurve));
    return min(sdr * (target / max(curve, 1e-4)), ((float3)(headroom)));
}

// A reversible tone-mapped domain for neighbourhood filters (TAA, sharpening): bright pixels do not dominate.
float3 tmap(float3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
float3 itmap(float3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }

#endif
