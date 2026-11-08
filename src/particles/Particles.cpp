#include <quench/particles/Particles.h>

#include <glm/common.hpp>

#include <algorithm>
#include <cstddef>

namespace quench::particles {
namespace {

template <class Key, class Value>
Value evaluate(const std::array<Key, 8>& keys, std::uint32_t count, float t, const Value& fallback) {
    if (count == 0) return fallback;
    if (t <= keys[0].t) return keys[0].value;
    for (std::uint32_t i = 1; i < count; ++i)
        if (t < keys[i].t) {
            const Key& a = keys[i - 1];
            const Key& b = keys[i];
            const float span = b.t - a.t;
            const float f = span > 0.0f ? (t - a.t) / span : 1.0f;
            return a.value + (b.value - a.value) * f;
        }
    return keys[count - 1].value;
}

} // namespace

FloatCurve::FloatCurve(float value) {
    keys[0] = {0.0f, value};
    count = 1;
}

FloatCurve::FloatCurve(std::initializer_list<Key> list) {
    for (const Key& k : list) {
        if (count == kMaxKeys) break;
        keys[count++] = k;
    }
}

float FloatCurve::operator()(float t) const { return evaluate(keys, count, t, 1.0f); }

ColorCurve::ColorCurve(const glm::vec4& value) {
    keys[0] = {0.0f, value};
    count = 1;
}

ColorCurve::ColorCurve(std::initializer_list<Key> list) {
    for (const Key& k : list) {
        if (count == kMaxKeys) break;
        keys[count++] = k;
    }
}

glm::vec4 ColorCurve::operator()(float t) const { return evaluate(keys, count, t, glm::vec4(1.0f)); }

ParticleUniforms uniformsOf(const ParticleDesc& desc) {
    ParticleUniforms u{};
    u.gravity = glm::vec4(desc.gravity, std::max(desc.drag, 0.0f));
    u.noise = glm::vec4(std::max(desc.noise, 0.0f), 1.0f / std::max(desc.noiseScale, 1e-3f),
                        std::max(desc.noiseRate, 0.0f), std::clamp(desc.wind, 0.0f, 1.0f));
    u.look =
        glm::vec4(std::clamp(desc.twinkle, 0.0f, 1.0f), std::max(desc.twinkleRate, 0.0f),
                  std::clamp(desc.twinkleSharpness, 1.0f, 64.0f), std::clamp(desc.brightnessVariation, 0.0f, 1.0f));
    u.color = glm::vec4(glm::max(desc.color, glm::vec3(0.0f)), std::clamp(desc.opacity, 0.0f, 1.0f));
    for (int i = 0; i < QUENCH_PARTICLE_CURVE; ++i) {
        const float t = float(i) / float(QUENCH_PARTICLE_CURVE - 1);
        u.size[std::size_t(i / 4)][i % 4] = desc.sizeOverLife(t);
        u.colorCurve[std::size_t(i)] = desc.colorOverLife(t);
    }
    return u;
}

} // namespace quench::particles
