// Ray-traced reflections, indirect light, the probe grid and the path tracer of quench::render::Renderer
// (docs/render.md, "Ray-traced lighting"). The tracing entries (rt_reflect_fs, rt_indirect_fs, rt_probe_trace_fs,
// pt_fs in PathTrace.hlsli) use ray queries and are compiled by DXC (RayHit.hlsli has their bindings); the probe
// updates (rt_probe_irradiance_fs, rt_probe_distance_fs) by glslang.

#include "quench/Bindings.hlsli"
#include "quench/Common.hlsli"
#include "quench/Frame.hlsli"
#include "quench/Fullscreen.hlsli"
#include "quench/Probes.hlsli"
#include "quench/SceneUniforms.h"

#include "RayTracingUniforms.h"

// Radiance above this is clipped: a rare bright hit seen by one pixel would otherwise stay as a speck for the whole
// history of the denoiser.
static const float kMaxRayRadiance = 64.0;

float3 rtClampRadiance(float3 c) {
    const float l = max(max(c.r, c.g), c.b);
    return l > kMaxRayRadiance ? c * (kMaxRayRadiance / l) : c;
}

// The probe rays' directions: a spherical Fibonacci set turned by the frame's rotation.
float3 rtProbeRay(uint i, uint rays, float4x4 rotation) {
    const float n = float(rays);
    const float phi = 6.2831853 * frac(float(i) * 0.6180340);
    const float cosTheta = 1.0 - (2.0 * float(i) + 1.0) / n;
    const float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    return normalize(mul((float3x3)rotation, float3(cos(phi) * sinTheta, cosTheta, sin(phi) * sinTheta)));
}

// The world cell whose probe a slot holds, for a grid starting at `first`.
int3 rtProbeCell(int3 slot, int3 first) {
    const int3 c = quenchProbeCounts();
    return first + (((slot - first) % c) + c) % c;
}

#if defined(QUENCH_ENTRY_rt_reflect_fs) || defined(QUENCH_ENTRY_rt_indirect_fs) ||                                   \
    defined(QUENCH_ENTRY_rt_probe_trace_fs)
#define QUENCH_RT_SAMPLERS 7
#include "RayHit.hlsli"
#endif

#if defined(QUENCH_ENTRY_pt_fs)
#define QUENCH_RT_SAMPLERS 8
#include "PathTrace.hlsli"
#endif

#if defined(QUENCH_ENTRY_rt_reflect_fs)
// One ray per pixel in a direction of the GGX lobe of the G-buffer's normal and roughness, for surfaces below the
// roughness cutoff: rgb the radiance it brings, a the roughness relative to the cutoff (0.02..1; 0: not traced).
float4 rt_reflect_fs(FullscreenOut vin) : SV_Target0 {
    const int2 p = int2(vin.position.xy);
    const float d = rtDepth.Load(int3(p, 0));
    if (d <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    const float4 g = rtGbuffer.Load(int3(p, 0));
    const float cutoff = rtHit.params.x;
    if (g.w >= cutoff) return float4(0.0, 0.0, 0.0, 0.0);
    const float3 P = rtWorldAt(vin.position.xy, d);
    const float3 V = normalize(quenchFrame.cameraPos.xyz - P);
    const float3 N = normalize(g.xyz);
    uint seed = rtSeed(uint2(p), uint(quenchFrame.options.y), 0x9e37u);
    const float2 u = float2(rtRandom(seed), rtRandom(seed));
    const float alpha = max(g.w * g.w, 1e-3);
    float3 R = reflect(-V, rtGgxVisibleNormal(N, V, alpha, u));
    if (dot(R, N) <= 0.0) R = reflect(-V, N);
    const float3 origin = P + N * (rtHit.params.y * max(quenchViewDepth(P), 1.0));
    RtHit hit;
    const float3 radiance = rtRadiance(origin, R, rtHit.params.z, seed, hit);
    return float4(rtClampRadiance(radiance), clamp(g.w / cutoff, 0.02, 1.0));
}
#endif

#if defined(QUENCH_ENTRY_rt_indirect_fs)
// One cosine-weighted ray per pixel over the hemisphere of the G-buffer's normal: the radiance it brings, whose mean
// is the irradiance / pi the surface reflects (the hits read the probe grid for further bounces).
float4 rt_indirect_fs(FullscreenOut vin) : SV_Target0 {
    const int2 p = int2(vin.position.xy);
    const float d = rtDepth.Load(int3(p, 0));
    if (d <= 0.0) return float4(0.0, 0.0, 0.0, 1.0);
    const float3 P = rtWorldAt(vin.position.xy, d);
    const float3 N = normalize(rtGbuffer.Load(int3(p, 0)).xyz);
    uint seed = rtSeed(uint2(p), uint(quenchFrame.options.y), 0x51edu);
    const float3 dir = rtCosineDirection(N, float2(rtRandom(seed), rtRandom(seed)));
    const float3 origin = P + N * (rtHit.params.y * max(quenchViewDepth(P), 1.0));
    RtHit hit;
    const float3 radiance = rtRadiance(origin, dir, rtHit.params.z, seed, hit);
    return float4(rtClampRadiance(radiance), 1.0);
}
#endif

#if defined(QUENCH_ENTRY_rt_probe_trace_fs)
// The probe rays, one texel each (x: ray, y: probe slot): rgb the radiance, a the hit distance (negative where the
// ray hit the back of a surface: the probe is inside geometry; the longest distance on a miss).
float4 rt_probe_trace_fs(FullscreenOut vin) : SV_Target0 {
    const uint ray = uint(vin.position.x), probe = uint(vin.position.y);
    const int3 counts = quenchProbeCounts();
    const int3 slot = int3(int(probe) % counts.x, (int(probe) / counts.x) % counts.y, int(probe) / (counts.x * counts.y));
    const int3 cell = rtProbeCell(slot, int3(quenchFrame.probeGrid.xyz));
    const float3 origin = quenchProbePosition(cell);
    const float3 dir = rtProbeRay(ray, uint(rtHit.probes.x), rtHit.rotation);
    uint seed = rtSeed(uint2(ray, probe), uint(quenchFrame.options.y), 0x2545u);
    const RtHit hit = rtTraceClosest(origin, dir, rtHit.params.z, QUENCH_RT_MASK_VISIBLE, seed);
    if (!hit.hit) return float4(rtClampRadiance(skyRadiance(dir)), rtHit.params.z);
    const RtSurface s = rtSurfaceAt(hit, origin, dir);
    if (s.back) return float4(0.0, 0.0, 0.0, -0.2 * hit.t);
    return float4(rtClampRadiance(rtShade(s, -dir)), hit.t);
}
#endif

#if defined(QUENCH_ENTRY_rt_probe_irradiance_fs) || defined(QUENCH_ENTRY_rt_probe_distance_fs)
cbuffer RayHitBlock : QUENCH_UNIFORM(1) {
    RayHitUniforms rtHit;
};
Texture2D<float4> rayTex : QUENCH_TEXTURE(0);
SamplerState raySampler : QUENCH_SAMPLER(0);
Texture2D<float4> previousTex : QUENCH_TEXTURE(1);
SamplerState previousSampler : QUENCH_SAMPLER(1);

// The interior texel a border texel of a tile copies (the tile's opposite edge, mirrored), so bilinear filtering
// across the border continues the octahedral map.
int2 rtProbeInterior(int2 t, int tile) {
    const int last = tile - 1;
    int2 s = t;
    if (t.y == 0) s = int2(last - t.x, 1);
    if (t.y == last) s = int2(last - t.x, last - 1);
    if (t.x == 0) s = int2(1, last - t.y);
    if (t.x == last) s = int2(last - 1, last - t.y);
    if (t.x == 0 && t.y == 0) s = int2(last - 1, last - 1);
    if (t.x == last && t.y == 0) s = int2(1, last - 1);
    if (t.x == 0 && t.y == last) s = int2(last - 1, 1);
    if (t.x == last && t.y == last) s = int2(1, 1);
    return s;
}

// A texel of an atlas with tiles of `tile` texels: the probe it belongs to, the direction it stores and how much of
// the previous value it keeps (none for a probe that came into the grid this frame).
void rtProbeTexel(int2 texel, int tile, out uint probe, out float3 dir, out float keep) {
    const int3 counts = quenchProbeCounts();
    const int2 tileIndex = texel / tile;
    const int2 local = rtProbeInterior(texel - tileIndex * tile, tile);
    const float2 oct = (float2(local - 1) + 0.5) / float(tile - 2) * 2.0 - 1.0;
    dir = quenchOctDecode(oct);
    const int3 slot = int3(tileIndex.x % counts.x, tileIndex.x / counts.x, tileIndex.y);
    probe = uint(slot.x + slot.y * counts.x + slot.z * counts.x * counts.y);
    const int3 now = rtProbeCell(slot, int3(quenchFrame.probeGrid.xyz));
    const int3 before = rtProbeCell(slot, int3(rtHit.previousGrid.xyz));
    keep = rtHit.probes.w > 0.5 || any(now != before) ? 0.0 : rtHit.probes.z;
}
#endif

#if defined(QUENCH_ENTRY_rt_probe_irradiance_fs)
// The cosine-weighted mean of the probe's rays around the texel's direction (irradiance / pi), blended with the
// previous value by the hysteresis.
float4 rt_probe_irradiance_fs(FullscreenOut vin) : SV_Target0 {
    const int2 texel = int2(vin.position.xy);
    uint probe;
    float3 dir;
    float keep;
    rtProbeTexel(texel, int(quenchFrame.probeCounts.w), probe, dir, keep);
    const uint rays = uint(rtHit.probes.x);
    float3 sum = float3(0.0, 0.0, 0.0);
    float weights = 0.0;
    for (uint r = 0u; r < rays; ++r) {
        const float w = max(dot(dir, rtProbeRay(r, rays, rtHit.rotation)), 0.0);
        if (w <= 0.0) continue;
        const float4 s = rayTex.Load(int3(int(r), int(probe), 0));
        if (s.a > 0.0) sum += s.rgb * w; // back faces bring no light
        weights += w;
    }
    const float3 value = weights > 0.0 ? sum / weights : float3(0.0, 0.0, 0.0);
    // Not lerp with keep 0: the previous atlas starts uninitialized (NaN times 0 stays NaN).
    if (keep <= 0.0) return float4(value, 1.0);
    return float4(lerp(value, previousTex.Load(int3(texel, 0)).rgb, keep), 1.0);
}
#endif

#if defined(QUENCH_ENTRY_rt_probe_distance_fs)
// The mean and mean square distance of the probe's rays in a narrow cone around the texel's direction.
float4 rt_probe_distance_fs(FullscreenOut vin) : SV_Target0 {
    const int2 texel = int2(vin.position.xy);
    uint probe;
    float3 dir;
    float keep;
    rtProbeTexel(texel, int(quenchFrame.probeAtlas.x), probe, dir, keep);
    const uint rays = uint(rtHit.probes.x);
    float2 sum = float2(0.0, 0.0);
    float weights = 0.0;
    for (uint r = 0u; r < rays; ++r) {
        const float c = max(dot(dir, rtProbeRay(r, rays, rtHit.rotation)), 0.0);
        if (c <= 0.0) continue;
        const float w = pow(c, 50.0);
        const float d = min(abs(rayTex.Load(int3(int(r), int(probe), 0)).a), rtHit.probes.y);
        sum += float2(d, d * d) * w;
        weights += w;
    }
    const float2 value = weights > 1e-6 ? sum / weights : float2(rtHit.probes.y, rtHit.probes.y * rtHit.probes.y);
    if (keep <= 0.0) return float4(value, 0.0, 1.0);
    return float4(lerp(value, previousTex.Load(int3(texel, 0)).xy, keep), 0.0, 1.0);
}
#endif
