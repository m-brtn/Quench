// Ray-traced sun shadows and ambient occlusion of quench::render::Renderer (docs/render.md, "Ray-traced shadows").
// rt_shadow_fs uses ray queries and is compiled by DXC (gpu::Caps::rayTracing); rt_denoise_fs by glslang.
//
// rt_shadow_fs traces from the depth of a prepass: per pixel, rays toward the sun's disc and, with ambient
// occlusion, rays over the hemisphere of the normal reconstructed from the depth. Opaque instances end a ray. For
// the others (RayInstanceData at the instance id) every hit is a candidate: cut-out materials let the ray through
// where their alpha is below the cutoff, translucent ones tint it as their transmittance map does
// (quenchTransmittanceOf), once per instance. Output: rgb the sun's transmittance, a the ambient occlusion.
//
// Bindings (fragment): uniform 0 FrameUniforms, uniform 1 RayTracingUniforms. rt_shadow_fs: sampler 0 the prepass
// depth, samplers 1..8 the alpha textures (texture 0 of the materials), storage buffers t9..t11 (RayInstanceData[],
// vertices as four float4 each, uint indices), acceleration structure t12. rt_denoise_fs: sampler 0 the traced
// mask, sampler 1 the depth.

#include "quench/Bindings.hlsli"
#include "quench/Common.hlsli"
#include "quench/Frame.hlsli"
#include "quench/Fullscreen.hlsli"
#include "quench/SceneUniforms.h"

#include "RayTracingUniforms.h"

cbuffer RayTracingBlock : QUENCH_UNIFORM(1) {
    RayTracingUniforms rt;
};

// World position of a pixel center at a depth (reversed: near 1, far 0).
float3 worldAt(float2 pixel, float depth) {
    const float2 uv = pixel * quenchFrame.screen.zw;
    const float4 w = mul(quenchFrame.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return w.xyz / w.w;
}

#if defined(QUENCH_ENTRY_rt_shadow_fs)
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

static const uint kMaskOpaque = 1u;      // opaque and cut-out instances
static const uint kMaskTranslucent = 2u; // translucent instances (not in ambient occlusion)

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

// The standard surface's base color at a candidate hit: material color, texture 0 and the vertex color.
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
    const int slot = int(d.geometry.w);
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
    q.TraceRayInline(rtScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, kMaskOpaque | kMaskTranslucent, ray);
    float3 t = float3(1.0, 1.0, 1.0);
    uint seen0 = 0xffffffffu, seen1 = 0xffffffffu, seen2 = 0xffffffffu, seen3 = 0xffffffffu;
    while (q.Proceed()) {
        const uint id = q.CandidateInstanceID();
        const RayInstanceData d = rtInstances[id];
        const float4 c = candidateColor(d, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics());
        if (d.params.x > 0.0 && c.a < d.params.x) continue; // a cut-out hole
        if (int(d.geometry.z) != 2) {
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

// The distance to the nearest opaque or cut-out surface along a ray (translucent ones are not in the mask); tMax:
// none.
float traceDistance(float3 origin, float3 direction, float tMax) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(rtScene, RAY_FLAG_NONE, kMaskOpaque, ray);
    while (q.Proceed()) {
        const RayInstanceData d = rtInstances[q.CandidateInstanceID()];
        const float4 c = candidateColor(d, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics());
        if (d.params.x > 0.0 && c.a < d.params.x) continue;
        q.CommitNonOpaqueTriangleHit();
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : tMax;
}

// A unit vector perpendicular to n.
float3 perpendicular(float3 n) {
    return abs(n.y) < 0.99 ? normalize(cross(n, float3(0.0, 1.0, 0.0))) : normalize(cross(n, float3(1.0, 0.0, 0.0)));
}

float4 rt_shadow_fs(FullscreenOut vin) : SV_Target0 {
    const int2 p = int2(vin.position.xy);
    const float d = depthTex.Load(int3(p, 0));
    if (d <= 0.0) return float4(1.0, 1.0, 1.0, 1.0); // the sky

    // The normal from the depth: the smaller difference on each axis keeps edges from bending it.
    const float3 P = worldAt(vin.position.xy, d);
    const int2 last = int2(quenchFrame.screen.xy) - 1;
    const int2 pr = min(p + int2(1, 0), last), pl = max(p - int2(1, 0), int2(0, 0));
    const int2 pd = min(p + int2(0, 1), last), pu = max(p - int2(0, 1), int2(0, 0));
    const float3 R = worldAt(float2(pr) + 0.5, depthTex.Load(int3(pr, 0))) - P;
    const float3 L = P - worldAt(float2(pl) + 0.5, depthTex.Load(int3(pl, 0)));
    const float3 D = worldAt(float2(pd) + 0.5, depthTex.Load(int3(pd, 0))) - P;
    const float3 U = P - worldAt(float2(pu) + 0.5, depthTex.Load(int3(pu, 0)));
    const float3 dx = dot(R, R) < dot(L, L) ? R : L;
    const float3 dy = dot(D, D) < dot(U, U) ? D : U;
    float3 N = normalize(cross(dy, dx));
    const float3 V = normalize(quenchFrame.cameraPos.xyz - P);
    if (dot(N, V) < 0.0) N = -N;

    const float viewDistance = max(quenchViewDepth(P), 1.0);
    const float3 origin = P + N * (rt.shadow.w * viewDistance);
    // Per pixel and frame: TAA and the denoise pass average the samples.
    const float rotation = 6.2831853 * ign(vin.position.xy + float(uint(quenchFrame.options.y) & 63u) * 5.588238);

    float4 result = float4(1.0, 1.0, 1.0, 1.0);
    const uint sunRays = uint(rt.shadow.x);
    const float3 sun = quenchFrame.sunDir.xyz;
    if (sunRays > 0u && quenchFrame.sunDir.w > 0.0) {
        if (dot(N, sun) <= -0.05) {
            result.rgb = float3(0.0, 0.0, 0.0); // facing away: unlit anyway
        } else {
            const float3 T = perpendicular(sun);
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
        const float3 T = perpendicular(N);
        const float3 B = cross(N, T);
        const float jitter = ign(vin.position.yx + float(uint(quenchFrame.options.y) & 63u) * 3.17);
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
