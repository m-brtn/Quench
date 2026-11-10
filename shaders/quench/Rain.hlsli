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

// The share of the weather's rain and snow that falls at a height: all of it under the clouds, none at and above
// their base (FrameUniforms::rainGrid.w; weather::Weather::precipitation says the same), less over the last 100 m
// under it. The wetness grid knows no heights: its rain is the ground's, and a summit in or over the clouds stays
// dry and bare.
float quenchFallReach(float y) {
    return quenchFrame.rainGrid.w > 0.0 ? saturate((quenchFrame.rainGrid.w - y) / 100.0) : 1.0;
}

// The wetness grid at a world xz: float4(ground wetness, standing water, precipitation, the share of it that falls as
// rain), bilinear between the cells' centres (beyond the grid its edge cells').
float4 quenchWetnessAt(Texture2D<float4> maps, SamplerState s, float2 xz) {
    const float n = float(QUENCH_WETNESS_GRID);
    const float2 cell = clamp((xz - quenchFrame.rainGrid.xy) * quenchFrame.rainGrid.z * n, 0.5, n - 0.5);
    const float2 uv = float2(cell.x / float(QUENCH_WEATHER_MAPS_WIDTH),
                             (float(QUENCH_WETNESS_ROW) + cell.y) / float(QUENCH_WEATHER_MAPS_HEIGHT));
    return maps.SampleLevel(s, uv, 0.0);
}

// The snow cover at a world xz (the weather's Wetness::snow: 0 bare .. 1 covered), from the cells beside the wetness
// grid's.
float quenchSnowAt(Texture2D<float4> maps, SamplerState s, float2 xz) {
    const float n = float(QUENCH_WETNESS_GRID);
    const float2 cell = clamp((xz - quenchFrame.rainGrid.xy) * quenchFrame.rainGrid.z * n, 0.5, n - 0.5);
    const float2 uv = float2((n + cell.x) / float(QUENCH_WEATHER_MAPS_WIDTH),
                             (float(QUENCH_WETNESS_ROW) + cell.y) / float(QUENCH_WEATHER_MAPS_HEIGHT));
    return maps.SampleLevel(s, uv, 0.0).r;
}

// How much of a surface at P (its geometric normal N, `distance` m from the camera) a snow cover of `cover` (0..1)
// hides: none on walls and overhangs, less on slopes, and a thin cover as a dusting with patches (value noise at
// 0.8 m and 0.2 m, the finer one faded out with the distance) that close as it deepens; from 20 m to 100 m away the
// patches, smaller than a pixel there, give way to the share of the ground they cover.
float quenchSnowAmount(float3 P, float3 N, float cover, float distance) {
    const float depth = cover * smoothstep(0.3, 0.8, N.y) * 1.2;
    if (depth <= 0.0) return 0.0;
    const float fine = lerp(valueNoise(P.xz / 0.2 + P.y * 0.37 + 5.1), 0.5, saturate(distance / 40.0));
    const float n = valueNoise(P.xz / 0.8 + P.y * 0.53) * 0.65 + fine * 0.35;
    const float patches = lerp(smoothstep(n - 0.06, n + 0.06, depth), smoothstep(0.15, 0.85, depth),
                               saturate(distance / 80.0 - 0.25));
    return max(patches, saturate(depth) * 0.3);
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

// How open a point is to the rain: 1 in the open (also outside the occlusion map), 0 under cover and at and above
// the cloud base (quenchFallReach). Surfaces look up a point a little off them along their normal, so that walls see
// the open ground beside them and floors their roof.
float quenchRainExposure(Texture2D<float4> maps, SamplerState s, float3 P) {
    const float reach = quenchFallReach(P.y);
    const float3 m = quenchRainMapPos(P);
    if (any(m.xy < 0.0) || any(m.xy > 1.0) || m.z < 0.0) return reach;
    const float top = quenchRainOccluder(maps, s, m.xy);
    return reach * saturate(1.0 - (m.z - top) * quenchFrame.rain.y / QUENCH_RAIN_COVER_DEPTH);
}

// The relief whose hollows puddles fill (MaterialDesc::puddles): 0 deepest .. 1 highest, value noise at 6 m and
// 1.5 m. Standing water w fills it up to 0.42 w: about a third of a flat surface under full puddles.
float quenchPuddleHeight(float2 xz) { return valueNoise(xz / 6.0) * 0.7 + valueNoise(xz / 1.5 + 13.1) * 0.3; }

// The rings of rain drops on standing water: the slope of its surface (d height / dx, d height / dz) at a world xz,
// at time t (s) for precipitation 0..1. A drop leaves a short packet of capillary waves, not a target of wide rings:
// two or three crests a couple of centimetres apart (the short ones lead, the longer follow) that run out at about
// 0.3 m/s, weaken as their circle grows and are gone in half a second, some 10 to 25 cm from where the drop fell;
// the water inside the packet is calm again. Two grids of cells QUENCH_RIPPLE_CELL m across, offset; a drop falls in
// a cell every 0.9 s (light rain) to 0.5 s (heavy) at a place of its own, in a share of the cells that grows with the
// rain. Each drop has a size of its own (its waves' length, speed and strength), and the drops fall more densely
// and strike harder in some puddles than in others (value noise at 3 m, as gusts and drips do). The waves are finer
// than a pixel from a few metres on: quenchRippleReach says how much of them a surface that far away shows, and
// quenchRippleRoughness what the rest does to its gloss.
static const float QUENCH_RIPPLE_CELL = 0.22;
static const float QUENCH_RIPPLE_LIFE = 0.55;

float2 quenchRipples(float2 xz, float t, float rain) {
    const float place = valueNoise(xz / 3.0 + 41.7);
    const float density = rain * lerp(0.6, 1.5, place);
    const float period = lerp(0.9, 0.5, saturate(rain));
    float2 slope = float2(0.0, 0.0);
    [unroll] for (int layer = 0; layer < 2; ++layer) {
        const float2 p = xz / QUENCH_RIPPLE_CELL + float(layer) * float2(0.37, 0.71);
        const float2 base = floor(p);
        [unroll] for (int j = -1; j <= 1; ++j) {
            [unroll] for (int k = -1; k <= 1; ++k) {
                const float2 id = base + float2(float(k), float(j));
                const float phase = t / period + hash12(id + float(layer) * 17.0);
                const float drop = floor(phase);
                if (hash12(id * 1.37 + drop * 3.1 + float(layer) * 7.0) > density) continue;
                const float age = (phase - drop) * period / QUENCH_RIPPLE_LIFE;
                if (age >= 1.0) continue;
                const float2 own = hash22(id * 0.73 + drop * 5.7 + float(layer) * 3.0);
                const float size = lerp(0.65, 1.35, own.x);
                // Metres from where the drop fell (within the middle of its cell: its rings stay inside the
                // neighbours looked at).
                const float2 d = (p - (id + 0.2 + 0.6 * hash22(id * 2.11 + drop + 5.0))) * QUENCH_RIPPLE_CELL;
                const float r = length(d);
                const float front = (0.02 + age * QUENCH_RIPPLE_LIFE * 0.3) * size;
                const float wavelength = 0.02 * size * lerp(0.8, 1.2, own.y);
                // Wavelengths behind the packet's front.
                const float x = (front - r) / wavelength;
                if (x < -0.5 || x > 6.0) continue;
                const float behind = max(x, 0.0);
                const float packet = smoothstep(-0.5, 0.25, x) * exp(-0.8 * behind);
                const float wave = sin(6.2832 * x / (1.0 + 0.12 * behind));
                const float fade = (1.0 - age) * (1.0 - age) * smoothstep(0.0, 0.06, age);
                slope -= d / max(r, 1e-4) * (wave * packet * fade * size * sqrt(0.03 / (r + 0.03)));
            }
        }
    }
    return slope * (0.5 * lerp(0.7, 1.3, place));
}

// How much of the rings a surface `distance` m from the camera shows (1 near .. 0 from about 14 m at 1080 lines:
// their crests are closer together than its pixels there, and would flicker).
float quenchRippleReach(float distance) {
    return 1.0 - smoothstep(5.0, 14.0, distance * 1080.0 * quenchFrame.screen.w);
}

// The roughness of standing water whose rings are too fine to draw: the rain's dimples blur what it mirrors.
float quenchRippleRoughness(float rain) { return 0.05 + 0.1 * saturate(rain); }

#endif
