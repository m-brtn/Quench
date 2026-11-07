#ifndef QUENCH_RAIN_HLSLI
#define QUENCH_RAIN_HLSLI

// What surfaces, a game's passes and the rain's own passes see of the rain (docs/weather.md, "Rain"): the rain
// occlusion map (where the rain reaches), the weather's wetness grid (how wet the ground is, its standing water and
// the precipitation now), the hollows puddles fill and the rings of rain drops on standing water. Both maps are
// regions of the weather maps (surface sampler 14, FrameUniforms::rain.x 1 when they are there).

#include "quench/Common.hlsli"
#include "quench/Frame.hlsli"

// A point this far (m) below the highest surface along the rain is under cover.
static const float QUENCH_RAIN_COVER_DEPTH = 0.4;

// The wetness grid at a world xz: float4(ground wetness, standing water, precipitation, 1), bilinear between the
// cells' centres (beyond the grid its edge cells').
float4 quenchWetnessAt(Texture2D<float4> maps, SamplerState s, float2 xz) {
    const float n = float(QUENCH_WETNESS_GRID);
    const float2 cell = clamp((xz - quenchFrame.rainGrid.xy) * quenchFrame.rainGrid.z * n, 0.5, n - 0.5);
    const float2 uv = float2(cell.x / float(QUENCH_WEATHER_MAPS_WIDTH),
                             (float(QUENCH_WETNESS_ROW) + cell.y) / float(QUENCH_WEATHER_MAPS_HEIGHT));
    return maps.SampleLevel(s, uv, 0.0);
}

// A world point in the rain occlusion map: xy its uv (0..1, top left 0, 0), z its depth (0..1, 0 at the map's top).
float3 quenchRainMapPos(float3 P) {
    const float4 c = mul(quenchFrame.rainViewProj, float4(P, 1.0));
    return float3(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5, c.z);
}

// The depth (0..1) of the highest surface the rain meets at a uv of the occlusion map (1 where it meets none).
float quenchRainOccluder(Texture2D<float4> maps, SamplerState s, float2 uv) {
    const float rows = float(QUENCH_WETNESS_ROW - QUENCH_RAIN_MAP_ROW);
    const float edge = 0.5 / rows;
    const float v =
        (float(QUENCH_RAIN_MAP_ROW) + clamp(uv.y, edge, 1.0 - edge) * rows) / float(QUENCH_WEATHER_MAPS_HEIGHT);
    return maps.SampleLevel(s, float2(uv.x, v), 0.0).r;
}

// How open a point is to the rain: 1 in the open (also outside the occlusion map), 0 under cover. Surfaces look up a
// point a little off them along their normal, so that walls see the open ground beside them and floors their roof.
float quenchRainExposure(Texture2D<float4> maps, SamplerState s, float3 P) {
    const float3 m = quenchRainMapPos(P);
    if (any(m.xy < 0.0) || any(m.xy > 1.0) || m.z < 0.0) return 1.0;
    const float top = quenchRainOccluder(maps, s, m.xy);
    return saturate(1.0 - (m.z - top) * quenchFrame.rain.y / QUENCH_RAIN_COVER_DEPTH);
}

// The relief whose hollows puddles fill (MaterialDesc::puddles): 0 deepest .. 1 highest, value noise at 6 m and
// 1.5 m. Standing water w fills it up to 0.42 w: about a third of a flat surface under full puddles.
float quenchPuddleHeight(float2 xz) { return valueNoise(xz / 6.0) * 0.7 + valueNoise(xz / 1.5 + 13.1) * 0.3; }

// The rings of rain drops on standing water: the slope of its surface (d height / dx, d height / dz) at a world xz,
// at time t (s) for precipitation 0..1. Two grids of cells 0.4 m across, offset; a drop falls in a cell every 0.8 s at
// a place of its own, in a share of the cells that grows with the rain, and its ring spreads and fades.
float2 quenchRipples(float2 xz, float t, float rain) {
    float2 slope = float2(0.0, 0.0);
    [unroll] for (int layer = 0; layer < 2; ++layer) {
        const float2 p = xz / 0.4 + float(layer) * float2(0.37, 0.71);
        const float2 base = floor(p);
        [unroll] for (int j = -1; j <= 1; ++j) {
            [unroll] for (int k = -1; k <= 1; ++k) {
                const float2 id = base + float2(float(k), float(j));
                const float phase = t / 0.8 + hash12(id + float(layer) * 17.0);
                const float drop = floor(phase);
                if (hash12(id * 1.37 + drop * 3.1 + float(layer) * 7.0) > rain) continue;
                const float age = phase - drop;
                const float2 d = p - (id + 0.15 + 0.7 * hash22(id * 2.11 + drop + 5.0));
                const float r = length(d);
                const float front = age * 0.6;
                const float band = saturate(1.0 - abs(r - front) * 4.0);
                const float fade = (1.0 - age) * (1.0 - age);
                slope -= d / max(r, 1e-3) * sin((r - front) * 25.0) * band * fade;
            }
        }
    }
    return slope * 0.3;
}

#endif
