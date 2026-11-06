#pragma once

// 3D sound: one listener, emitters with a distance attenuation model, stereo panning and Doppler. The mixer applies
// these per voice on the audio thread; the functions are public for tests and for games that want the same numbers.
// Coordinates follow the engine: right-handed, Y up, meters, seconds.

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace quench::audio {

enum class Attenuation : std::uint8_t {
    None,        // full volume at any distance
    Inverse,     // min / (min + rolloff * (d - min)): the physical 1/d falloff with rolloff 1
    Linear,      // 1 - rolloff * (d - min) / (max - min): silent at max with rolloff 1
    Exponential, // (d / min)^-rolloff
};

const char* attenuationName(Attenuation attenuation);
std::optional<Attenuation> parseAttenuation(std::string_view name);

struct Listener {
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    glm::vec3 velocity{0.0f}; // m/s, for Doppler
};

struct Emitter {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f}; // m/s, for Doppler
    Attenuation attenuation = Attenuation::Inverse;
    float minDistance = 1.0f;   // full volume up to here; panning also narrows toward the center inside it
    float maxDistance = 100.0f; // the distance used stops growing here
    float rolloff = 1.0f;
    float doppler = 1.0f; // scale of the pitch shift from motion; 0: none
};

// Gain of the distance model in [0, 1]; the distance is clamped to [minDistance, maxDistance].
float distanceGain(const Emitter& emitter, float distance);

// Constant-power pan of a mono source: pan -1 (left) .. 1 (right) to (left, right) gains with left² + right² = 1
// (both 0.707 at the center).
glm::vec2 panGains(float pan);
// Balance of a stereo source: pan 0 keeps both channels at 1, pan 1 silences the left one.
glm::vec2 balanceGains(float pan);

struct SpatialResult {
    float gain = 1.0f;  // distance model
    float pan = 0.0f;   // -1 .. 1
    float pitch = 1.0f; // Doppler factor
};

// Gain, pan and Doppler factor of an emitter heard by the listener. The pan is the direction's component along the
// listener's right axis (front and back sound alike in stereo), scaled down inside the emitter's minDistance. The
// Doppler factor is (c + vl) / (c + ve) with the listener's and emitter's velocities along the line from the listener
// to the emitter (approaching raises the pitch), each scaled by dopplerScale * emitter.doppler, limited to 0.9 c and
// clamped to [0.25, 4].
SpatialResult spatialize(const Listener& listener, const Emitter& emitter, float speedOfSound = 343.0f,
                         float dopplerScale = 1.0f);

} // namespace quench::audio
