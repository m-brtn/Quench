// The clouds' noise, made by the GPU when clouds first appear (src/render/Clouds.cpp): one fullscreen draw per slice
// of a 3D texture, every texel a tiling function of its position, so the textures wrap without seams.
//
//   kind 0, shape (128^3, Rgba8Unorm): r the clouds' large shapes: Perlin-Worley (Perlin fbm reshaped by Worley fbm)
//           eroded by Worley fbm at 4, 8 and 16 cells per side (Schneider, "The real-time volumetric cloudscapes of
//           Horizon Zero Dawn", SIGGRAPH 2015; "Nubis", 2017), spread to about an even distribution over 0..1, so that
//           the share of it above 1 - c is about c (the coverage of the weather map); g, b, a those Worley fbms.
//   kind 1, detail (32^3): r, g, b Worley fbm at 2, 4 and 8 cells per side: the eroded edges.
//   kind 2, layer (256^2, 2D): r Perlin fbm, g Worley fbm, b fine Perlin fbm, a fine Worley: the 2D cloud layer and
//           the cirrus.
//
// Bindings: fragment uniform slot 0 holds CloudUniforms (only `noise`: x slice, y slices, z kind).

#include "quench/Bindings.hlsli"
#include "quench/Fullscreen.hlsli"

#include "CloudUniforms.h"

cbuffer CloudNoiseBlock : QUENCH_UNIFORM(0) {
    CloudUniforms cloudNoise;
};

uint quenchNoiseHash(int3 c, uint seed) {
    uint h = uint(c.x) * 0x8da6b343u;
    h ^= uint(c.y) * 0xd8163841u;
    h ^= uint(c.z) * 0xcb1ab31fu;
    h ^= seed * 0x9e3779b9u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

float3 quenchNoiseHash3(int3 c, uint seed) {
    const uint a = quenchNoiseHash(c, seed);
    const uint b = quenchNoiseHash(c, seed + 1u);
    const uint d = quenchNoiseHash(c, seed + 2u);
    return float3(float(a >> 8), float(b >> 8), float(d >> 8)) * (1.0 / 16777216.0);
}

int3 wrapCell(int3 c, int period) { return ((c % period) + period) % period; }

// Gradient noise with a period of `period` lattice cells, about -1..1.
float tilingPerlin(float3 p, int period, uint seed) {
    const float3 i = floor(p);
    const float3 f = p - i;
    const float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float corners[8];
    for (int k = 0; k < 8; ++k) {
        const int3 o = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        const float3 g = normalize(quenchNoiseHash3(wrapCell(int3(i) + o, period), seed) * 2.0 - 1.0 + 1e-4);
        corners[k] = dot(g, f - float3(o));
    }
    const float x0 = lerp(corners[0], corners[1], u.x);
    const float x1 = lerp(corners[2], corners[3], u.x);
    const float x2 = lerp(corners[4], corners[5], u.x);
    const float x3 = lerp(corners[6], corners[7], u.x);
    return lerp(lerp(x0, x1, u.y), lerp(x2, x3, u.y), u.z) * 1.6;
}

// 1 - the distance to the nearest of one random point per cell, `period` cells per side: 1 at the points, 0 between.
float tilingWorley(float3 p, int period, uint seed) {
    const float3 i = floor(p);
    const float3 f = p - i;
    float best = 1e9;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                const int3 o = int3(x, y, z);
                const float3 r = float3(o) + quenchNoiseHash3(wrapCell(int3(i) + o, period), seed) - f;
                best = min(best, dot(r, r));
            }
    return 1.0 - saturate(sqrt(best));
}

// Three octaves of Worley noise from `cells` per side of the tile (uvw in 0..1).
float worleyFbm(float3 uvw, int cells, uint seed) {
    return tilingWorley(uvw * float(cells), cells, seed) * 0.625 +
           tilingWorley(uvw * float(cells * 2), cells * 2, seed + 11u) * 0.25 +
           tilingWorley(uvw * float(cells * 4), cells * 4, seed + 23u) * 0.125;
}

float perlinFbm(float3 uvw, int cells, int octaves, uint seed) {
    float sum = 0.0;
    float amplitude = 1.0;
    float norm = 0.0;
    int period = cells;
    for (int i = 0; i < octaves; ++i) {
        sum += tilingPerlin(uvw * float(period), period, seed + uint(i) * 7u) * amplitude;
        norm += amplitude;
        amplitude *= 0.5;
        period *= 2;
    }
    return sum / norm;
}

float remapClamped(float v, float lo, float hi, float newLo, float newHi) {
    return saturate(newLo + (v - lo) / max(hi - lo, 1e-5) * (newHi - newLo));
}

#if defined(QUENCH_ENTRY_cloud_noise_fs)
float4 cloud_noise_fs(FullscreenOut vin) : SV_Target0 {
    const float slices = max(cloudNoise.noise.y, 1.0);
    const float3 uvw = float3(vin.uv, (cloudNoise.noise.x + 0.5) / slices);
    const int kind = int(cloudNoise.noise.z);
    if (kind == 0) {
        const float perlin = perlinFbm(uvw, 4, 4, 1u) * 0.5 + 0.5;
        const float worley = worleyFbm(uvw, 4, 2u);
        const float worley8 = worleyFbm(uvw, 8, 3u);
        const float worley16 = worleyFbm(uvw, 16, 4u);
        const float perlinWorley = remapClamped(perlin, worley - 1.0, 1.0, 0.0, 1.0);
        const float fbm = worley * 0.625 + worley8 * 0.25 + worley16 * 0.125;
        const float shape = remapClamped(perlinWorley, fbm - 1.0, 1.0, 0.0, 1.0);
        // About normal with mean 0.780 and deviation 0.051 (measured over the tile): its logistic CDF.
        const float even = 1.0 / (1.0 + exp(-(shape - 0.780) * 33.4));
        return float4(even, worley, worley8, worley16);
    }
    if (kind == 1) return float4(worleyFbm(uvw, 2, 5u), worleyFbm(uvw, 4, 6u), worleyFbm(uvw, 8, 7u), 1.0);
    const float3 p = float3(vin.uv, 0.0);
    return float4(perlinFbm(p, 4, 5, 8u) * 0.5 + 0.5, worleyFbm(p, 6, 9u), perlinFbm(p, 16, 3, 10u) * 0.5 + 0.5,
                  worleyFbm(p, 24, 11u));
}
#endif
