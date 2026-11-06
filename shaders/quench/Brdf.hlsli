#ifndef QUENCH_BRDF_HLSLI
#define QUENCH_BRDF_HLSLI

// Shading terms. ggxSpec and envBRDFApprox were identical in TrailForge and Ashfall.

#include "quench/Common.hlsli"

// GGX distribution with the Schlick-GGX geometry term (k from Karis 2013), divided by 4 N.V N.L.
float ggxSpec(float NdotH, float NdotV, float NdotL, float rough) {
    const float a = max(rough * rough, 0.002);
    const float a2 = a * a;
    const float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    const float D = a2 / (QUENCH_PI * d * d);
    const float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    const float G = NdotV / (NdotV * (1.0 - k) + k) * NdotL / (NdotL * (1.0 - k) + k);
    return D * G / max(4.0 * NdotV * NdotL, 1e-4);
}

// Split-sum environment BRDF, analytic fit (Karis, "Physically Based Shading on Mobile").
float3 envBRDFApprox(float3 F0, float rough, float NdotV) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = rough * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    const float2 AB = float2(-1.04, 1.04) * a004 + r.zw;
    return F0 * AB.x + AB.y;
}

float3 fresnelSchlick(float3 F0, float VdotH) { return F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0); }

// The roughness (alpha) along the anisotropy direction (x) and across it (y) for a perceptual roughness and an
// anisotropy of 0..1, as KHR_materials_anisotropy.
float2 ggxAnisotropicAlpha(float rough, float anisotropy) {
    const float a = max(rough * rough, 0.002);
    return float2(lerp(a, 1.0, anisotropy * anisotropy), a);
}

// The frame of an anisotropic lobe: T along `direction` and B across it, both perpendicular to the normal N.
void ggxAnisotropicFrame(float3 N, float3 direction, out float3 T, out float3 B) {
    float3 t = direction - N * dot(N, direction);
    if (dot(t, t) < 1e-8) t = abs(N.y) < 0.99 ? cross(N, float3(0.0, 1.0, 0.0)) : cross(N, float3(1.0, 0.0, 0.0));
    T = normalize(t);
    B = cross(N, T);
}

// Anisotropic GGX (Burley 2012) with the height-correlated Smith visibility (Heitz 2014), divided by 4 N.V N.L like
// ggxSpec: T along the anisotropy direction, B across it (unit, perpendicular to N), alpha from ggxAnisotropicAlpha.
float ggxSpecAnisotropic(float3 N, float3 T, float3 B, float3 V, float3 L, float2 alpha) {
    const float3 H = normalize(V + L);
    const float NdotV = max(dot(N, V), 1e-3);
    const float NdotL = max(dot(N, L), 1e-4);
    const float a2 = alpha.x * alpha.y;
    const float3 d = float3(alpha.y * dot(T, H), alpha.x * dot(B, H), a2 * dot(N, H));
    const float w2 = a2 / max(dot(d, d), 1e-12);
    const float D = a2 * w2 * w2 / QUENCH_PI;
    const float lambdaV = NdotL * length(float3(alpha.x * dot(T, V), alpha.y * dot(B, V), NdotV));
    const float lambdaL = NdotV * length(float3(alpha.x * dot(T, L), alpha.y * dot(B, L), NdotL));
    return D * 0.5 / (lambdaV + lambdaL);
}

// The normal toward which an anisotropic lobe bends the reflection of the sky and the area lights (the lobe is wide
// along T): the normal turned into the plane across B that holds V, by how far the lobe is stretched.
float3 ggxAnisotropicNormal(float3 N, float3 B, float3 V, float rough, float anisotropy) {
    const float3 bent = normalize(cross(cross(B, V), B));
    const float k = 1.0 - anisotropy * (1.0 - rough);
    return normalize(lerp(bent, N, k * k * k * k));
}

#endif
