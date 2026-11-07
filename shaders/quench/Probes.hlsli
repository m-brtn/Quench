#ifndef QUENCH_PROBES_HLSLI
#define QUENCH_PROBES_HLSLI

// The irradiance probe grid of quench::render::Renderer (IndirectLight::Probes and RayTraced; FrameUniforms
// probeGrid, probeCounts, probeAtlas): probes on a world-aligned grid around the camera, each an octahedral map of
// the light arriving from every direction (cosine-weighted, stored as irradiance / pi, so color += value * albedo as
// with the sky ambient) and one of the distance to the surfaces around it (mean and mean square), which keeps light
// from leaking through walls (Majercik et al. 2019, "Dynamic Diffuse Global Illumination with Ray-Traced Irradiance
// Fields"). Cascades (RenderSettings::probeCascades): grids of the same counts around the same point, each twice the
// spacing of the one before; a point takes the finest cascade whose probes surround it, blended into the next one
// over the last cell before its edge. A probe's maps are one tile of each atlas: its slot is its cell modulo the
// counts (a cascade scrolls with the camera without moving data), tile x = slot.x + slot.y * counts.x, tile y =
// slot.z + cascade * counts.z; tiles have a one-texel border copied from the opposite edge for bilinear filtering.
// The irradiance alpha is 1 where a probe has a value and 0 where it has none (a probe that came into the grid and
// waits for its turn to be traced, or one left out by classification). With relocation or classification
// (probeAtlas.w) the irradiance atlas has rows of data below the tiles, a texel per probe (quenchProbeDataTexel):
// xyz the probe's offset from the middle of its cell (m), w its state (raylight.hlsl, rt_probe_data_fs).

#include "quench/Frame.hlsli"

static const uint kQuenchProbeRelocation = 1u;
static const uint kQuenchProbeClassification = 2u;

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
int quenchProbeCascades() { return clamp(int(quenchFrame.probeAtlas.z), 1, QUENCH_MAX_PROBE_CASCADES); }
uint quenchProbeFlags() { return uint(quenchFrame.probeAtlas.w); }
int3 quenchProbeFirst(int cascade) { return int3(quenchFrame.probeGrid[cascade].xyz); }
float quenchProbeSpacing(int cascade) { return quenchFrame.probeGrid[cascade].w; }

// A probe sits in the middle of its cell, off the planes at whole multiples of the spacing (floors at y = 0).
float3 quenchProbePosition(int3 cell, int cascade) { return (float3(cell) + 0.5) * quenchProbeSpacing(cascade); }

// The storage slot of a world cell.
int3 quenchProbeSlot(int3 cell) {
    const int3 c = quenchProbeCounts();
    return ((cell % c) + c) % c;
}

// A probe's tile (in tiles) in both atlases.
int2 quenchProbeTile(int3 slot, int cascade) {
    const int3 c = quenchProbeCounts();
    return int2(slot.x + slot.y * c.x, slot.z + cascade * c.z);
}

// The rows of probe data below the irradiance atlas's tiles (0 without relocation and classification).
float quenchProbeDataRows() {
    return quenchProbeFlags() != 0u ? float(quenchProbeCounts().z * quenchProbeCascades()) : 0.0;
}

// The texel of a probe's data in the irradiance atlas.
int2 quenchProbeDataTexel(int3 slot, int cascade) {
    const int3 c = quenchProbeCounts();
    const int tiles = c.z * quenchProbeCascades() * int(quenchFrame.probeCounts.w);
    return int2(slot.x + slot.y * c.x, tiles + cascade * c.z + slot.z);
}

// Atlas size in texels for tiles of `tile` texels per side, with `rows` of data below them.
float2 quenchProbeAtlasSize(float tile, float rows) {
    const int3 c = quenchProbeCounts();
    return float2(float(c.x * c.y), float(c.z * quenchProbeCascades())) * tile + float2(0.0, rows);
}

// The uv of a direction in a probe's tile.
float2 quenchProbeUv(int3 slot, int cascade, float3 dir, float tile, float rows) {
    const float2 origin = float2(quenchProbeTile(slot, cascade)) * tile;
    const float2 oct = quenchOctEncode(dir) * 0.5 + 0.5;
    return (origin + 1.0 + oct * (tile - 2.0)) / quenchProbeAtlasSize(tile, rows);
}

// The 8 probes of one cascade around the biased point: the sum of their weighted values (`weights` the sum of the
// weights), each weighted by trilinear position, by whether the surface faces it, by whether it can see the probe (a
// Chebyshev test against the probe's distances) and by whether the probe has a value.
float3 quenchProbeCascadeLight(Texture2D<float4> irradiance, Texture2D<float4> distances, SamplerState s,
                               int cascade, float3 P, float3 N, float3 biased, out float weights) {
    const float spacing = quenchProbeSpacing(cascade);
    const int3 first = quenchProbeFirst(cascade);
    const int3 counts = quenchProbeCounts();
    const float irradianceTile = quenchFrame.probeCounts.w, distanceTile = quenchFrame.probeAtlas.x;
    const float rows = quenchProbeDataRows();
    const bool relocated = (quenchProbeFlags() & kQuenchProbeRelocation) != 0u;
    const float3 g = biased / spacing - 0.5;
    const int3 base = int3(floor(g));
    const float3 alpha = g - float3(base);
    float3 sum = float3(0.0, 0.0, 0.0);
    weights = 0.0;
    for (int i = 0; i < 8; ++i) {
        const int3 offset = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const int3 cell = base + offset;
        if (any(cell < first) || any(cell >= first + counts)) continue;
        const int3 slot = quenchProbeSlot(cell);
        float3 probe = quenchProbePosition(cell, cascade);
        if (relocated) probe += irradiance.Load(int3(quenchProbeDataTexel(slot, cascade), 0)).xyz;
        const float3 trilinear = lerp(1.0 - alpha, alpha, float3(offset));
        const float3 toProbe = normalize(probe - P);
        const float facing = (dot(toProbe, N) + 1.0) * 0.5;
        float w = facing * facing + 0.2;
        const float3 fromProbe = biased - probe;
        const float d = length(fromProbe);
        const float2 m =
            distances.SampleLevel(s, quenchProbeUv(slot, cascade, fromProbe / max(d, 1e-4), distanceTile, 0.0), 0.0)
                .xy;
        if (d > m.x) {
            const float variance = abs(m.y - m.x * m.x);
            const float diff = d - m.x;
            const float chebyshev = variance / max(variance + diff * diff, 1e-6);
            w *= max(chebyshev * chebyshev * chebyshev, 0.05);
        }
        w = max(w, 1e-6);
        if (w < 0.2) w *= w * w / 0.04; // small weights fall off faster: less light through thin walls
        const float4 e = irradiance.SampleLevel(s, quenchProbeUv(slot, cascade, N, irradianceTile, rows), 0.0);
        w *= trilinear.x * trilinear.y * trilinear.z * e.a;
        sum += w * e.rgb;
        weights += w;
    }
    return sum;
}

// The light the grid gives a surface at P with normal N seen from V (irradiance / pi): from the finest cascade
// whose probes surround the point (blended into the next over its last cell), from the last one with what probes it
// has there, `fallback` beyond it.
float3 quenchProbeIrradiance(Texture2D<float4> irradiance, Texture2D<float4> distances, SamplerState s, float3 P,
                             float3 N, float3 V, float3 fallback) {
    const int cascades = quenchProbeCascades();
    const int3 counts = quenchProbeCounts();
    const float3 shift = N * 0.2 + V * 0.8;
    float bias = quenchFrame.probeAtlas.y;
    if (cascades == 1) {
        float weights;
        const float3 sum = quenchProbeCascadeLight(irradiance, distances, s, 0, P, N, P + shift * bias, weights);
        return weights > 1e-5 ? sum / weights : fallback;
    }
    float3 light = float3(0.0, 0.0, 0.0);
    float left = 1.0;
    for (int c = 0; c < cascades; ++c, bias *= 2.0) {
        const float3 biased = P + shift * bias;
        float blend = 1.0;
        if (c + 1 < cascades) {
            // How far (cells) inside the region where all 8 probes around a point are in the cascade.
            const float3 g = biased / quenchProbeSpacing(c) - 0.5 - float3(quenchProbeFirst(c));
            const float3 inside = min(g, float3(counts - 1) - g);
            blend = saturate(min(min(inside.x, inside.y), inside.z));
            if (blend <= 0.0) continue;
        }
        float weights;
        const float3 sum = quenchProbeCascadeLight(irradiance, distances, s, c, P, N, biased, weights);
        if (weights > 1e-5) {
            light += left * blend * (sum / weights);
            left *= 1.0 - blend;
        }
        if (left <= 0.0) break;
    }
    return light + left * fallback;
}

#endif
