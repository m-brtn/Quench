// Ray-traced reflections, indirect light, the probe grid and the path tracer of quench::render::Renderer
// (docs/render.md, "Ray-traced lighting"). The tracing entries (rt_reflect_fs, rt_indirect_fs, rt_probe_trace_fs,
// pt_fs in PathTrace.hlsli) use ray queries and are compiled by DXC (RayHit.hlsli has their bindings); the probe
// updates (rt_probe_data_fs, rt_probe_irradiance_fs, rt_probe_distance_fs) by glslang. Each tracing entry has a _maps
// variant whose hits read normal maps (QUENCH_RT_NORMAL_MAPS), used while an instance in the top level has one: the
// tangent frame costs the path tracer a few percent even where no hit reads a map.

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

// The probes of one cascade: its rows in the probe ray target (the cascades' rows follow each other).
uint rtProbesPerCascade() {
    const int3 c = quenchProbeCounts();
    return uint(c.x * c.y * c.z);
}

// A probe's state in its data texel (rt_probe_data_fs, w): the updates in a row whose rays found no surface near it
// or found it inside geometry, up to kProbeIdle, when classification leaves it out: its rays are a quarter of the set
// (every fourth) and it is not shaded with; plus 16 where this update traced only that quarter.
static const float kProbeIdle = 8.0;
static const float kProbeQuarter = 16.0;

bool rtProbeLeftOut(float state) {
    return (quenchProbeFlags() & kQuenchProbeClassification) != 0u &&
           (state >= kProbeQuarter ? state - kProbeQuarter : state) >= kProbeIdle;
}

// Probes traced in turns (RenderSettings::probeUpdatePeriod; n = rtHit.probeTurns.x of 1, 2, 4 or 8): a probe's turn
// is a sub-lattice of the grid, from its slot's bits: bit 0 the parity of x + y + z, bit 1 that of x, bit 2 that of y
// (two turns: a 3D checkerboard; eight: every other probe along each axis); its turn is those bits modulo n. The rows
// of the probe ray target are ordered by turn, so one turn's rays are one band of rows; with one turn the row is the
// slot's index. The counts are even.
uint rtProbeTurnBits(int3 slot) {
    return uint((slot.x + slot.y + slot.z) & 1) | (uint(slot.x & 1) << 1) | (uint(slot.y & 1) << 2);
}

uint rtProbeRow(int3 slot, uint turns) {
    const int3 c = quenchProbeCounts();
    if (turns <= 1u) return uint(slot.x + slot.y * c.x + slot.z * c.x * c.y);
    const int3 h = c / 2;
    const uint bits = rtProbeTurnBits(slot);
    const uint index = uint((slot.x >> 1) + (slot.y >> 1) * h.x + (slot.z >> 1) * h.x * h.y);
    const uint band = uint(c.x * c.y * c.z) / turns;
    return (bits & (turns - 1u)) * band + index * (8u / turns) + bits / turns;
}

int3 rtProbeSlotOfRow(uint row, uint turns) {
    const int3 c = quenchProbeCounts();
    if (turns <= 1u) return int3(int(row) % c.x, (int(row) / c.x) % c.y, int(row) / (c.x * c.y));
    const int3 h = c / 2;
    const uint band = uint(c.x * c.y * c.z) / turns, per = 8u / turns;
    const uint turn = row / band, i = row % band;
    const uint bits = turn + (i % per) * turns;
    const int index = int(i / per);
    const int bx = int(bits >> 1u) & 1, by = int(bits >> 2u) & 1;
    const int bz = int(bits & 1u) ^ bx ^ by;
    return int3(index % h.x, (index / h.x) % h.y, index / (h.x * h.y)) * 2 + int3(bx, by, bz);
}

#if defined(QUENCH_ENTRY_rt_reflect_maps_fs)
#define QUENCH_ENTRY_rt_reflect_fs
#define rt_reflect_fs rt_reflect_maps_fs
#endif
#if defined(QUENCH_ENTRY_rt_indirect_maps_fs)
#define QUENCH_ENTRY_rt_indirect_fs
#define rt_indirect_fs rt_indirect_maps_fs
#endif
#if defined(QUENCH_ENTRY_rt_probe_trace_maps_fs)
#define QUENCH_ENTRY_rt_probe_trace_fs
#define rt_probe_trace_fs rt_probe_trace_maps_fs
#endif
#if defined(QUENCH_ENTRY_pt_maps_fs)
#define QUENCH_ENTRY_pt_fs
#define pt_fs pt_maps_fs
#endif
#if defined(QUENCH_ENTRY_rt_reflect_maps_fs) || defined(QUENCH_ENTRY_rt_indirect_maps_fs) ||                         \
    defined(QUENCH_ENTRY_rt_probe_trace_maps_fs) || defined(QUENCH_ENTRY_pt_maps_fs)
#define QUENCH_RT_NORMAL_MAPS
#endif

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
// The probe rays, one texel each (x: ray, y: the cascade's rows, then the probe's row in them, rtProbeRow; drawn for
// this frame's turn): rgb the radiance, a the hit distance (-0.2 times it where the ray hit the back of a surface:
// the probe is inside geometry; the longest distance on a miss). A probe starts at its offset and a probe left out
// traces every fourth ray (its data from the last update, in the irradiance atlas; none after a restart or for a
// probe new in the grid).
float4 rt_probe_trace_fs(FullscreenOut vin) : SV_Target0 {
    const uint ray = uint(vin.position.x), row = uint(vin.position.y);
    const uint perCascade = rtProbesPerCascade();
    const int cascade = int(row / perCascade);
    const int3 slot = rtProbeSlotOfRow(row % perCascade, uint(rtHit.probeTurns.x));
    const int3 first = quenchProbeFirst(cascade);
    const int3 cell = rtProbeCell(slot, first);
    float3 origin = quenchProbePosition(cell, cascade);
    const uint flags = quenchProbeFlags();
    if (flags != 0u && rtHit.probes.w < 0.5 && all(cell == rtProbeCell(slot, int3(rtHit.previousGrid[cascade].xyz)))) {
        const float4 data = rtProbeIrradiance.Load(int3(quenchProbeDataTexel(slot, cascade), 0));
        origin += data.xyz;
        if (rtProbeLeftOut(data.w) && (ray & 3u) != 0u) return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float3 dir = rtProbeRay(ray, uint(rtHit.probes.x), rtHit.rotation);
    uint seed = rtSeed(uint2(ray, row), uint(quenchFrame.options.y), 0x2545u);
    const RtHit hit = rtTraceClosest(origin, dir, rtHit.params.z, QUENCH_RT_MASK_VISIBLE, seed);
    if (!hit.hit) return float4(rtClampRadiance(skyRadiance(dir)), rtHit.params.z);
    const RtSurface s = rtSurfaceAt(hit, origin, dir);
    if (s.back) return float4(0.0, 0.0, 0.0, -0.2 * hit.t);
    return float4(rtClampRadiance(rtShade(s, -dir)), hit.t);
}
#endif

#if defined(QUENCH_ENTRY_rt_probe_data_fs) || defined(QUENCH_ENTRY_rt_probe_irradiance_fs) ||                       \
    defined(QUENCH_ENTRY_rt_probe_distance_fs)
cbuffer RayHitBlock : QUENCH_UNIFORM(1) {
    RayHitUniforms rtHit;
};
Texture2D<float4> rayTex : QUENCH_TEXTURE(0);
SamplerState raySampler : QUENCH_SAMPLER(0);
Texture2D<float4> previousTex : QUENCH_TEXTURE(1);
SamplerState previousSampler : QUENCH_SAMPLER(1);

// What becomes of a probe's texels this frame: traced (blended with the previous value), kept (not the probe's turn:
// the previous value) or emptied (the probe came into the grid this frame and it is not its turn: no value, its
// irradiance alpha 0 leaves it out of the shading until its turn).
static const int kProbeTraced = 0;
static const int kProbeKept = 1;
static const int kProbeEmptied = 2;

struct RtProbe {
    int3 slot;
    int cascade;
    uint row;   // of its rays
    bool fresh; // no history: a restart, or the probe came into the grid this frame
    int state;
};

// The probe of a tile (in tiles: the texel of the data target).
RtProbe rtProbeAt(int2 tile) {
    const int3 counts = quenchProbeCounts();
    RtProbe p;
    p.cascade = tile.y / counts.z;
    p.slot = int3(tile.x % counts.x, tile.x / counts.x, tile.y % counts.z);
    const uint turns = uint(rtHit.probeTurns.x);
    p.row = uint(p.cascade) * rtProbesPerCascade() + rtProbeRow(p.slot, turns);
    const int3 now = rtProbeCell(p.slot, quenchProbeFirst(p.cascade));
    const int3 before = rtProbeCell(p.slot, int3(rtHit.previousGrid[p.cascade].xyz));
    p.fresh = rtHit.probes.w > 0.5 || any(now != before);
    p.state = (rtProbeTurnBits(p.slot) & (turns - 1u)) == uint(rtHit.probeTurns.y) ? kProbeTraced
              : p.fresh                                                           ? kProbeEmptied
                                                                                  : kProbeKept;
    return p;
}
#endif

#if defined(QUENCH_ENTRY_rt_probe_irradiance_fs) || defined(QUENCH_ENTRY_rt_probe_distance_fs)
// This update's probe data (rt_probe_data_fs; a placeholder without relocation and classification). The distance
// update also reads the previous irradiance: its alpha says which probes have a value.
#if defined(QUENCH_ENTRY_rt_probe_irradiance_fs)
Texture2D<float4> probeDataTex : QUENCH_TEXTURE(2);
SamplerState probeDataSampler : QUENCH_SAMPLER(2);
#else
Texture2D<float4> previousLightTex : QUENCH_TEXTURE(2);
SamplerState previousLightSampler : QUENCH_SAMPLER(2);
Texture2D<float4> probeDataTex : QUENCH_TEXTURE(3);
SamplerState probeDataSampler : QUENCH_SAMPLER(3);
#endif

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

// A texel of an atlas with tiles of `tile` texels: its probe and the direction it stores.
RtProbe rtProbeTexel(int2 texel, int tile, out float3 dir) {
    const int2 tileIndex = texel / tile;
    const int2 local = rtProbeInterior(texel - tileIndex * tile, tile);
    const float2 oct = (float2(local - 1) + 0.5) / float(tile - 2) * 2.0 - 1.0;
    dir = quenchOctDecode(oct);
    return rtProbeAt(tileIndex);
}

// The stride over a traced probe's rays (4 where it traced a quarter of them); 0 where classification leaves it out.
uint rtProbeStride(RtProbe p) {
    if (quenchProbeFlags() == 0u) return 1u;
    const float state = probeDataTex.Load(int3(quenchProbeTile(p.slot, p.cascade), 0)).w;
    if (rtProbeLeftOut(state)) return 0u;
    return state >= kProbeQuarter ? 4u : 1u;
}
#endif

#if defined(QUENCH_ENTRY_rt_probe_data_fs)
// Relocation (Majercik et al. 2021, "Scaling Probe-Based Real-Time Dynamic Global Illumination for Production"):
// a probe whose rays see more than kProbeInside back faces is inside geometry and moves through the nearest one; a
// probe nearer than kProbeMinFront spacings to a surface moves away from it; else it moves back toward the middle of
// its cell, never nearer than that to a surface. A move that would take it further than kProbeMaxOffset spacings
// from the middle is not made. Classification: a probe is near a surface when a ray hits one within kProbeNear
// spacings of the middle along every axis (the surfaces that can be shaded with it).
static const float kProbeInside = 0.25;
static const float kProbeMinFront = 0.2;
static const float kProbeMaxOffset = 0.45;
static const float kProbeNear = 1.5;

// A traced probe's data (xyz: offset from the middle of its cell, m; w: state) from its rays and its last data.
float4 rtProbeUpdate(RtProbe p, float4 previous) {
    const uint flags = quenchProbeFlags();
    const float spacing = quenchProbeSpacing(p.cascade);
    const uint rays = uint(rtHit.probes.x);
    const bool quarter = rtProbeLeftOut(previous.w);
    uint count = 0u, backfaces = 0u;
    bool near = false;
    float closestBack = 1e30, closestFront = 1e30;
    float3 backDir = float3(0.0, 0.0, 0.0), frontDir = float3(0.0, 0.0, 0.0);
    for (uint r = 0u; r < rays; r += quarter ? 4u : 1u) {
        const float a = rayTex.Load(int3(int(r), int(p.row), 0)).a;
        const float3 dir = rtProbeRay(r, rays, rtHit.rotation);
        ++count;
        if (a < 0.0) {
            ++backfaces;
            if (-5.0 * a < closestBack) {
                closestBack = -5.0 * a;
                backDir = dir;
            }
        } else if (a < rtHit.params.z) {
            if (a < closestFront) {
                closestFront = a;
                frontDir = dir;
            }
            if (all(abs(previous.xyz + dir * a) < kProbeNear * spacing)) near = true;
        }
    }
    const bool inside = float(backfaces) > kProbeInside * float(count);
    const float idle = previous.w >= kProbeQuarter ? previous.w - kProbeQuarter : previous.w;
    const float state = (near && !inside ? 0.0 : min(idle + 1.0, kProbeIdle)) + (quarter ? kProbeQuarter : 0.0);
    float3 offset = float3(0.0, 0.0, 0.0);
    if ((flags & kQuenchProbeRelocation) != 0u) {
        offset = previous.xyz;
        const float minFront = kProbeMinFront * spacing;
        float3 moved = offset;
        if (inside) {
            moved += backDir * (closestBack + 0.5 * minFront);
        } else if (closestFront < minFront) {
            moved -= frontDir * (minFront - closestFront);
        } else {
            const float l = length(offset);
            if (l > 0.0) moved -= offset * (min(closestFront - minFront, l) / l);
        }
        if (dot(moved, moved) < kProbeMaxOffset * kProbeMaxOffset * spacing * spacing) offset = moved;
    }
    return float4(offset, state);
}

// A probe's data this update, a texel per probe (target: counts.x * counts.y by counts.z * cascades): traced probes
// updated from their rays, kept ones as they were, new ones at the middle of their cells.
float4 rt_probe_data_fs(FullscreenOut vin) : SV_Target0 {
    const int2 texel = int2(vin.position.xy);
    const RtProbe p = rtProbeAt(texel);
    if (p.state == kProbeEmptied) return float4(0.0, 0.0, 0.0, 0.0);
    const float4 previous = p.fresh ? float4(0.0, 0.0, 0.0, 0.0) : previousTex.Load(int3(texel, 0));
    if (p.state == kProbeKept) return previous;
    return rtProbeUpdate(p, previous);
}
#endif

#if defined(QUENCH_ENTRY_rt_probe_irradiance_fs)
// The cosine-weighted mean of the probe's rays around the texel's direction (irradiance / pi), blended with the
// previous value by the hysteresis; alpha 1 where the probe has a value. Below the tiles (with relocation or
// classification) a copy of this update's probe data, which the shading and the next frame's rays read.
float4 rt_probe_irradiance_fs(FullscreenOut vin) : SV_Target0 {
    const int2 texel = int2(vin.position.xy);
    const int tile = int(quenchFrame.probeCounts.w);
    const int3 counts = quenchProbeCounts();
    const int tiles = counts.z * quenchProbeCascades() * tile;
    if (texel.y >= tiles) {
        const int2 at = int2(texel.x, texel.y - tiles);
        return at.x < counts.x * counts.y ? probeDataTex.Load(int3(at, 0)) : float4(0.0, 0.0, 0.0, 0.0);
    }
    const float4 previous = previousTex.Load(int3(texel, 0));
    float3 dir;
    const RtProbe p = rtProbeTexel(texel, tile, dir);
    if (p.state == kProbeKept) return previous;
    if (p.state == kProbeEmptied) return float4(0.0, 0.0, 0.0, 0.0);
    const uint stride = rtProbeStride(p);
    if (stride == 0u) return float4(0.0, 0.0, 0.0, 0.0);
    const float keep = p.fresh || previous.a <= 0.5 ? 0.0 : rtHit.probes.z;
    const uint rays = uint(rtHit.probes.x);
    float3 sum = float3(0.0, 0.0, 0.0);
    float weights = 0.0;
    for (uint r = 0u; r < rays; r += stride) {
        const float w = max(dot(dir, rtProbeRay(r, rays, rtHit.rotation)), 0.0);
        if (w <= 0.0) continue;
        const float4 s = rayTex.Load(int3(int(r), int(p.row), 0));
        if (s.a > 0.0) sum += s.rgb * w; // back faces bring no light
        weights += w;
    }
    const float3 value = weights > 0.0 ? sum / weights : float3(0.0, 0.0, 0.0);
    // Not lerp with keep 0: the previous atlas starts uninitialized (NaN times 0 stays NaN).
    if (keep <= 0.0) return float4(value, 1.0);
    return float4(lerp(value, previous.rgb, keep), 1.0);
}
#endif

#if defined(QUENCH_ENTRY_rt_probe_distance_fs)
// The mean and mean square distance of the probe's rays in a narrow cone around the texel's direction.
float4 rt_probe_distance_fs(FullscreenOut vin) : SV_Target0 {
    const int2 texel = int2(vin.position.xy);
    const int tile = int(quenchFrame.probeAtlas.x);
    // The probe's previous irradiance alpha, from an interior texel of its irradiance tile (only where a probe can
    // have none: after a frame in turns, or with classification).
    const int2 lightTexel = texel / tile * int(quenchFrame.probeCounts.w) + 1;
    const bool hadValue = rtHit.probeTurns.z < 0.5 || previousLightTex.Load(int3(lightTexel, 0)).a > 0.5;
    float3 dir;
    const RtProbe p = rtProbeTexel(texel, tile, dir);
    const float longest = rtHit.probes.y * float(1 << p.cascade);
    if (p.state == kProbeKept) return previousTex.Load(int3(texel, 0));
    if (p.state == kProbeEmptied) return float4(longest, longest * longest, 0.0, 1.0);
    const uint stride = rtProbeStride(p);
    if (stride == 0u) return float4(longest, longest * longest, 0.0, 1.0);
    const float keep = p.fresh || !hadValue ? 0.0 : rtHit.probes.z;
    const uint rays = uint(rtHit.probes.x);
    float2 sum = float2(0.0, 0.0);
    float weights = 0.0;
    for (uint r = 0u; r < rays; r += stride) {
        const float c = max(dot(dir, rtProbeRay(r, rays, rtHit.rotation)), 0.0);
        if (c <= 0.0) continue;
        const float w = pow(c, 50.0);
        const float d = min(abs(rayTex.Load(int3(int(r), int(p.row), 0)).a), longest);
        sum += float2(d, d * d) * w;
        weights += w;
    }
    const float2 value = weights > 1e-6 ? sum / weights : float2(longest, longest * longest);
    if (keep <= 0.0) return float4(value, 0.0, 1.0);
    return float4(lerp(value, previousTex.Load(int3(texel, 0)).xy, keep), 0.0, 1.0);
}
#endif
