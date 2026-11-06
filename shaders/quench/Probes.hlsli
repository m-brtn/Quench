#ifndef QUENCH_PROBES_HLSLI
#define QUENCH_PROBES_HLSLI

// The irradiance probe grid of quench::render::Renderer (IndirectLight::Probes and RayTraced; FrameUniforms
// probeGrid, probeCounts, probeAtlas): probes on a world-aligned grid around the camera, each an octahedral map of
// the light arriving from every direction (cosine-weighted, stored as irradiance / pi, so color += value * albedo as
// with the sky ambient) and one of the distance to the surfaces around it (mean and mean square), which keeps light
// from leaking through walls (Majercik et al. 2019, "Dynamic Diffuse Global Illumination with Ray-Traced Irradiance
// Fields"). A probe's maps are one tile of each atlas: its slot is its cell modulo the counts (the grid scrolls with
// the camera without moving data), tile x = slot.x + slot.y * counts.x, tile y = slot.z; tiles have a one-texel
// border copied from the opposite edge for bilinear filtering.

#include "quench/Frame.hlsli"

float quenchSignNotZero(float v) { return v >= 0.0 ? 1.0 : -1.0; }

// Octahedral map of the unit sphere onto [-1, 1]^2, +y at the center.
float2 quenchOctEncode(float3 n) {
    const float3 a = n / max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    float2 e = a.xz;
    if (a.y < 0.0) e = (1.0 - abs(a.zx)) * float2(quenchSignNotZero(a.x), quenchSignNotZero(a.z));
    return e;
}

float3 quenchOctDecode(float2 e) {
    float3 n = float3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    if (n.y < 0.0) n.xz = (1.0 - abs(n.zx)) * float2(quenchSignNotZero(n.x), quenchSignNotZero(n.z));
    return normalize(n);
}

int3 quenchProbeCounts() { return int3(quenchFrame.probeCounts.xyz); }

// A probe sits in the middle of its cell, off the planes at whole multiples of the spacing (floors at y = 0).
float3 quenchProbePosition(int3 cell) { return (float3(cell) + 0.5) * quenchFrame.probeGrid.w; }

// The storage slot of a world cell.
int3 quenchProbeSlot(int3 cell) {
    const int3 c = quenchProbeCounts();
    return ((cell % c) + c) % c;
}

// Atlas size in texels for tiles of `tile` texels per side.
float2 quenchProbeAtlasSize(float tile) {
    const int3 c = quenchProbeCounts();
    return float2(float(c.x * c.y), float(c.z)) * tile;
}

// The uv of a direction in a probe's tile.
float2 quenchProbeUv(int3 slot, float3 dir, float tile) {
    const int3 c = quenchProbeCounts();
    const float2 origin = float2(float(slot.x + slot.y * c.x), float(slot.z)) * tile;
    const float2 oct = quenchOctEncode(dir) * 0.5 + 0.5;
    return (origin + 1.0 + oct * (tile - 2.0)) / quenchProbeAtlasSize(tile);
}

// The light the grid gives a surface at P with normal N seen from V (irradiance / pi), from the 8 probes around it,
// each weighted by trilinear position, by whether the surface faces it and by whether it can see the probe (a
// Chebyshev test against the probe's distances); `fallback` outside the grid.
float3 quenchProbeIrradiance(Texture2D<float4> irradiance, Texture2D<float4> distances, SamplerState s, float3 P,
                             float3 N, float3 V, float3 fallback) {
    const float spacing = quenchFrame.probeGrid.w;
    const int3 first = int3(quenchFrame.probeGrid.xyz);
    const int3 counts = quenchProbeCounts();
    const float irradianceTile = quenchFrame.probeCounts.w, distanceTile = quenchFrame.probeAtlas.x;
    const float3 biased = P + (N * 0.2 + V * 0.8) * quenchFrame.probeAtlas.y;
    const float3 g = biased / spacing - 0.5;
    const int3 base = int3(floor(g));
    const float3 alpha = g - float3(base);
    float3 sum = float3(0.0, 0.0, 0.0);
    float weights = 0.0;
    for (int i = 0; i < 8; ++i) {
        const int3 offset = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const int3 cell = base + offset;
        if (any(cell < first) || any(cell >= first + counts)) continue;
        const int3 slot = quenchProbeSlot(cell);
        const float3 probe = quenchProbePosition(cell);
        const float3 trilinear = lerp(1.0 - alpha, alpha, float3(offset));
        const float3 toProbe = normalize(probe - P);
        const float facing = (dot(toProbe, N) + 1.0) * 0.5;
        float w = facing * facing + 0.2;
        const float3 fromProbe = biased - probe;
        const float d = length(fromProbe);
        const float2 m = distances.SampleLevel(s, quenchProbeUv(slot, fromProbe / max(d, 1e-4), distanceTile), 0.0).xy;
        if (d > m.x) {
            const float variance = abs(m.y - m.x * m.x);
            const float diff = d - m.x;
            const float chebyshev = variance / max(variance + diff * diff, 1e-6);
            w *= max(chebyshev * chebyshev * chebyshev, 0.05);
        }
        w = max(w, 1e-6);
        if (w < 0.2) w *= w * w / 0.04; // small weights fall off faster: less light through thin walls
        w *= trilinear.x * trilinear.y * trilinear.z;
        sum += w * irradiance.SampleLevel(s, quenchProbeUv(slot, N, irradianceTile), 0.0).rgb;
        weights += w;
    }
    return weights > 1e-5 ? sum / weights : fallback;
}

#endif
