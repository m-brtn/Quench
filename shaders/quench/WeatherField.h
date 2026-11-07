#ifndef QUENCH_WEATHER_FIELD_H
#define QUENCH_WEATHER_FIELD_H

// The weather map's field, read by C++ (quench_weather: weather::Weather::sample and the queries built on it) and by
// HLSL (the render module's weather pass, which draws it into the texture the clouds read): one source, so what a
// game asks on the CPU and what the clouds show on the screen agree (docs/weather.md, "The weather map").
//
// The field is a pure function of WeatherUniforms and a world position: integer hashes (exact on both sides), value
// noise and a few smoothsteps, all in 32-bit float. CPU and GPU results differ only by rounding (about 1e-6).
//
// A sample is float4(coverage, type, precipitation, cirrus), each 0..1: coverage is the share of the column that holds
// cloud, type runs from stratus (0) over cumulus (1/3) and towering cumulus (2/3) to cumulonimbus (1), precipitation
// is the rain or snow that falls from the cloud, cirrus the high ice cloud above.

#include "quench/Shared.h"

#define QUENCH_MAX_CLOUD_CELLS 16

#ifdef __cplusplus
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace quench::shader {
using float2 = glm::vec2;
using uint = std::uint32_t;
// The HLSL intrinsics the field uses, for float only (no std:: overload sets, no double promotions).
inline float floor(float v) { return std::floor(v); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float log(float v) { return std::log(v); }
inline float exp(float v) { return std::exp(v); }
inline float max(float a, float b) { return a > b ? a : b; }
inline float clamp(float v, float lo, float hi) { return std::clamp(v, lo, hi); }
inline float saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothstep(float e0, float e1, float v) {
    const float t = saturate((v - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}
} // namespace quench::shader

#define QUENCH_WEATHER_FN inline
#define QUENCH_WEATHER_UNIFORMS const WeatherUniforms&
#else
#define QUENCH_WEATHER_FN
#define QUENCH_WEATHER_UNIFORMS WeatherUniforms
#endif

QUENCH_SHARED_BEGIN

struct QUENCH_ALIGN16 WeatherUniforms {
    float4 field; // x: coverage, y: cloud type, z: type variation, w: precipitation (0..1 each)
    float4 shape; // x: 1 / feature size (1/m), y: evolution time (field units), z: cirrus (0..1), w: seed (24 bits)
    float4 air;   // xy: how far the air has moved (world xz, m), zw: unit wind direction (world xz)
    float4 layer; // x: cloud base (m), y: top of the tallest clouds (m), z: cirrus height (m), w: number of cells
    // Two per cell: xy its centre now (world xz, m), z radius (m), w coverage; x type, y precipitation, z its phase
    // in the evolution (a different shape per cell), w unused.
    float4 cells[QUENCH_MAX_CLOUD_CELLS * 2];
};

// 0..1 from integer lattice coordinates and a seed: a 32-bit integer hash (lowbias32), the top 24 bits as a float.
QUENCH_WEATHER_FN float quenchWeatherHash(int x, int y, int z, uint seed) {
    uint h = uint(x) * 0x8da6b343u;
    h ^= uint(y) * 0xd8163841u;
    h ^= uint(z) * 0xcb1ab31fu;
    h ^= seed * 0x9e3779b9u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return float(h >> 8) * (1.0f / 16777216.0f);
}

// Smooth value noise, 0..1, on a unit lattice in x, y and z.
QUENCH_WEATHER_FN float quenchWeatherNoise(float x, float y, float z, uint seed) {
    const float fx = floor(x);
    const float fy = floor(y);
    const float fz = floor(z);
    const int ix = int(fx);
    const int iy = int(fy);
    const int iz = int(fz);
    float tx = x - fx;
    float ty = y - fy;
    float tz = z - fz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    tz = tz * tz * (3.0f - 2.0f * tz);
    const float x00 = lerp(quenchWeatherHash(ix, iy, iz, seed), quenchWeatherHash(ix + 1, iy, iz, seed), tx);
    const float x10 = lerp(quenchWeatherHash(ix, iy + 1, iz, seed), quenchWeatherHash(ix + 1, iy + 1, iz, seed), tx);
    const float x01 = lerp(quenchWeatherHash(ix, iy, iz + 1, seed), quenchWeatherHash(ix + 1, iy, iz + 1, seed), tx);
    const float x11 =
        lerp(quenchWeatherHash(ix, iy + 1, iz + 1, seed), quenchWeatherHash(ix + 1, iy + 1, iz + 1, seed), tx);
    return lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz);
}

// Four octaves of it, each turned by 37 degrees and changing faster in z (small features change faster), 0..1.
QUENCH_WEATHER_FN float quenchWeatherFbm(float x, float y, float z, uint seed) {
    float sum = 0.0f;
    float amplitude = 0.5f;
    float norm = 0.0f;
    for (int i = 0; i < 4; ++i) {
        sum += amplitude * quenchWeatherNoise(x, y, z, seed + uint(i) * 101u);
        norm += amplitude;
        const float rx = 0.8f * x - 0.6f * y;
        const float ry = 0.6f * x + 0.8f * y;
        x = rx * 2.03f + 17.1f;
        y = ry * 2.03f - 9.3f;
        z = z * 1.7f + 3.7f;
        amplitude *= 0.5f;
    }
    return sum / norm;
}

// The field at a world position (xz, m): float4(coverage, type, precipitation, cirrus).
QUENCH_WEATHER_FN float4 quenchWeatherSample(QUENCH_WEATHER_UNIFORMS u, float2 xz) {
    const uint seed = uint(u.shape.w);
    const float ax = (xz.x - u.air.x) * u.shape.x; // the air's own coordinates: the field drifts with the wind
    const float az = (xz.y - u.air.y) * u.shape.x;
    const float t = u.shape.y;
    const float amount = u.field.x;

    // Coverage: the amount's logit moved by the fbm (about normal, mean 0.5, deviation 0.134, here scaled to a
    // deviation of 1.2): groups of clouds and clear gaps. The logit is widened by sqrt(1 + pi 1.2^2 / 8) so that the
    // mean of the logistic-normal coverage stays near the amount; from 0.85 on it closes into overcast. The
    // renderer's noise makes the single clouds of a group, which cover about this share of the sky there.
    const float n = quenchWeatherFbm(ax, az, t, seed);
    const float p = clamp(amount, 0.001f, 0.999f);
    const float logit = log(p / (1.0f - p)) * 1.2512f + (n - 0.5f) * (1.2f / 0.134f);
    float coverage = saturate(amount * 50.0f) / (1.0f + exp(-logit));
    coverage = lerp(coverage, 1.0f, saturate((amount - 0.85f) * 6.67f));

    // Type: the chosen one, varied over larger patches; the thickest parts of a field grow taller.
    const float m = quenchWeatherFbm(ax * 0.5f + 31.7f, az * 0.5f - 11.3f, t * 0.7f, seed + 7919u);
    float type = saturate(u.field.y + u.field.z * ((m - 0.5f) * 2.5f + 0.4f * (coverage - 0.5f)));

    // Precipitation: from the thick parts, more from taller clouds.
    float rain = u.field.w * smoothstep(0.55f, 0.95f, coverage) * (0.5f + 0.5f * type);

    // Cirrus: patches stretched along the wind.
    const float along = ax * u.air.z + az * u.air.w;
    const float across = az * u.air.z - ax * u.air.w;
    const float c = quenchWeatherFbm(along * 0.15f, across * 0.6f, t * 0.5f, seed + 104729u);
    const float cirrus = u.shape.z * saturate(0.5f + (c - 0.5f) * 3.0f + (u.shape.z - 0.5f));

    // Cells: round clouds with lobes that change with time, over the field; their precipitation falls from the core.
    const int count = int(u.layer.w);
    for (int i = 0; i < count; ++i) {
        const float4 a = u.cells[i * 2];
        const float4 b = u.cells[i * 2 + 1];
        const float dx = (xz.x - a.x) / a.z;
        const float dz = (xz.y - a.y) / a.z;
        float d = sqrt(dx * dx + dz * dz);
        d *= 0.8f + 0.4f * quenchWeatherNoise(dx * 1.5f, dz * 1.5f, t * 2.0f + b.z, seed + 31u);
        const float f = 1.0f - smoothstep(0.35f, 1.0f, d);
        coverage = max(coverage, f * a.w);
        type = lerp(type, b.x, f);
        rain = max(rain, b.y * (1.0f - smoothstep(0.1f, 0.55f, d)));
    }
    return float4(coverage, type, rain, cirrus);
}

QUENCH_SHARED_END

#endif
