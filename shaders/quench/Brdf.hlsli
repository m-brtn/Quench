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

#endif
