#ifndef QUENCH_FOG_HLSLI
#define QUENCH_FOG_HLSLI

// Analytic height fog: density falling off exponentially above a base height, with sun scattering in its color.
// fogLayer was identical in TrailForge and Ashfall; fogAt and applyFog are TrailForge's without its valley layer.

#include "quench/Frame.hlsli"
#include "quench/Sky.hlsli"

// Optical depth of an exponential height layer (density a at y = 0, falloff b) along a ray from camPos.
float fogLayer(float a, float b, float3 camPos, float3 rd, float dist) {
    const float k = b * rd.y;
    const float camTerm = exp(-b * camPos.y);
    return abs(k) > 1e-4 ? a * camTerm * (1.0 - exp(-k * dist)) / k : a * camTerm * dist;
}

// rgb: fog color along the ray to worldPos, a: amount (0: clear).
float4 fogAt(float3 worldPos, float3 camPos) {
    const float3 d = worldPos - camPos;
    const float dist = length(d);
    const float density = quenchFrame.fogParams.x;
    if (dist < 1e-3 || density <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    const float3 rd = d / dist;
    const float3 c = float3(camPos.x, camPos.y - quenchFrame.fogParams.z, camPos.z);
    const float optical = fogLayer(density, quenchFrame.fogParams.y, c, rd, dist);
    const float fog = (1.0 - exp(-max(optical, 0.0))) * quenchFrame.fogColor.w;
    return float4(hazeColor(rd), fog);
}

float3 applyFog(float3 color, float3 worldPos) {
    const float4 fog = fogAt(worldPos, quenchFrame.cameraPos.xyz);
    return lerp(color, fog.rgb, fog.a);
}

#endif
