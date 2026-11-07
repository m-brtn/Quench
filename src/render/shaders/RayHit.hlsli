#ifndef QUENCH_RAY_HIT_HLSLI
#define QUENCH_RAY_HIT_HLSLI

// What the rays of the hit passes see (raylight.hlsl, pathtrace.hlsl; ray queries, compiled by DXC): the scene's top
// level, every instance's triangles and material (RayInstanceData), the base color, metallic-roughness, normal and
// emissive textures as layers of two texture arrays (256 x 256, no mip levels), the lights and the probe grid. Hits
// are shaded with the standard surface's BRDF from the material's values (anisotropy and normal maps included): a
// surface shader's own shading is not run for rays. The shapes of area lights are in the top level for path rays only
// (QUENCH_RT_MASK_EMITTER): the other rays get the area lights' light from their shading, as the raster passes do.
//
// Bindings (fragment): uniform 0 FrameUniforms, uniform 1 RayHitUniforms, uniform 2 LightUniforms; samplers 0 the
// prepass depth, 1 the G-buffer normal and roughness, 2 the color array (sRGB: base color, emissive), 3 the linear
// array (metallic-roughness, normal maps), 4 and 5 the probe grid's irradiance and distance atlases, 6 the area lights' table
// (quench/AreaLights.hlsli), then the entry's own (QUENCH_RT_SAMPLERS in all); storage buffers after them:
// RayInstanceData[], the vertex table and the frame's skinned vertices (four float4 per vertex), the index table;
// then the acceleration structure.

#include "quench/AreaLights.hlsli"
#include "quench/Bindings.hlsli"
#include "quench/Brdf.hlsli"
#include "quench/Probes.hlsli"
#include "quench/SceneUniforms.h"
#include "quench/Sky.hlsli"

#include "RayCommon.hlsli"
#include "RayTracingUniforms.h"

cbuffer RayHitBlock : QUENCH_UNIFORM(1) {
    RayHitUniforms rtHit;
};
cbuffer RayLightBlock : QUENCH_UNIFORM(2) {
    LightUniforms rtLights;
};

Texture2D<float> rtDepth : QUENCH_TEXTURE(0);
SamplerState rtPointSampler : QUENCH_SAMPLER(0);
Texture2D<float4> rtGbuffer : QUENCH_TEXTURE(1);
SamplerState rtGbufferSampler : QUENCH_SAMPLER(1);
Texture2DArray<float4> rtHitColor : QUENCH_TEXTURE(2);
SamplerState rtHitColorSampler : QUENCH_SAMPLER(2);
Texture2DArray<float4> rtHitLinear : QUENCH_TEXTURE(3);
SamplerState rtHitLinearSampler : QUENCH_SAMPLER(3);
Texture2D<float4> rtProbeIrradiance : QUENCH_TEXTURE(4);
SamplerState rtProbeSampler : QUENCH_SAMPLER(4);
Texture2D<float4> rtProbeDistance : QUENCH_TEXTURE(5);
SamplerState rtProbeDistanceSampler : QUENCH_SAMPLER(5);
Texture2DArray<float4> rtAreaTable : QUENCH_TEXTURE(6);
SamplerState rtAreaSampler : QUENCH_SAMPLER(6);

#if QUENCH_RT_SAMPLERS == 7
StructuredBuffer<RayInstanceData> rtInstances : QUENCH_STORAGE(7);
StructuredBuffer<float4> rtVertices : QUENCH_STORAGE(8);
StructuredBuffer<float4> rtSkinned : QUENCH_STORAGE(9);
StructuredBuffer<uint> rtIndices : QUENCH_STORAGE(10);
RaytracingAccelerationStructure rtScene : QUENCH_STORAGE(11);
#elif QUENCH_RT_SAMPLERS == 8
StructuredBuffer<RayInstanceData> rtInstances : QUENCH_STORAGE(8);
StructuredBuffer<float4> rtVertices : QUENCH_STORAGE(9);
StructuredBuffer<float4> rtSkinned : QUENCH_STORAGE(10);
StructuredBuffer<uint> rtIndices : QUENCH_STORAGE(11);
RaytracingAccelerationStructure rtScene : QUENCH_STORAGE(12);
#else
#error "QUENCH_RT_SAMPLERS: 7 or 8"
#endif

// A vertex's float4 n (0: position and normal.x, 1: normal.yz and uv, 2: color, 3: tangent) by its index in the
// mesh: the frame's posed vertices of a skinned instance, else the vertex table's.
float4 rtVertex(RayInstanceData d, uint vertex, uint n) {
    const uint skinned = asuint(d.geometry.z);
    if (skinned != 0u) return rtSkinned[(skinned - 1u + vertex) * 4u + n];
    return rtVertices[(asuint(d.geometry.y) + vertex) * 4u + n];
}

void rtTriangle(RayInstanceData d, uint primitive, out uint v0, out uint v1, out uint v2) {
    const uint first = asuint(d.geometry.x) + primitive * 3u;
    v0 = rtIndices[first];
    v1 = rtIndices[first + 1u];
    v2 = rtIndices[first + 2u];
}

// The material's base color at a candidate hit: for the alpha test of cut-outs and the opacity of translucent ones.
float4 rtCandidateColor(RayInstanceData d, uint primitive, float2 bary) {
    uint v0, v1, v2;
    rtTriangle(d, primitive, v0, v1, v2);
    const float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    const float2 uv = rtVertex(d, v0, 1u).zw * w.x + rtVertex(d, v1, 1u).zw * w.y + rtVertex(d, v2, 1u).zw * w.z;
    float4 c = d.color * (rtVertex(d, v0, 2u) * w.x + rtVertex(d, v1, 2u) * w.y + rtVertex(d, v2, 2u) * w.z);
    if (d.layers.x >= 0.0) c *= rtHitColor.SampleLevel(rtHitColorSampler, float3(uv, d.layers.x), 0.0);
    return c;
}

struct RtHit {
    bool hit;
    float t;
    uint instance;
    uint primitive;
    float2 bary;
};

// The nearest opaque surface along a ray: cut-out texels let it through, and translucent instances in the mask stop
// it with a probability of their opacity (`seed` draws it).
RtHit rtTraceClosest(float3 origin, float3 direction, float tMax, uint mask, inout uint seed) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(rtScene, RAY_FLAG_NONE, mask, ray);
    while (q.Proceed()) {
        const RayInstanceData d = rtInstances[q.CandidateInstanceID()];
        const float4 c = rtCandidateColor(d, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics());
        const uint mode = uint(d.geometry.w);
        if (mode == 1u && c.a < d.emissive.w) continue;
        if (mode == 2u && rtRandom(seed) >= c.a) continue;
        q.CommitNonOpaqueTriangleHit();
    }
    RtHit h;
    h.hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    h.t = h.hit ? q.CommittedRayT() : tMax;
    h.instance = h.hit ? q.CommittedInstanceID() : 0u;
    h.primitive = h.hit ? q.CommittedPrimitiveIndex() : 0u;
    h.bary = h.hit ? q.CommittedTriangleBarycentrics() : float2(0.0, 0.0);
    return h;
}

// The light that passes along a shadow ray: 0 behind opaque geometry and cut-out texels, the product of the
// transmittance of the translucent instances it crosses (each counted once), 1 otherwise.
float3 rtVisibility(float3 origin, float3 direction, float tMax) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(rtScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, QUENCH_RT_MASK_CASTER | QUENCH_RT_MASK_TRANSLUCENT,
                     ray);
    float3 t = float3(1.0, 1.0, 1.0);
    uint seen0 = 0xffffffffu, seen1 = 0xffffffffu;
    while (q.Proceed()) {
        const uint id = q.CandidateInstanceID();
        const RayInstanceData d = rtInstances[id];
        const float4 c = rtCandidateColor(d, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics());
        const uint mode = uint(d.geometry.w);
        if (mode == 1u && c.a < d.emissive.w) continue;
        if (mode != 2u) {
            q.CommitNonOpaqueTriangleHit();
            q.Abort();
            break;
        }
        if (id == seen0 || id == seen1) continue;
        seen1 = seen0;
        seen0 = id;
        t *= quenchTransmittanceOf(c.rgb, c.a);
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? float3(0.0, 0.0, 0.0) : t;
}

// A hit point with the material's values.
struct RtSurface {
    float3 position;
    float3 normal; // shading normal (interpolated), toward the ray's origin
    bool back;     // the ray hit the back of a single-sided triangle (the inside of a closed mesh)
    float3 albedo;
    float opacity;
    float roughness;
    float metallic;
    float3 emissive;
    float anisotropy; // 0: isotropic
    float3 anisotropyDirection;
    int light; // the shape of an area light: 1 + its id (LocalLight::shape.w), else 0
};

RtSurface rtSurfaceAt(RtHit h, float3 origin, float3 direction) {
    const RayInstanceData d = rtInstances[h.instance];
    uint v0, v1, v2;
    rtTriangle(d, h.primitive, v0, v1, v2);
    const float3 w = float3(1.0 - h.bary.x - h.bary.y, h.bary.x, h.bary.y);
    const float4 a0 = rtVertex(d, v0, 0u), a1 = rtVertex(d, v1, 0u), a2 = rtVertex(d, v2, 0u);
    const float4 b0 = rtVertex(d, v0, 1u), b1 = rtVertex(d, v1, 1u), b2 = rtVertex(d, v2, 1u);
    const float3x3 normalMatrix = float3x3(d.normal0.xyz, d.normal1.xyz, d.normal2.xyz);
    const float3 geometric = mul(normalMatrix, cross(a1.xyz - a0.xyz, a2.xyz - a0.xyz));
    float3 n = mul(normalMatrix, float3(a0.w, b0.x, b0.y) * w.x + float3(a1.w, b1.x, b1.y) * w.y +
                                     float3(a2.w, b2.x, b2.y) * w.z);
    n = dot(n, n) > 1e-12 ? normalize(n) : normalize(geometric);
    const bool facing = dot(geometric, direction) < 0.0; // counter-clockwise front faces
    const float2 uv = b0.zw * w.x + b1.zw * w.y + b2.zw * w.z;
    // The anisotropy direction: the vertex tangent turned by the material's rotation (the normal matrix keeps the
    // tangent's direction under rotations and uniform scales).
    const float anisotropy = d.normal0.w;
    float3 anisotropyDirection = float3(1.0, 0.0, 0.0);
    if (anisotropy > 0.0) {
        const float4 t0 = rtVertex(d, v0, 3u), t1 = rtVertex(d, v1, 3u), t2 = rtVertex(d, v2, 3u);
        const float3 T = mul(normalMatrix, t0.xyz * w.x + t1.xyz * w.y + t2.xyz * w.z);
        const float3 B = cross(n, T) * (t0.w < 0.0 ? -1.0 : 1.0);
        anisotropyDirection = T * d.normal1.w + B * d.normal2.w;
    }
    float3 shading = dot(n, direction) > 0.0 ? -n : n;
#if defined(QUENCH_RT_NORMAL_MAPS)
    // The normal map in the frame of the raster's quenchNormalFromMap: the interpolated normal turned to the side the
    // ray hit, the tangent made perpendicular to it, the bitangent's sign turned by a mirroring transform. A mapped
    // normal that faces away from the ray's origin is not used.
    [branch] if (d.maps.x >= 0.0) {
        const float4 t0 = rtVertex(d, v0, 3u), t1 = rtVertex(d, v1, 3u), t2 = rtVertex(d, v2, 3u);
        const float3 T = mul(normalMatrix, t0.xyz * w.x + t1.xyz * w.y + t2.xyz * w.z);
        const float3 side = facing ? n : -n;
        const float3 t = T - side * dot(side, T);
        if (dot(t, t) > 1e-12) {
            const float handedness = t0.w < 0.0 ? -1.0 : 1.0;
            const float mirrored = dot(cross(d.normal0.xyz, d.normal1.xyz), d.normal2.xyz) < 0.0 ? -1.0 : 1.0;
            const float3 c = rtHitLinear.SampleLevel(rtHitLinearSampler, float3(uv, d.maps.x), 0.0).rgb * 2.0 - 1.0;
            const float3 Tn = normalize(t);
            const float3 mapped = Tn * c.x + cross(side, Tn) * (handedness * mirrored) * c.y + side * c.z;
            if (dot(mapped, mapped) > 1e-8 && dot(mapped, direction) < 0.0) shading = normalize(mapped);
        }
    }
#endif
    n = shading;
    const float4 vc = rtVertex(d, v0, 2u) * w.x + rtVertex(d, v1, 2u) * w.y + rtVertex(d, v2, 2u) * w.z;
    float4 base = d.color * vc;
    if (d.layers.x >= 0.0) base *= rtHitColor.SampleLevel(rtHitColorSampler, float3(uv, d.layers.x), 0.0);
    float4 mr = float4(1.0, 1.0, 1.0, 1.0);
    if (d.layers.y >= 0.0) mr = rtHitLinear.SampleLevel(rtHitLinearSampler, float3(uv, d.layers.y), 0.0);
    float3 emissive = d.emissive.rgb;
    if (d.layers.z >= 0.0) emissive *= rtHitColor.SampleLevel(rtHitColorSampler, float3(uv, d.layers.z), 0.0).rgb;
    RtSurface s;
    s.position = origin + direction * h.t;
    s.normal = n;
    s.back = !facing && d.surface.w < 0.5;
    s.albedo = base.rgb;
    s.opacity = base.a;
    s.roughness = saturate(d.surface.x * mr.g);
    s.metallic = saturate(d.surface.y * mr.b);
    s.emissive = emissive;
    s.anisotropy = anisotropy;
    s.anisotropyDirection = anisotropyDirection;
    s.light = int(d.layers.w);
    return s;
}

// Lambert diffuse and GGX specular from one direction L with irradiance E (quench/Surface.hlsli's quenchDirectLight).
float3 rtDirectLight(RtSurface s, float3 V, float3 L, float3 E) {
    const float3 N = s.normal;
    const float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0) return float3(0.0, 0.0, 0.0);
    const float3 H = normalize(L + V);
    const float NdotV = max(dot(N, V), 1e-3);
    const float NdotH = saturate(dot(N, H));
    const float3 F0 = lerp(((float3)(0.04)), s.albedo, s.metallic);
    const float3 F = fresnelSchlick(F0, saturate(dot(V, H)));
    const float3 kd = (1.0 - F) * (1.0 - s.metallic);
    if (s.anisotropy > 0.0) {
        float3 T, B;
        ggxAnisotropicFrame(N, s.anisotropyDirection, T, B);
        const float spec = ggxSpecAnisotropic(N, T, B, V, L, ggxAnisotropicAlpha(s.roughness, s.anisotropy));
        return (kd * s.albedo / QUENCH_PI + F * spec) * E * NdotL;
    }
    return (kd * s.albedo / QUENCH_PI + F * ggxSpec(NdotH, NdotV, NdotL, s.roughness)) * E * NdotL;
}

// The normal and roughness of the lobe that reflects the sky and the area lights (quenchShade's).
void rtLobe(RtSurface s, float3 V, out float3 normal, out float roughness) {
    normal = s.normal;
    roughness = s.roughness;
    if (s.anisotropy > 0.0) {
        float3 T, B;
        ggxAnisotropicFrame(s.normal, s.anisotropyDirection, T, B);
        normal = ggxAnisotropicNormal(s.normal, B, V, s.roughness, s.anisotropy);
        const float2 alpha = ggxAnisotropicAlpha(s.roughness, s.anisotropy);
        roughness = sqrt(sqrt(alpha.x * alpha.y));
    }
}

// The diffuse light of the hit's surroundings (irradiance / pi): the probe grid, or the sky's ambient.
float3 rtIndirectAt(RtSurface s, float3 V) {
    const float3 ambient = lerp(quenchFrame.groundAmbient.rgb, quenchFrame.skyAmbient.rgb, s.normal.y * 0.5 + 0.5);
    if (rtHit.params.w < 0.5) return ambient;
    return quenchProbeIrradiance(rtProbeIrradiance, rtProbeDistance, rtProbeSampler, s.position, s.normal, V, ambient);
}

// The light a hit sends back along the ray: emissive, the sun (with a shadow ray), the local and area lights
// (without shadows) and the indirect light; its own reflections are the sky's.
float3 rtShade(RtSurface s, float3 V) {
    float3 color = s.emissive;
    const float offset = 0.002 * max(length(s.position - quenchFrame.cameraPos.xyz), 1.0);
    if (quenchFrame.sunDir.w > 0.0 && dot(s.normal, quenchFrame.sunDir.xyz) > 0.0) {
        const float3 visibility = rtVisibility(s.position + s.normal * offset, quenchFrame.sunDir.xyz, rtHit.params.z);
        color += rtDirectLight(s, V, quenchFrame.sunDir.xyz, quenchFrame.sunColor.rgb * visibility);
    }
    float3 lobeNormal;
    float lobeRoughness;
    rtLobe(s, V, lobeNormal, lobeRoughness);
    QuenchLtc ltc = quenchNoLtc();
    if (rtLights.count.y > 0.0)
        ltc = quenchLtcAt(rtAreaTable, s.normal, lobeNormal, V, lobeRoughness, s.albedo, s.metallic);
    const int lights = int(rtLights.count.x);
    for (int k = 0; k < lights; ++k) {
        const LocalLight light = rtLights.lights[k];
        const int type = int(light.color.w);
        if (type >= QUENCH_LIGHT_RECT) {
            const float a = quenchAreaWindow(light, s.position);
            if (a > 0.0) color += quenchAreaLight(light, s.position, ltc) * a;
            continue;
        }
        float3 L = -light.direction.xyz;
        float attenuation = 1.0;
        if (type != 0) {
            const float3 d = light.positionRange.xyz - s.position;
            const float d2 = max(dot(d, d), 1e-4);
            L = d * rsqrt(d2);
            const float r = d2 / max(light.positionRange.w * light.positionRange.w, 1e-6);
            const float window = saturate(1.0 - r * r);
            attenuation = window * window / d2;
            if (type == 2) {
                const float t = saturate((dot(-L, light.direction.xyz) - light.direction.w) * light.cone.x);
                attenuation *= t * t;
            }
        }
        if (attenuation > 0.0) color += rtDirectLight(s, V, L, light.color.rgb * attenuation);
    }
    const float3 indirect = rtIndirectAt(s, V);
    color += indirect * s.albedo * (1.0 - s.metallic);
    const float3 F0 = lerp(((float3)(0.04)), s.albedo, s.metallic);
    const float NdotV = max(dot(s.normal, V), 1e-3);
    const float3 R = reflect(-V, lobeNormal);
    color += lerp(skyRadiance(R), indirect, saturate(lobeRoughness * 1.2)) * envBRDFApprox(F0, lobeRoughness, NdotV);
    return color;
}

// Radiance along a ray: the hit's, or the sky's without the sun disc.
float3 rtRadiance(float3 origin, float3 direction, float tMax, inout uint seed, out RtHit hit) {
    hit = rtTraceClosest(origin, direction, tMax, QUENCH_RT_MASK_VISIBLE, seed);
    if (!hit.hit) return skyRadiance(direction);
    const RtSurface s = rtSurfaceAt(hit, origin, direction);
    return rtShade(s, -direction);
}

#endif
