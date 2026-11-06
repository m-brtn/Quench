#include <quench/audio/Spatial.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace quench::audio {

const char* attenuationName(Attenuation attenuation) {
    switch (attenuation) {
    case Attenuation::None: return "none";
    case Attenuation::Inverse: return "inverse";
    case Attenuation::Linear: return "linear";
    case Attenuation::Exponential: return "exponential";
    }
    return "?";
}

std::optional<Attenuation> parseAttenuation(std::string_view name) {
    for (Attenuation a : {Attenuation::None, Attenuation::Inverse, Attenuation::Linear, Attenuation::Exponential})
        if (name == attenuationName(a)) return a;
    return std::nullopt;
}

float distanceGain(const Emitter& emitter, float distance) {
    const float minDistance = std::max(emitter.minDistance, 1e-3f);
    const float maxDistance = std::max(emitter.maxDistance, minDistance);
    const float d = std::clamp(distance, minDistance, maxDistance);
    const float rolloff = std::max(emitter.rolloff, 0.0f);
    switch (emitter.attenuation) {
    case Attenuation::None: return 1.0f;
    case Attenuation::Inverse: return minDistance / (minDistance + rolloff * (d - minDistance));
    case Attenuation::Linear:
        if (maxDistance <= minDistance) return 1.0f;
        return std::clamp(1.0f - rolloff * (d - minDistance) / (maxDistance - minDistance), 0.0f, 1.0f);
    case Attenuation::Exponential: return std::pow(d / minDistance, -rolloff);
    }
    return 1.0f;
}

glm::vec2 panGains(float pan) {
    const float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * float(std::numbers::pi / 4.0);
    return {std::cos(angle), std::sin(angle)};
}

glm::vec2 balanceGains(float pan) {
    const float p = std::clamp(pan, -1.0f, 1.0f);
    return {p > 0.0f ? 1.0f - p : 1.0f, p < 0.0f ? 1.0f + p : 1.0f};
}

SpatialResult spatialize(const Listener& listener, const Emitter& emitter, float speedOfSound, float dopplerScale) {
    SpatialResult result;
    const glm::vec3 offset = emitter.position - listener.position;
    const float distance = glm::length(offset);
    result.gain = distanceGain(emitter, distance);
    if (distance < 1e-4f) return result;
    const glm::vec3 direction = offset / distance;

    glm::vec3 right = glm::cross(listener.forward, listener.up);
    const float rightLength = glm::length(right);
    if (rightLength > 1e-6f) {
        right /= rightLength;
        const float nearField = std::clamp(distance / std::max(emitter.minDistance, 1e-3f), 0.0f, 1.0f);
        result.pan = std::clamp(glm::dot(direction, right), -1.0f, 1.0f) * nearField;
    }

    const float scale = dopplerScale * emitter.doppler;
    const float c = std::max(speedOfSound, 1.0f);
    if (scale != 0.0f) {
        // Speeds toward the other end of the line: the listener moving toward the emitter, the emitter away from
        // the listener.
        const float limit = 0.9f * c;
        const float vl = std::clamp(glm::dot(listener.velocity, direction) * scale, -limit, limit);
        const float ve = std::clamp(glm::dot(emitter.velocity, direction) * scale, -limit, limit);
        result.pitch = std::clamp((c + vl) / (c + ve), 0.25f, 4.0f);
    }
    return result;
}

} // namespace quench::audio
