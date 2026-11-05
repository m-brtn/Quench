#include <quench/render/Daylight.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>

namespace quench::render {
namespace {

float smooth(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float luminance(const glm::vec3& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

// Palette keyed by sun elevation (degrees), linear HDR. From TrailForge (src/render/TimeOfDay.cpp at b97883e).
struct Key {
    float elevation;
    glm::vec3 sun, glow, disc, zenith, horizon, fog, ambient;
    float exposure;
};

const Key kKeys[] = {
    {-18.0f,
     {0, 0, 0},
     {0, 0, 0},
     {0, 0, 0},
     {0.0030f, 0.0050f, 0.0130f},
     {0.0080f, 0.0115f, 0.0230f},
     {0.0080f, 0.0110f, 0.0210f},
     {0.012f, 0.016f, 0.032f},
     2.7f},
    {-9.0f,
     {0, 0, 0},
     {0.05f, 0.02f, 0.02f},
     {0, 0, 0},
     {0.0070f, 0.0110f, 0.0300f},
     {0.040f, 0.032f, 0.050f},
     {0.032f, 0.028f, 0.045f},
     {0.020f, 0.022f, 0.040f},
     2.4f},
    {-4.0f,
     {0, 0, 0},
     {0.70f, 0.20f, 0.07f},
     {0, 0, 0},
     {0.040f, 0.040f, 0.110f},
     {0.42f, 0.15f, 0.08f},
     {0.26f, 0.12f, 0.09f},
     {0.075f, 0.060f, 0.085f},
     1.75f},
    {0.0f,
     {1.30f, 0.36f, 0.07f},
     {1.60f, 0.48f, 0.10f},
     {3.2f, 1.00f, 0.18f},
     {0.200f, 0.110f, 0.200f},
     {1.40f, 0.48f, 0.12f},
     {0.95f, 0.40f, 0.16f},
     {0.26f, 0.16f, 0.14f},
     1.30f},
    {5.0f,
     {3.20f, 1.30f, 0.38f},
     {2.60f, 1.05f, 0.26f},
     {3.6f, 1.55f, 0.36f},
     {0.340f, 0.200f, 0.260f},
     {1.85f, 0.78f, 0.22f},
     {1.25f, 0.62f, 0.26f},
     {0.40f, 0.26f, 0.22f},
     1.10f},
    {12.0f,
     {4.40f, 3.10f, 1.85f},
     {2.40f, 1.70f, 1.00f},
     {9.0f, 7.0f, 4.0f},
     {0.200f, 0.300f, 0.640f},
     {1.05f, 0.80f, 0.58f},
     {0.85f, 0.74f, 0.62f},
     {0.28f, 0.30f, 0.38f},
     1.05f},
    {25.0f,
     {5.00f, 4.40f, 3.70f},
     {2.60f, 2.40f, 2.10f},
     {40.0f, 37.0f, 32.0f},
     {0.170f, 0.420f, 1.100f},
     {0.68f, 0.86f, 1.12f},
     {0.66f, 0.82f, 1.05f},
     {0.21f, 0.34f, 0.62f},
     1.0f},
    {60.0f,
     {5.40f, 5.13f, 4.75f},
     {2.60f, 2.50f, 2.30f},
     {45.0f, 43.0f, 40.0f},
     {0.150f, 0.380f, 1.150f},
     {0.62f, 0.80f, 1.12f},
     {0.60f, 0.76f, 1.05f},
     {0.21f, 0.35f, 0.66f},
     1.0f},
};

Key sample(float elevation) {
    const int n = int(std::size(kKeys));
    if (elevation <= kKeys[0].elevation) return kKeys[0];
    if (elevation >= kKeys[n - 1].elevation) return kKeys[n - 1];
    int i = 0;
    while (elevation > kKeys[i + 1].elevation) ++i;
    const Key& a = kKeys[i];
    const Key& b = kKeys[i + 1];
    const float t = smooth(a.elevation, b.elevation, elevation); // no visible kinks while the sun moves
    const auto mix = [t](const glm::vec3& x, const glm::vec3& y) { return x + (y - x) * t; };
    return {elevation,           mix(a.sun, b.sun),         mix(a.glow, b.glow),
            mix(a.disc, b.disc), mix(a.zenith, b.zenith),   mix(a.horizon, b.horizon),
            mix(a.fog, b.fog),   mix(a.ambient, b.ambient), a.exposure + (b.exposure - a.exposure) * t};
}

} // namespace

Daylight daylightFor(const glm::vec3& towardSun) {
    const float elevation = std::asin(std::clamp(towardSun.y, -1.0f, 1.0f)) * 180.0f / std::numbers::pi_v<float>;
    const Key k = sample(elevation);
    Daylight d;
    Environment& e = d.environment;
    e.skyZenith = k.zenith;
    e.skyHorizon = k.horizon;
    e.fogColor = k.fog;
    e.sunGlow = k.glow;
    e.exposure = k.exposure;
    // Stylized disc: 1 degree high up, 2.6 degrees at the horizon.
    const float discDegrees = 1.0f + 1.6f * (1.0f - smooth(0.0f, 20.0f, elevation));
    e.sunAngularRadius = discDegrees * std::numbers::pi_v<float> / 180.0f;
    e.sunDisc = k.disc * smooth(-discDegrees, 0.0f, elevation);
    d.sunColor = k.sun * smooth(-0.5f, 0.5f, elevation);
    e.skyAmbient = k.ambient;
    const float bounce = luminance(d.sunColor) * 0.012f + luminance(e.skyAmbient) * 0.16f;
    e.groundAmbient = glm::vec3(1.0f, 0.82f, 0.58f) * bounce;
    e.skyGround = glm::mix(k.fog * 0.85f, e.groundAmbient * 1.4f, 0.5f);
    return d;
}

glm::vec3 skyDirection(float elevationDegrees, float azimuthDegrees) {
    constexpr float kRad = std::numbers::pi_v<float> / 180.0f;
    const float el = elevationDegrees * kRad, az = azimuthDegrees * kRad;
    return glm::normalize(glm::vec3(std::cos(el) * std::sin(az), std::sin(el), -std::cos(el) * std::cos(az)));
}

} // namespace quench::render
