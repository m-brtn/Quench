#ifndef QUENCH_PARTICLE_FIELD_H
#define QUENCH_PARTICLE_FIELD_H

// How particles move and look (docs/particles.md), read by C++ (quench_particles: a spawn's first part of a step and
// the CPU simulation, particles::Simulation) and by HLSL (the render module's compute pass that steps the particles
// on the GPU, src/render/shaders/particles.hlsl): one source, so both paths move and color a particle the same way,
// up to rounding.
//
// A particle is three float4 (QuenchParticle), stored in a ring of slots per system; each frame every live particle
// is stepped and written as three float4 for the draw (QuenchParticleInstance). The motion is a pure function of the
// state, ParticleUniforms and the step: gravity and drag toward the air's velocity, integrated exactly for a constant
// air, where the air is a curl noise (divergence-free: it swirls without gathering particles into clumps) plus a share
// of the weather's wind, taken in substeps of at most QUENCH_PARTICLE_SUBSTEP.

#include "quench/Shared.h"
#include "quench/WeatherField.h"

// Samples per curve over a particle's life (size and color).
#define QUENCH_PARTICLE_CURVE 16
// The particles' clock (ParticleUniforms::clock.y) wraps after this many seconds.
#define QUENCH_PARTICLE_PERIOD 4096.0f
// The longest substep (s), and the most substeps in one step: longer steps (a hitch) are taken in that many.
#define QUENCH_PARTICLE_SUBSTEP (1.0f / 60.0f)
#define QUENCH_PARTICLE_MAX_SUBSTEPS 8
// Threads per group of the compute pass.
#define QUENCH_PARTICLE_GROUP 64

#ifdef __cplusplus
#include <glm/geometric.hpp>

#include <cmath>

namespace quench::shader {
// The HLSL intrinsics the particles use beyond the weather field's, for float only.
inline float sin(float v) { return std::sin(v); }
inline float cos(float v) { return std::cos(v); }
inline float pow(float a, float b) { return std::pow(a, b); }
inline float ceil(float v) { return std::ceil(v); }
inline float min(float a, float b) { return a < b ? a : b; }
inline float frac(float v) { return v - std::floor(v); }
inline float4 lerp(float4 a, float4 b, float t) { return a + (b - a) * t; }
} // namespace quench::shader

#define QUENCH_PARTICLE_FN inline
#define QUENCH_PARTICLE_UNIFORMS const ParticleUniforms&
#else
#define QUENCH_PARTICLE_FN
#define QUENCH_PARTICLE_UNIFORMS ParticleUniforms
#endif

QUENCH_SHARED_BEGIN

// One system's step (uniform slot 0 of the compute pass; the CPU simulation reads the same).
struct QUENCH_ALIGN16 ParticleUniforms {
    // x: the system's first slot in the pool, y: its slots (the ring's size), z: the ring slot (0..y-1) of this
    // step's first spawn, w: this step's spawns (at most y; slot z + i mod y takes spawn i).
    float4 range;
    // x: the step (s), y: the clock at its end (s, modulo QUENCH_PARTICLE_PERIOD), z: the first spawn's record in the
    // spawn buffer, w: 1 to forget the slots' particles (a new system, or a restart).
    float4 clock;
    float4 gravity; // xyz: acceleration (m/s^2), w: drag (1/s): how fast a particle takes the air's velocity
    // x: the noise's speed (m/s, its RMS along each axis), y: 1 / its feature size (1/m), z: how fast it changes
    // (1/s), w: the share of the weather's wind in the air (0 without a weather)
    float4 noise;
    // x: twinkle 0..1 (how much a particle's brightness flickers), y: its rate (Hz), z: its sharpness (1: a sine, 16:
    // short flashes), w: brightness variation 0..1 between particles
    float4 look;
    float4 color;                             // rgb: linear HDR, a: opacity
    float4 size[QUENCH_PARTICLE_CURVE / 4];   // the size over the life (times the particle's own), 16 samples
    float4 colorCurve[QUENCH_PARTICLE_CURVE]; // rgba over the life (times color)
    WindUniforms wind;                        // the weather's wind (speed.x 0: none)
};

// A particle in its slot.
struct QUENCH_ALIGN16 QuenchParticle {
    float4 a; // xyz: position (m), w: age (s)
    float4 b; // xyz: velocity (m/s), w: lifetime (s); 0: no particle in the slot
    float4 c; // x: its random 0..1 (its twinkle and brightness), y: its size (m, half the quad's side), z: rotation
              // at spawn (rad), w: spin (rad/s)
};

// What the draw reads of a particle: half size 0 is nothing drawn.
struct QUENCH_ALIGN16 QuenchParticleInstance {
    float4 a; // xyz: position (m), w: half size (m)
    float4 b; // rgb: color (linear HDR, with its twinkle), a: opacity
    float4 c; // xyz: velocity (m/s), w: rotation (rad)
};

// A curl noise of three sine products (a vector potential psi = (sin y sin z, sin z sin x, sin x sin y) with phases
// that drift with t), at two scales, the second turned and twice as fine: its curl is divergence-free, so it swirls
// without sources or sinks. Its RMS per axis is about 1.
QUENCH_PARTICLE_FN float3 quenchCurlOctave(float3 p, float t) {
    const float a1 = t * 0.31f, b1 = 1.7f - t * 0.23f;
    const float a2 = 3.1f + t * 0.27f, b2 = 0.4f - t * 0.19f;
    const float a3 = 5.3f - t * 0.21f, b3 = 2.2f + t * 0.33f;
    const float sx2 = sin(p.x + b2), cx2 = cos(p.x + b2);
    const float sx3 = sin(p.x + a3), cx3 = cos(p.x + a3);
    const float sy1 = sin(p.y + a1), cy1 = cos(p.y + a1);
    const float sy3 = sin(p.y + b3), cy3 = cos(p.y + b3);
    const float sz1 = sin(p.z + b1), cz1 = cos(p.z + b1);
    const float sz2 = sin(p.z + a2), cz2 = cos(p.z + a2);
    // psi.x = sin(y + a1) sin(z + b1), psi.y = sin(z + a2) sin(x + b2), psi.z = sin(x + a3) sin(y + b3)
    return float3(sx3 * cy3 - cz2 * sx2, sy1 * cz1 - cx3 * sy3, sz2 * cx2 - cy1 * sz1);
}

QUENCH_PARTICLE_FN float3 quenchParticleCurl(float3 p, float t) {
    // The second octave in a turned frame (R, orthonormal rows), so that the two do not line up, and turned back
    // (R^T), which keeps it divergence-free.
    const float3 r0 = float3(0.36f, 0.48f, -0.8f);
    const float3 r1 = float3(-0.8f, 0.6f, 0.0f);
    const float3 r2 = float3(0.48f, 0.64f, 0.6f);
    const float3 q = float3(dot(r0, p), dot(r1, p), dot(r2, p)) * 2.03f + float3(1.3f, 4.1f, 2.7f);
    const float3 f = quenchCurlOctave(q, t * 1.7f);
    const float3 v = quenchCurlOctave(p, t) + (r0 * f.x + r1 * f.y + r2 * f.z) * 0.5f;
    return v * 1.26f;
}

// The air's velocity at p (m/s): the noise and the weather's wind.
QUENCH_PARTICLE_FN float3 quenchParticleAir(QUENCH_PARTICLE_UNIFORMS u, float3 p, float time) {
    float3 air = float3(0.0f, 0.0f, 0.0f);
    if (u.noise.x > 0.0f) air = quenchParticleCurl(p * u.noise.y, time * u.noise.z) * u.noise.x;
    if (u.noise.w > 0.0f && u.wind.speed.x > 0.0f) {
        const QuenchWind w = quenchWeatherWind(u.wind, p);
        air = air + (w.mean + w.gust) * u.noise.w;
    }
    return air;
}

// A particle `dt` s later, its step ending at `timeEnd` on the particles' clock. Particles past their lifetime and
// empty slots only age.
QUENCH_PARTICLE_FN QuenchParticle quenchParticleStep(QUENCH_PARTICLE_UNIFORMS u, QuenchParticle q, float dt,
                                                     float timeEnd) {
    if (q.b.w <= 0.0f || q.a.w >= q.b.w || dt <= 0.0f) {
        q.a.w = q.a.w + max(dt, 0.0f);
        return q;
    }
    // Without air (no noise, no wind) one substep is exact; with it the air is taken as constant over each.
    const bool still = u.noise.x <= 0.0f && (u.noise.w <= 0.0f || u.wind.speed.x <= 0.0f);
    const float n = still ? 1.0f : clamp(ceil(dt / QUENCH_PARTICLE_SUBSTEP), 1.0f, float(QUENCH_PARTICLE_MAX_SUBSTEPS));
    const float h = dt / n;
    const float3 g = float3(u.gravity.x, u.gravity.y, u.gravity.z);
    const float k = u.gravity.w;
    const float keep = exp(-k * h);
    float3 p = float3(q.a.x, q.a.y, q.a.z);
    float3 v = float3(q.b.x, q.b.y, q.b.z);
    float t = timeEnd - dt;
    for (int i = 0; i < int(n); ++i) {
        t = t + h;
        const float3 air = still ? float3(0.0f, 0.0f, 0.0f) : quenchParticleAir(u, p, t);
        // dv/dt = g - k (v - air), integrated exactly over the substep: v goes toward air + g / k.
        if (k > 1e-4f) {
            const float3 terminal = air + g / k;
            const float3 dv = v - terminal;
            p = p + terminal * h + dv * ((1.0f - keep) / k);
            v = terminal + dv * keep;
        } else {
            p = p + v * h + g * (0.5f * h * h);
            v = v + g * h;
        }
    }
    q.a = float4(p.x, p.y, p.z, q.a.w + dt);
    q.b = float4(v.x, v.y, v.z, q.b.w);
    return q;
}

// A curve's value at t (0..1), linear between its samples.
QUENCH_PARTICLE_FN float quenchParticleSize(QUENCH_PARTICLE_UNIFORMS u, float t) {
    const float x = saturate(t) * float(QUENCH_PARTICLE_CURVE - 1);
    const int i = int(min(floor(x), float(QUENCH_PARTICLE_CURVE - 2)));
    const float4 s0 = u.size[i / 4];
    const float4 s1 = u.size[(i + 1) / 4];
    return lerp(s0[i % 4], s1[(i + 1) % 4], x - float(i));
}

QUENCH_PARTICLE_FN float4 quenchParticleColor(QUENCH_PARTICLE_UNIFORMS u, float t) {
    const float x = saturate(t) * float(QUENCH_PARTICLE_CURVE - 1);
    const int i = int(min(floor(x), float(QUENCH_PARTICLE_CURVE - 2)));
    return lerp(u.colorCurve[i], u.colorCurve[i + 1], x - float(i));
}

// How bright a particle is at `time` against its mean: its own brightness (look.w) and its twinkle, a sine raised to
// the sharpness (short flashes) at a rate of its own, divided by its mean so that twinkling keeps the average light.
QUENCH_PARTICLE_FN float quenchParticleTwinkle(QUENCH_PARTICLE_UNIFORMS u, float random, float time) {
    const float r2 = frac(random * 97.13f + 0.37f);
    const float r3 = frac(random * 13.71f + 0.71f);
    float bright = max(1.0f + (r2 - 0.5f) * 2.0f * u.look.w, 0.0f);
    if (u.look.x > 0.0f) {
        const float rate = u.look.y * (0.6f + 0.8f * r3);
        const float k = max(u.look.z, 1.0f);
        const float f = pow(0.5f + 0.5f * sin(6.2831853f * (time * rate + random * 17.0f)), k);
        // The mean of ((1 + sin) / 2)^k, close for k >= 1 (1/2 at k = 1, 0.198 at k = 8 against 0.196).
        const float mean = 1.0f / sqrt(3.14159265f * k + 0.86f);
        bright = bright * lerp(1.0f, f / mean, u.look.x);
    }
    return bright;
}

// What the draw reads of a particle at `time` on the particles' clock.
QUENCH_PARTICLE_FN QuenchParticleInstance quenchParticleLook(QUENCH_PARTICLE_UNIFORMS u, QuenchParticle q, float time) {
    QuenchParticleInstance o;
    o.a = float4(q.a.x, q.a.y, q.a.z, 0.0f);
    o.b = float4(0.0f, 0.0f, 0.0f, 0.0f);
    o.c = float4(q.b.x, q.b.y, q.b.z, 0.0f);
    if (q.b.w <= 0.0f || q.a.w >= q.b.w) return o;
    const float t = q.a.w / q.b.w;
    const float4 c = quenchParticleColor(u, t) * u.color;
    const float bright = quenchParticleTwinkle(u, q.c.x, time);
    o.a.w = q.c.y * max(quenchParticleSize(u, t), 0.0f);
    o.b = float4(c.x * bright, c.y * bright, c.z * bright, saturate(c.w));
    o.c.w = q.c.z + q.c.w * q.a.w;
    return o;
}

QUENCH_SHARED_END

#endif
