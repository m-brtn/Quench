// Ray-traced shadows and ambient occlusion of quench::render::Renderer (docs/render.md, "Ray-traced shadows").
// rt_shadow_fs and rt_shadow_local_fs use ray queries and are compiled by DXC (gpu::Caps::rayTracing); rt_denoise_fs
// by glslang.
//
// rt_shadow_fs traces from the depth of a prepass: per pixel, rays toward the sun's disc and, with ambient
// occlusion, rays over the hemisphere of the normal reconstructed from the depth. Opaque instances end a ray. For
// the others (RayInstanceData at the instance id) every hit is a candidate: cut-out materials let the ray through
// where their alpha is below the cutoff, translucent ones tint it as their transmittance map does
// (quenchTransmittanceOf), once per instance. Output: rgb the sun's transmittance, a the ambient occlusion.
// rt_shadow_local_fs also traces one ray per pixel toward each local light with ray-traced shadows (LocalLight::cone:
// y the channel + 1, z the light's radius) into a second target.
//
// Bindings (fragment): uniform 0 FrameUniforms, uniform 1 RayTracingUniforms, uniform 2 LightUniforms
// (rt_shadow_local_fs). rt_shadow_fs: sampler 0 the prepass depth, samplers 1..8 the alpha textures (texture 0 of
// the materials), storage buffers t9..t11 (RayInstanceData[], vertices as four float4 each, uint indices),
// acceleration structure t12. rt_denoise_fs: sampler 0 the traced mask, sampler 1 the depth.

#include "quench/Bindings.hlsli"
#include "quench/Common.hlsli"
#include "quench/Frame.hlsli"
#include "quench/Fullscreen.hlsli"
#include "quench/SceneUniforms.h"

#include "RayCommon.hlsli"
#include "RayTracingUniforms.h"

cbuffer RayTracingBlock : QUENCH_UNIFORM(1) {
    RayTracingUniforms rt;
};

#if defined(QUENCH_ENTRY_rt_shadow_fs) || defined(QUENCH_ENTRY_rt_shadow_local_fs)
Texture2D<float> depthTex : QUENCH_TEXTURE(0);
SamplerState depthSampler : QUENCH_SAMPLER(0);
Texture2D<float4> alpha0 : QUENCH_TEXTURE(1);
SamplerState alphaSampler0 : QUENCH_SAMPLER(1);
Texture2D<float4> alpha1 : QUENCH_TEXTURE(2);
SamplerState alphaSampler1 : QUENCH_SAMPLER(2);
Texture2D<float4> alpha2 : QUENCH_TEXTURE(3);
SamplerState alphaSampler2 : QUENCH_SAMPLER(3);
Texture2D<float4> alpha3 : QUENCH_TEXTURE(4);
SamplerState alphaSampler3 : QUENCH_SAMPLER(4);
Texture2D<float4> alpha4 : QUENCH_TEXTURE(5);
SamplerState alphaSampler4 : QUENCH_SAMPLER(5);
Texture2D<float4> alpha5 : QUENCH_TEXTURE(6);
SamplerState alphaSampler5 : QUENCH_SAMPLER(6);
Texture2D<float4> alpha6 : QUENCH_TEXTURE(7);
SamplerState alphaSampler6 : QUENCH_SAMPLER(7);
Texture2D<float4> alpha7 : QUENCH_TEXTURE(8);
SamplerState alphaSampler7 : QUENCH_SAMPLER(8);
StructuredBuffer<RayInstanceData> rtInstances : QUENCH_STORAGE(9);
StructuredBuffer<float4> rtVertices : QUENCH_STORAGE(10);
StructuredBuffer<uint> rtIndices : QUENCH_STORAGE(11);
RaytracingAccelerationStructure rtScene : QUENCH_STORAGE(12);

float4 alphaTexture(int slot, float2 uv) {
    switch (slot) {
    case 0: return alpha0.SampleLevel(alphaSampler0, uv, 0.0);
    case 1: return alpha1.SampleLevel(alphaSampler1, uv, 0.0);
    case 2: return alpha2.SampleLevel(alphaSampler2, uv, 0.0);
    case 3: return alpha3.SampleLevel(alphaSampler3, uv, 0.0);
    case 4: return alpha4.SampleLevel(alphaSampler4, uv, 0.0);
    case 5: return alpha5.SampleLevel(alphaSampler5, uv, 0.0);
    case 6: return alpha6.SampleLevel(alphaSampler6, uv, 0.0);
    case 7: return alpha7.SampleLevel(alphaSampler7, uv, 0.0);
    default: return float4(1.0, 1.0, 1.0, 1.0);
    }
}

// The standard surface's base color at a candidate hit: material color, texture 0 and the vertex color. Cut-out and
// translucent casters are in the vertex table with their bind-pose vertices (uv and color do not move with a pose).
float4 candidateColor(RayInstanceData d, uint primitive, float2 bary) {
    const uint first = asuint(d.geometry.x) + primitive * 3u;
    const uint base = asuint(d.geometry.y);
    const uint i0 = (base + rtIndices[first]) * 4u;
    const uint i1 = (base + rtIndices[first + 1u]) * 4u;
    const uint i2 = (base + rtIndices[first + 2u]) * 4u;
    const float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    const float2 uv = rtVertices[i0 + 1u].zw * w.x + rtVertices[i1 + 1u].zw * w.y + rtVertices[i2 + 1u].zw * w.z;
    const float4 color = rtVertices[i0 + 2u] * w.x + rtVertices[i1 + 2u] * w.y + rtVertices[i2 + 2u] * w.z;
    float4 c = d.color * color;
    const int slot = int(d.surface.z);
    if (slot >= 0) c *= alphaTexture(slot, uv);
    return c;
}

// The light that passes along a ray: 0 behind opaque geometry and cut-out texels, the product of the transmittance
// of the translucent instances it crosses (each counted once), 1 otherwise.
float3 traceVisibility(float3 origin, float3 direction, float tMax) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(rtScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                     QUENCH_RT_MASK_CASTER | QUENCH_RT_MASK_TRANSLUCENT, ray);
    float3 t = float3(1.0, 1.0, 1.0);
    uint seen0 = 0xffffffffu, seen1 = 0xffffffffu, seen2 = 0xffffffffu, seen3 = 0xffffffffu;
    while (q.Proceed()) {
        const uint id = q.CandidateInstanceID();
        const RayInstanceData d = rtInstances[id];
        const float4 c = candidateColor(d, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics());
        if (d.emissive.w > 0.0 && c.a < d.emissive.w) continue; // a cut-out hole
        if (int(d.geometry.w) != 2) {
            q.CommitNonOpaqueTriangleHit();
            q.Abort();
            break;
        }
        if (id == seen0 || id == seen1 || id == seen2 || id == seen3) continue;
        seen3 = seen2;
        seen2 = seen1;
        seen1 = seen0;
        seen0 = id;
        t *= quenchTransmittanceOf(c.rgb, c.a);
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? float3(0.0, 0.0, 0.0) : t;
}

// The distance to the nearest opaque or cut-out caster along a ray (translucent ones are not in the mask); tMax:
// none.
float traceDistance(float3 origin, float3 direction, float tMax) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(rtScene, RAY_FLAG_NONE, QUENCH_RT_MASK_CASTER, ray);
    while (q.Proceed()) {
        const RayInstanceData d = rtInstances[q.CandidateInstanceID()];
        const float4 c = candidateColor(d, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics());
        if (d.emissive.w > 0.0 && c.a < d.emissive.w) continue;
        q.CommitNonOpaqueTriangleHit();
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : tMax;
}

float4 traceSunAndOcclusion(float2 position, float3 P, float3 N, float3 origin) {
    // Per pixel and frame: TAA and the denoise pass average the samples.
    const float rotation = 6.2831853 * ign(position + float(uint(quenchFrame.options.y) & 63u) * 5.588238);

    float4 result = float4(1.0, 1.0, 1.0, 1.0);
    const uint sunRays = uint(rt.shadow.x);
    const float3 sun = quenchFrame.sunDir.xyz;
    if (sunRays > 0u && quenchFrame.sunDir.w > 0.0) {
        if (dot(N, sun) <= -0.05) {
            result.rgb = float3(0.0, 0.0, 0.0); // facing away: unlit anyway
        } else {
            const float3 T = rtPerpendicular(sun);
            const float3 B = cross(sun, T);
            float3 sum = float3(0.0, 0.0, 0.0);
            for (uint i = 0u; i < sunRays; ++i) {
                // Vogel disc over the sun's cone.
                const float r = sqrt((float(i) + 0.5) / float(sunRays));
                const float a = float(i) * 2.3999632 + rotation;
                const float3 dir = normalize(sun + (T * cos(a) + B * sin(a)) * (r * rt.shadow.y));
                sum += traceVisibility(origin, dir, rt.shadow.z);
            }
            result.rgb = sum / float(sunRays);
        }
    }

    const uint aoRays = uint(rt.occlusion.x);
    if (aoRays > 0u) {
        const float3 T = rtPerpendicular(N);
        const float3 B = cross(N, T);
        const float jitter = ign(position.yx + float(uint(quenchFrame.options.y) & 63u) * 3.17);
        float occluded = 0.0;
        for (uint i = 0u; i < aoRays; ++i) {
            // Cosine-weighted hemisphere: a Vogel disc projected up.
            const float u = (float(i) + jitter) / float(aoRays);
            const float r = sqrt(u);
            const float a = float(i) * 2.3999632 + rotation;
            const float3 dir = normalize(T * (r * cos(a)) + B * (r * sin(a)) + N * sqrt(max(1.0 - u, 0.0)));
            occluded += 1.0 - traceDistance(origin, dir, rt.occlusion.y) / rt.occlusion.y;
        }
        result.a = 1.0 - rt.occlusion.z * occluded / float(aoRays);
    }
    return result;
}
#endif

#if defined(QUENCH_ENTRY_rt_shadow_fs)
float4 rt_shadow_fs(FullscreenOut vin) : SV_Target0 {
    const int2 p = int2(vin.position.xy);
    const float d = depthTex.Load(int3(p, 0));
    if (d <= 0.0) return float4(1.0, 1.0, 1.0, 1.0); // the sky
    const float3 P = rtWorldAt(vin.position.xy, d);
    const float3 N = rtDepthNormal(depthTex, p, P);
    const float3 origin = P + N * (rt.shadow.w * max(quenchViewDepth(P), 1.0));
    return traceSunAndOcclusion(vin.position.xy, P, N, origin);
}
#endif

#if defined(QUENCH_ENTRY_rt_shadow_local_fs)
cbuffer RayLightBlock : QUENCH_UNIFORM(2) {
    LightUniforms rtLights;
};

struct ShadowOut {
    float4 mask : SV_Target0;
    float4 local : SV_Target1; // one channel per local light with ray-traced shadows
};

ShadowOut rt_shadow_local_fs(FullscreenOut vin) {
    ShadowOut o;
    o.mask = float4(1.0, 1.0, 1.0, 1.0);
    o.local = float4(1.0, 1.0, 1.0, 1.0);
    const int2 p = int2(vin.position.xy);
    const float d = depthTex.Load(int3(p, 0));
    if (d <= 0.0) return o;
    const float3 P = rtWorldAt(vin.position.xy, d);
    const float3 N = rtDepthNormal(depthTex, p, P);
    const float3 origin = P + N * (rt.shadow.w * max(quenchViewDepth(P), 1.0));
    o.mask = traceSunAndOcclusion(vin.position.xy, P, N, origin);
    uint seed = rtSeed(uint2(p), uint(quenchFrame.options.y), 0x3c6eu);
    const int lights = int(rtLights.count.x);
    for (int k = 0; k < lights; ++k) {
        const LocalLight light = rtLights.lights[k];
        if (light.cone.y <= 0.0) continue;
        const int c = int(light.cone.y) - 1;
        // A point on the light's sphere, uniformly over its area.
        const float z = rtRandom(seed) * 2.0 - 1.0;
        const float a = 6.2831853 * rtRandom(seed);
        const float r = sqrt(max(1.0 - z * z, 0.0));
        const float3 target = light.positionRange.xyz + float3(r * cos(a), z, r * sin(a)) * light.cone.z;
        const float3 toLight = target - origin;
        const float distance = length(toLight);
        float visible = 0.0;
        if (distance > 1e-4 && dot(N, light.positionRange.xyz - P) > 0.0)
            visible = luminance(traceVisibility(origin, toLight / distance, max(distance - light.cone.z, 0.0)));
        o.local = c == 0 ? float4(visible, o.local.yzw) : o.local;
        o.local = c == 1 ? float4(o.local.x, visible, o.local.zw) : o.local;
        o.local = c == 2 ? float4(o.local.xy, visible, o.local.w) : o.local;
        o.local = c == 3 ? float4(o.local.xyz, visible) : o.local;
    }
    return o;
}
#endif

#if defined(QUENCH_ENTRY_rt_denoise_fs)
Texture2D<float4> maskTex : QUENCH_TEXTURE(0);
SamplerState maskSampler : QUENCH_SAMPLER(0);
Texture2D<float> depthTex : QUENCH_TEXTURE(1);
SamplerState depthSampler : QUENCH_SAMPLER(1);

// Depth-aware box blurs of the traced mask, one radius for rgb and one for alpha: neighbours at a different depth
// (other surfaces) do not bleed in.
float4 rt_denoise_fs(FullscreenOut vin) : SV_Target0 {
    const int2 p = int2(vin.position.xy);
    const float d = depthTex.Load(int3(p, 0));
    const float4 center = maskTex.Load(int3(p, 0));
    const int shadowRadius = int(rt.denoise.x), occlusionRadius = int(rt.denoise.z);
    const int radius = max(shadowRadius, occlusionRadius);
    if (d <= 0.0 || radius <= 0) return center;
    // Reversed infinite projection: view depth = near / depth.
    const float z = quenchFrame.cameraFwd.w / d;
    const float tolerance = rt.denoise.y * z;
    const int2 last = int2(quenchFrame.screen.xy) - 1;
    float4 sum = center;
    float2 weight = float2(1.0, 1.0); // rgb, a
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x) {
            if (x == 0 && y == 0) continue;
            const int2 q = clamp(p + int2(x, y), int2(0, 0), last);
            const float dq = depthTex.Load(int3(q, 0));
            if (dq <= 0.0) continue;
            const float w = saturate(1.0 - abs(quenchFrame.cameraFwd.w / dq - z) / tolerance);
            const int reach = max(abs(x), abs(y));
            const float2 wr = float2(reach <= shadowRadius ? w : 0.0, reach <= occlusionRadius ? w : 0.0);
            const float4 m = maskTex.Load(int3(q, 0));
            sum += float4(m.rgb * wr.x, m.a * wr.y);
            weight += wr;
        }
    return float4(sum.rgb / weight.x, sum.a / weight.y);
}
#endif
