// The reference path tracer of quench::render::Renderer (RenderSettings::pathTracer; docs/render.md, "Path tracer"):
// one path per pixel and frame through a random point of the pixel, accumulated into a running mean until the
// camera, the scene or the settings change. At each hit: the emissive light, the sun (a random point of its disc,
// with a shadow ray) and one local light chosen at random (with a shadow ray toward a point of its sphere, or of an
// area light's shape), then the next direction from the standard surface's BRDF (GGX visible normals, anisotropic
// where the surface is, or a cosine lobe, chosen by the Fresnel weight), with Russian roulette after the second
// bounce. Area lights are sampled both ways: by a point of their shape and by the BRDF's rays that hit the shape,
// weighted by multiple importance sampling (the power heuristic). Translucent surfaces are crossed with the
// probability of their transparency. The sky is seen without the sun disc after the first bounce (the sun comes by
// its shadow rays), so mirrors show no sun disc. Uses ray queries: compiled by DXC.
//
// Bindings: those of RayHit.hlsli with QUENCH_RT_SAMPLERS 8 (raylight.hlsl includes it so for pt_fs); sampler 7 the
// accumulation so far.

#ifndef QUENCH_PATH_TRACE_HLSLI
#define QUENCH_PATH_TRACE_HLSLI

#include "quench/Fullscreen.hlsli"

#include "RayHit.hlsli"

Texture2D<float4> accumulationTex : QUENCH_TEXTURE(7);
SamplerState accumulationSampler : QUENCH_SAMPLER(7);

// Smith's masking of GGX for one direction (alpha = roughness^2).
float ptSmithG1(float NdotX, float alpha) {
    const float a2 = alpha * alpha;
    return 2.0 * NdotX / (NdotX + sqrt(a2 + (1.0 - a2) * NdotX * NdotX));
}

// The same for an anisotropic lobe (alpha.x along T, alpha.y along B).
float ptSmithG1Anisotropic(float3 N, float3 T, float3 B, float3 X, float2 alpha) {
    const float NdotX = max(dot(N, X), 1e-4);
    return 2.0 * NdotX / (NdotX + length(float3(alpha.x * dot(T, X), alpha.y * dot(B, X), NdotX)));
}

// The density (per solid angle) of the BRDF sampling below choosing L: the visible normals of the GGX lobe with
// probability `specular`, else the cosine lobe.
float ptBrdfPdf(RtSurface s, float3 V, float3 L, float specular) {
    const float3 N = s.normal;
    const float NdotL = dot(N, L);
    if (NdotL <= 0.0) return 0.0;
    const float NdotV = max(dot(N, V), 1e-4);
    const float3 H = normalize(V + L);
    float D, G1;
    if (s.anisotropy > 0.0) {
        float3 T, B;
        ggxAnisotropicFrame(N, s.anisotropyDirection, T, B);
        const float2 a = ggxAnisotropicAlpha(s.roughness, s.anisotropy);
        const float a2 = a.x * a.y;
        const float3 d = float3(a.y * dot(T, H), a.x * dot(B, H), a2 * dot(N, H));
        const float w2 = a2 / max(dot(d, d), 1e-12);
        D = a2 * w2 * w2 / QUENCH_PI;
        G1 = ptSmithG1Anisotropic(N, T, B, V, a);
    } else {
        const float alpha = max(s.roughness * s.roughness, 1e-3);
        const float a2 = alpha * alpha;
        const float NdotH = saturate(dot(N, H));
        const float k = NdotH * NdotH * (a2 - 1.0) + 1.0;
        D = a2 / (QUENCH_PI * k * k);
        G1 = ptSmithG1(NdotV, alpha);
    }
    // Visible normals: G1(V) D(H) max(V.H, 0) / N.V per solid angle of H, divided by 4 V.H for L.
    return specular * G1 * D / (4.0 * NdotV) + (1.0 - specular) * NdotL / QUENCH_PI;
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

float ptPowerHeuristic(float a, float b) { return a * a / max(a * a + b * b, 1e-30); }

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
    float brdfPdf = 0.0; // the density the last bounce chose `dir` with (0: the camera ray)
    const int bounces = int(rtHit.path.x);
    const int lights = int(rtLights.count.x);
    const float sunCos = quenchFrame.sunDisc.w;
    for (int bounce = 0; bounce <= bounces; ++bounce) {
        const RtHit hit = rtTraceClosest(origin, dir, 1e5,
                                         QUENCH_RT_MASK_VISIBLE | QUENCH_RT_MASK_SEE_THROUGH | QUENCH_RT_MASK_EMITTER,
                                         seed);
        if (!hit.hit) {
            const float3 sky = quenchCloudySky(dir);
            radiance += throughput * (bounce == 0 ? quenchSunDisc(sky, dir) : sky);
            break;
        }
        const RtSurface s = rtSurfaceAt(hit, origin, dir);
        // The shape of an area light: its radiance as the camera sees it, or weighted against the light's sampling
        // (and with its range window, as its light) after a bounce. Shapes reflect nothing.
        if (s.light > 0) {
            if (s.back) break;
            if (bounce == 0) {
                radiance += throughput * s.emissive;
                break;
            }
            for (int k = 0; k < lights; ++k) {
                const LocalLight light = rtLights.lights[k];
                if (int(light.shape.w) != s.light) continue;
                const float cosLight = max(dot(s.normal, -dir), 1e-4);
                const float lightPdf = hit.t * hit.t / (cosLight * rtAreaOf(light)) / float(lights);
                radiance += throughput * s.emissive * quenchAreaWindow(light, origin) *
                            ptPowerHeuristic(brdfPdf, lightPdf);
                break;
            }
            break;
        }
        radiance += throughput * s.emissive;
        if (bounce == bounces) break;
        const float3 V = -dir;
        const float3 N = s.normal;
        const float3 P = s.position + N * (0.0005 * max(hit.t, 1.0));
        const float NdotV = max(dot(N, V), 1e-4);
        const float3 F0 = lerp(((float3)(0.04)), s.albedo, s.metallic);
        const float3 Fv = fresnelSchlick(F0, NdotV);
        const float specular = clamp(lerp(luminance(Fv), 1.0, s.metallic), 0.05, 0.95);

        // The sun: a random point of its disc.
        if (quenchFrame.sunDir.w > 0.0) {
            const float3 sun = quenchFrame.sunDir.xyz;
            const float3 T = rtPerpendicular(sun);
            const float3 B = cross(sun, T);
            const float r = sqrt(rtRandom(seed)) * sqrt(max(1.0 - sunCos * sunCos, 0.0)) / max(sunCos, 1e-3);
            const float a = 6.2831853 * rtRandom(seed);
            const float3 L = normalize(sun + (T * cos(a) + B * sin(a)) * r);
            if (dot(N, L) > 0.0) {
                const float3 sunLight = quenchFrame.sunColor.rgb * quenchOvercastSun();
                radiance += throughput * rtDirectLight(s, V, L, sunLight * rtVisibility(P, L, 1e4));
            }
        }
        // One local light, chosen uniformly (its contribution times the count).
        if (lights > 0) {
            const int k = min(int(rtRandom(seed) * float(lights)), lights - 1);
            const LocalLight light = rtLights.lights[k];
            if (int(light.color.w) >= QUENCH_LIGHT_RECT) {
                float3 normal;
                const float3 target = rtAreaPoint(light, P, seed, normal);
                const float3 d = target - P;
                const float d2 = max(dot(d, d), 1e-8);
                const float distance = sqrt(d2);
                const float3 L = d / distance;
                const float cosLight = dot(normal, -L);
                const float window = quenchAreaWindow(light, P);
                if (cosLight > 0.0 && dot(N, L) > 0.0 && window > 0.0) {
                    const float lightPdf = d2 / (cosLight * rtAreaOf(light)) / float(lights);
                    const float weight = ptPowerHeuristic(lightPdf, ptBrdfPdf(s, V, L, specular)) / lightPdf;
                    const float3 visible = rtVisibility(P, L, distance * 0.999);
                    radiance += throughput * rtDirectLight(s, V, L, light.color.rgb * window * visible) * weight;
                }
            } else {
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
                        const float3 target =
                            light.positionRange.xyz + float3(rr * cos(b), z, rr * sin(b)) * light.cone.z;
                        reach = length(target - P);
                        toward = (target - P) / max(reach, 1e-4);
                    }
                    const float3 visible = rtVisibility(P, toward, max(reach - light.cone.z, 0.0));
                    radiance += throughput * float(lights) * rtDirectLight(s, V, L, E * visible);
                }
            }
        }

        // The next direction: GGX visible normals or the cosine lobe.
        float3 next;
        if (rtRandom(seed) < specular) {
            const float2 u = float2(rtRandom(seed), rtRandom(seed));
            if (s.anisotropy > 0.0) {
                float3 T, B;
                ggxAnisotropicFrame(N, s.anisotropyDirection, T, B);
                const float2 a = ggxAnisotropicAlpha(s.roughness, s.anisotropy);
                const float3 H = rtGgxVisibleNormalAnisotropic(N, T, B, V, a, u);
                next = reflect(-V, H);
                if (dot(N, next) <= 0.0) break;
                throughput *= fresnelSchlick(F0, saturate(dot(V, H))) * ptSmithG1Anisotropic(N, T, B, next, a) /
                              specular;
            } else {
                const float alpha = max(s.roughness * s.roughness, 1e-3);
                const float3 H = rtGgxVisibleNormal(N, V, alpha, u);
                next = reflect(-V, H);
                const float NdotL = dot(N, next);
                if (NdotL <= 0.0) break;
                throughput *= fresnelSchlick(F0, saturate(dot(V, H))) * ptSmithG1(NdotL, alpha) / specular;
            }
        } else {
            next = rtCosineDirection(N, float2(rtRandom(seed), rtRandom(seed)));
            throughput *= (1.0 - Fv) * (1.0 - s.metallic) * s.albedo / (1.0 - specular);
        }
        brdfPdf = ptBrdfPdf(s, V, next, specular);
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
