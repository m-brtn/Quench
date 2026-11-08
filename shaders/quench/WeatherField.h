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
// is the rain or snow that falls from the cloud, cirrus the high ice cloud above. Whether it falls as rain or snow is
// quenchWeatherSnow's: by the temperature at a point, or a cell's own share.

#include "quench/Shared.h"

#define QUENCH_MAX_CLOUD_CELLS 16
// Precipitation falls as snow at or below this temperature (degrees C), as rain from QUENCH_RAIN_ABOVE, as sleet (a
// mix of both) between.
#define QUENCH_SNOW_BELOW 0.0f
#define QUENCH_RAIN_ABOVE 2.5f

#ifdef __cplusplus
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace quench::shader {
using float2 = glm::vec2;
using float3 = glm::vec3;
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
inline float3 lerp(float3 a, float3 b, float t) { return a + (b - a) * t; }
inline float smoothstep(float e0, float e1, float v) {
    const float t = saturate((v - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}
} // namespace quench::shader

#define QUENCH_WEATHER_FN inline
#define QUENCH_WEATHER_UNIFORMS const WeatherUniforms&
#define QUENCH_WIND_UNIFORMS const WindUniforms&
#else
#define QUENCH_WEATHER_FN
#define QUENCH_WEATHER_UNIFORMS WeatherUniforms
#define QUENCH_WIND_UNIFORMS WindUniforms
#endif

QUENCH_SHARED_BEGIN

struct QUENCH_ALIGN16 WeatherUniforms {
    float4 field; // x: coverage, y: cloud type, z: type variation, w: precipitation (0..1 each)
    float4 shape; // x: 1 / feature size (1/m), y: evolution time (field units), z: cirrus (0..1), w: seed (24 bits)
    float4 air;   // xy: how far the air has moved (world xz, m), zw: unit wind direction (world xz)
    float4 layer; // x: cloud base (m), y: top of the tallest clouds (m), z: cirrus height (m), w: number of cells
    // Two per cell: xy its centre now (world xz, m), z radius (m), w coverage; x type, y precipitation, z its phase
    // in the evolution (a different shape per cell), w the share of its precipitation that falls as snow (below 0:
    // by the temperature).
    float4 cells[QUENCH_MAX_CLOUD_CELLS * 2];
    // x: the temperature at y = 0 (degrees C), y: how much it falls per metre of height (degrees C / m), z: 1 when a
    // cell has a snow share of its own, w: unused.
    float4 climate;
};

// A 32-bit integer hash (lowbias32) of integer lattice coordinates and a seed.
QUENCH_WEATHER_FN uint quenchWeatherHashBits(int x, int y, int z, uint seed) {
    uint h = uint(x) * 0x8da6b343u;
    h ^= uint(y) * 0xd8163841u;
    h ^= uint(z) * 0xcb1ab31fu;
    h ^= seed * 0x9e3779b9u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// 0..1 from integer lattice coordinates and a seed: the hash's top 24 bits as a float.
QUENCH_WEATHER_FN float quenchWeatherHash(int x, int y, int z, uint seed) {
    return float(quenchWeatherHashBits(x, y, z, seed) >> 8) * (1.0f / 16777216.0f);
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

// The share of the precipitation at a world point (xz, m; `height` m above y = 0) that falls as snow, 0 rain .. 1
// snow: by the temperature there (falling with the height, so hills and the air aloft are colder), and within a cell
// that has a share of its own, that share (round, fading out toward its radius like its cloud).
QUENCH_WEATHER_FN float quenchWeatherSnow(QUENCH_WEATHER_UNIFORMS u, float2 xz, float height) {
    const float t = u.climate.x - u.climate.y * max(height, 0.0f);
    float snow = saturate((QUENCH_RAIN_ABOVE - t) / (QUENCH_RAIN_ABOVE - QUENCH_SNOW_BELOW));
    if (u.climate.z > 0.0f) {
        const int count = int(u.layer.w);
        for (int i = 0; i < count; ++i) {
            const float4 a = u.cells[i * 2];
            const float own = u.cells[i * 2 + 1].w;
            if (own < 0.0f) continue;
            const float dx = (xz.x - a.x) / a.z;
            const float dz = (xz.y - a.y) / a.z;
            snow = lerp(snow, own, 1.0f - smoothstep(0.35f, 1.0f, sqrt(dx * dx + dz * dz)));
        }
    }
    return snow;
}

// ---------------------------------------------------------------- the wind

// The wind field (docs/weather.md, "Wind"), the same for a game's queries (weather::Weather::windAt), the rain's and
// the snow's particles and the plants (quench/Wind.hlsli): the settings' wind at the cloud base, slower near the
// ground, with gusts and eddies that travel with the air. Its time runs modulo QUENCH_WIND_PERIOD seconds, over which
// its noise repeats, so that 32-bit floats keep it as exact after hours as at the start.
#define QUENCH_WIND_PERIOD 4096.0f

struct QUENCH_ALIGN16 WindUniforms {
    float4 air;   // xy: how far the air has moved (world xz, m), zw: unit wind direction (world xz)
    float4 speed; // x: the wind at the cloud base (m/s), y: the cloud base (m), z: gusts (0..1), w: turbulence (0..1)
    float4 time;  // x: the weather's time modulo QUENCH_WIND_PERIOD (s), y: the seed (24 bits), zw: unused
};

// The wind at a point in three parts: the mean wind, what the gusts and eddies add to it now (the wind is mean +
// gust), and how far they have moved the air from where the mean wind alone would have taken it (m; gust is its rate
// of change): what a particle carried by the wind adds to its mean motion.
struct QuenchWind {
    float3 mean;
    float3 gust;
    float3 offset;
};

// Value noise on a unit lattice in x, y and t, periodic in t over `period` lattice steps (a power of two), with its
// rate of change in t: float2(value 0..1, d value / d t).
QUENCH_WEATHER_FN float2 quenchWindNoise(float x, float y, float t, uint seed, int period) {
    const float fx = floor(x);
    const float fy = floor(y);
    const float ft = floor(t);
    const int ix = int(fx);
    const int iy = int(fy);
    const int t0 = int(ft) & (period - 1);
    const int t1 = (int(ft) + 1) & (period - 1);
    float tx = x - fx;
    float ty = y - fy;
    const float f = t - ft;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    const float a =
        lerp(lerp(quenchWeatherHash(ix, iy, t0, seed), quenchWeatherHash(ix + 1, iy, t0, seed), tx),
             lerp(quenchWeatherHash(ix, iy + 1, t0, seed), quenchWeatherHash(ix + 1, iy + 1, t0, seed), tx), ty);
    const float b =
        lerp(lerp(quenchWeatherHash(ix, iy, t1, seed), quenchWeatherHash(ix + 1, iy, t1, seed), tx),
             lerp(quenchWeatherHash(ix, iy + 1, t1, seed), quenchWeatherHash(ix + 1, iy + 1, t1, seed), tx), ty);
    return float2(lerp(a, b, f * f * (3.0f - 2.0f * f)), (b - a) * 6.0f * f * (1.0f - f));
}

// Two of them half a lattice step apart in space and time, so that their rate of change never vanishes everywhere at
// once (one noise's does at each of its time steps): float2(value 0..2, its rate of change).
QUENCH_WEATHER_FN float2 quenchWindNoise2(float x, float y, float t, uint seed, int period) {
    return quenchWindNoise(x, y, t, seed, period) + quenchWindNoise(x + 0.5f, y + 0.5f, t + 0.5f, seed + 1u, period);
}

// Three independent value noises like quenchWindNoise in one pass over the lattice, each corner's hash split into three
// 10-bit values (a third of the hashing): the values 0..1 and their rates of change in t.
struct QuenchWindNoise3 {
    float3 value;
    float3 rate;
};

QUENCH_WEATHER_FN float3 quenchWindCorner(int x, int y, int t, uint seed) {
    const uint h = quenchWeatherHashBits(x, y, t, seed);
    return float3(float(h >> 22), float((h >> 12) & 1023u), float((h >> 2) & 1023u)) * (1.0f / 1023.0f);
}

QUENCH_WEATHER_FN QuenchWindNoise3 quenchWindNoise3(float x, float y, float t, uint seed, int period) {
    const float fx = floor(x);
    const float fy = floor(y);
    const float ft = floor(t);
    const int ix = int(fx);
    const int iy = int(fy);
    const int t0 = int(ft) & (period - 1);
    const int t1 = (int(ft) + 1) & (period - 1);
    float tx = x - fx;
    float ty = y - fy;
    const float f = t - ft;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    const float3 a =
        lerp(lerp(quenchWindCorner(ix, iy, t0, seed), quenchWindCorner(ix + 1, iy, t0, seed), tx),
             lerp(quenchWindCorner(ix, iy + 1, t0, seed), quenchWindCorner(ix + 1, iy + 1, t0, seed), tx), ty);
    const float3 b =
        lerp(lerp(quenchWindCorner(ix, iy, t1, seed), quenchWindCorner(ix + 1, iy, t1, seed), tx),
             lerp(quenchWindCorner(ix, iy + 1, t1, seed), quenchWindCorner(ix + 1, iy + 1, t1, seed), tx), ty);
    QuenchWindNoise3 o;
    o.value = lerp(a, b, f * f * (3.0f - 2.0f * f));
    o.rate = (b - a) * (6.0f * f * (1.0f - f));
    return o;
}

// The share of the wind at the cloud base `base` (m) that blows `height` m above y = 0: a logarithmic profile with a
// roughness length of 0.1 m (open ground, grass), about 0.3 at 2 m under a base at 1.5 km.
QUENCH_WEATHER_FN float quenchWindProfile(float height, float base) {
    return saturate(log(max(height, 0.101f) / 0.1f) / log(max(base, 10.0f) / 0.1f));
}

// The wind at a world point (m). Gusts are bands across the wind, about 40 m along it and 120 m across, that change
// over about 8 s; eddies are about 10 m across, change over about 2 s and turn the wind in every direction (upward
// less). Both travel with the air at 0.6 of the cloud base's wind, as gusts of tens of metres do near the ground, and
// scale with the mean wind at the point. At gusts 1 the speed along the wind deviates by about 0.45 of the mean, at
// turbulence 1 along and across by about 0.3, upward by about 0.12 (weather_wind_gusts measures them).
QUENCH_WEATHER_FN QuenchWind quenchWeatherWind(QUENCH_WIND_UNIFORMS w, float3 p) {
    const float2 dir = float2(w.air.z, w.air.w);
    const float speed = w.speed.x * quenchWindProfile(p.y, w.speed.y);
    QuenchWind o;
    o.mean = float3(dir.x * speed, 0.0f, dir.y * speed);
    o.gust = float3(0.0f, 0.0f, 0.0f);
    o.offset = float3(0.0f, 0.0f, 0.0f);
    if (speed <= 0.0f || w.speed.z + w.speed.w <= 0.0f) return o;
    const float ax = p.x - w.air.x * 0.6f;
    const float az = p.z - w.air.y * 0.6f;
    const float along = ax * dir.x + az * dir.y;
    const float across = az * dir.x - ax * dir.y;
    const float t = w.time.x;
    const uint seed = uint(w.time.y);
    // The gusts' reach: a displacement whose rate of change has the deviation the setting asks for (the summed
    // noises' rate of change deviates by about 0.45 per lattice step).
    const float2 g = quenchWindNoise2(along / 40.0f, across / 120.0f, t / 8.0f, seed + 11u, 512);
    const float gustReach = w.speed.z * speed * 8.0f;
    o.offset = float3(dir.x, 0.0f, dir.y) * (gustReach * (g.x - 1.0f));
    o.gust = float3(dir.x, 0.0f, dir.y) * (gustReach * g.y / 8.0f);
    // The eddies: x, up and z from one pair of three-channel noises, half a lattice step apart like quenchWindNoise2.
    const float eddyReach = w.speed.w * speed * 2.0f * 0.625f;
    const QuenchWindNoise3 e0 = quenchWindNoise3(ax / 10.0f, az / 10.0f, t / 2.0f, seed + 21u, 2048);
    const QuenchWindNoise3 e1 =
        quenchWindNoise3(ax / 10.0f + 0.5f, az / 10.0f + 0.5f, t / 2.0f + 0.5f, seed + 22u, 2048);
    const float3 scale = float3(1.0f, 0.4f, 1.0f);
    o.offset = o.offset + (e0.value + e1.value - 1.0f) * scale * eddyReach;
    o.gust = o.gust + (e0.rate + e1.rate) * scale * (eddyReach / 2.0f);
    return o;
}

QUENCH_SHARED_END

#endif
