// The reference path tracer of quench::render::Renderer (RenderSettings::pathTracer; docs/render.md, "Path tracer"):
// one path per pixel and frame through a random point of the pixel, accumulated into a running mean until the
// camera, the scene or the settings change. At each hit: the emissive light, the sun (a random point of its disc,
// with a shadow ray) and one local light chosen at random (with a shadow ray toward a point of its sphere), then the
// next direction from the standard surface's BRDF (GGX visible normals or a cosine lobe, chosen by the Fresnel
// weight), with Russian roulette after the second bounce. Translucent surfaces are crossed with the probability of
// their transparency. The sky is seen without the sun disc after the first bounce (the sun comes by its shadow
// rays), so mirrors show no sun disc. Uses ray queries: compiled by DXC.
//
// Bindings: those of RayHit.hlsli with QUENCH_RT_SAMPLERS 7 (raylight.hlsl includes it so for pt_fs); sampler 6 the
// accumulation so far.

#ifndef QUENCH_PATH_TRACE_HLSLI
#define QUENCH_PATH_TRACE_HLSLI

#include "quench/Fullscreen.hlsli"

#include "RayHit.hlsli"

Texture2D<float4> accumulationTex : QUENCH_TEXTURE(6);
SamplerState accumulationSampler : QUENCH_SAMPLER(6);

// Smith's masking of GGX for one direction (alpha = roughness^2).
float ptSmithG1(float NdotX, float alpha) {
    const float a2 = alpha * alpha;
    return 2.0 * NdotX / (NdotX + sqrt(a2 + (1.0 - a2) * NdotX * NdotX));
}

// Light from a local light toward P (unshadowed): the direction and irradiance on a surface facing it.
float3 ptLocalLight(LocalLight light, float3 P, out float3 L, out float distance) {
    const int type = int(light.color.w);
    L = -light.direction.xyz;
    distance = 1e4;
    if (type == 0) return light.color.rgb;
    const float3 d = light.positionRange.xyz - P;
    const float d2 = max(dot(d, d), 1e-4);
    distance = sqrt(d2);
    L = d / distance;
    const float r = d2 / max(light.positionRange.w * light.positionRange.w, 1e-6);
    const float window = saturate(1.0 - r * r);
    float attenuation = window * window / d2;
    if (type == 2) {
        const float t = saturate((dot(-L, light.direction.xyz) - light.direction.w) * light.cone.x);
        attenuation *= t * t;
    }
    return light.color.rgb * attenuation;
}

float4 pt_fs(FullscreenOut vin) : SV_Target0 {
    const int2 p = int2(vin.position.xy);
    uint seed = rtSeed(uint2(p), uint(rtHit.path.y), 0x7f4au);
    const float2 pixel = vin.position.xy - 0.5 + float2(rtRandom(seed), rtRandom(seed));
    const float2 ndc = float2(pixel.x * quenchFrame.screen.z * 2.0 - 1.0, 1.0 - pixel.y * quenchFrame.screen.w * 2.0);
    const float4 far = mul(quenchFrame.invViewProj, float4(ndc, 1e-6, 1.0));
    float3 origin = quenchFrame.cameraPos.xyz;
    float3 dir = normalize(far.xyz / far.w - origin);
    float3 throughput = float3(1.0, 1.0, 1.0);
    float3 radiance = float3(0.0, 0.0, 0.0);
    const int bounces = int(rtHit.path.x);
    const int lights = int(rtLights.count.x);
    const float sunCos = quenchFrame.sunDisc.w;
    for (int bounce = 0; bounce <= bounces; ++bounce) {
        const RtHit hit =
            rtTraceClosest(origin, dir, 1e5, QUENCH_RT_MASK_VISIBLE | QUENCH_RT_MASK_SEE_THROUGH, seed);
        if (!hit.hit) {
            const float3 sky = skyRadiance(dir);
            radiance += throughput * (bounce == 0 ? quenchSunDisc(sky, dir) : sky);
            break;
        }
        const RtSurface s = rtSurfaceAt(hit, origin, dir);
        radiance += throughput * s.emissive;
        if (bounce == bounces) break;
        const float3 V = -dir;
        const float3 N = s.normal;
        const float3 P = s.position + N * (0.0005 * max(hit.t, 1.0));

        // The sun: a random point of its disc.
        if (quenchFrame.sunDir.w > 0.0) {
            const float3 sun = quenchFrame.sunDir.xyz;
            const float3 T = rtPerpendicular(sun);
            const float3 B = cross(sun, T);
            const float r = sqrt(rtRandom(seed)) * sqrt(max(1.0 - sunCos * sunCos, 0.0)) / max(sunCos, 1e-3);
            const float a = 6.2831853 * rtRandom(seed);
            const float3 L = normalize(sun + (T * cos(a) + B * sin(a)) * r);
            if (dot(N, L) > 0.0)
                radiance += throughput * rtDirectLight(s, V, L, quenchFrame.sunColor.rgb * rtVisibility(P, L, 1e4));
        }
        // One local light, chosen uniformly (its contribution times the count).
        if (lights > 0) {
            const int k = min(int(rtRandom(seed) * float(lights)), lights - 1);
            const LocalLight light = rtLights.lights[k];
            float3 L;
            float distance;
            const float3 E = ptLocalLight(light, P, L, distance);
            if (dot(N, L) > 0.0 && max(max(E.r, E.g), E.b) > 0.0) {
                float3 toward = L;
                float reach = distance;
                if (int(light.color.w) != 0 && light.cone.z > 0.0) {
                    const float z = rtRandom(seed) * 2.0 - 1.0;
                    const float b = 6.2831853 * rtRandom(seed);
                    const float rr = sqrt(max(1.0 - z * z, 0.0));
                    const float3 target = light.positionRange.xyz + float3(rr * cos(b), z, rr * sin(b)) * light.cone.z;
                    reach = length(target - P);
                    toward = (target - P) / max(reach, 1e-4);
                }
                const float3 visible = rtVisibility(P, toward, max(reach - light.cone.z, 0.0));
                radiance += throughput * float(lights) * rtDirectLight(s, V, L, E * visible);
            }
        }

        // The next direction: GGX visible normals or the cosine lobe.
        const float NdotV = max(dot(N, V), 1e-4);
        const float3 F0 = lerp(((float3)(0.04)), s.albedo, s.metallic);
        const float3 Fv = fresnelSchlick(F0, NdotV);
        const float specular = clamp(lerp(luminance(Fv), 1.0, s.metallic), 0.05, 0.95);
        const float alpha = max(s.roughness * s.roughness, 1e-3);
        float3 next;
        if (rtRandom(seed) < specular) {
            const float3 H = rtGgxVisibleNormal(N, V, alpha, float2(rtRandom(seed), rtRandom(seed)));
            next = reflect(-V, H);
            const float NdotL = dot(N, next);
            if (NdotL <= 0.0) break;
            throughput *= fresnelSchlick(F0, saturate(dot(V, H))) * ptSmithG1(NdotL, alpha) / specular;
        } else {
            next = rtCosineDirection(N, float2(rtRandom(seed), rtRandom(seed)));
            throughput *= (1.0 - Fv) * (1.0 - s.metallic) * s.albedo / (1.0 - specular);
        }
        if (bounce >= 2) {
            const float survive = clamp(max(max(throughput.r, throughput.g), throughput.b), 0.05, 0.95);
            if (rtRandom(seed) > survive) break;
            throughput /= survive;
        }
        origin = P;
        dir = next;
    }
    radiance = min(radiance, (float3)1e4);
    const float n = rtHit.path.y;
    if (rtHit.path.z > 0.5) return float4(radiance, 1.0);
    const float3 previous = accumulationTex.Load(int3(p, 0)).rgb;
    return float4(lerp(previous, radiance, 1.0 / (n + 1.0)), 1.0);
}
#endif
