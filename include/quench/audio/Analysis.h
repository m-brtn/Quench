#pragma once

// Measurements of a stretch of audio, for tests and scripted checks (the scenario command audio_measure).

#include <cstdint>
#include <span>

namespace quench::audio {

struct Levels {
    float rms[2] = {0.0f, 0.0f};  // per channel (mono: both the same)
    float peak[2] = {0.0f, 0.0f}; // largest absolute sample per channel
    std::uint64_t frames = 0;

    float rmsMono() const; // of both channels together
};

Levels measureLevels(std::span<const float> samples, std::uint32_t channels);

struct Peak {
    float frequency = 0.0f; // Hz; 0 when the stretch is silent or too short
    float level = 0.0f;     // magnitude of the peak relative to a full-scale sine (about 1 for a full-scale sine)
};

// The strongest frequency between minHz and maxHz of the channels' average: a Hann-windowed FFT over the largest
// power-of-two count of frames that fits (at most 65536, from the end of the stretch), with the peak interpolated
// between bins. Resolution about sampleRate / FFT size / 10 for a clean tone.
Peak dominantFrequency(std::span<const float> samples, std::uint32_t channels, std::uint32_t sampleRate,
                       float minHz = 20.0f, float maxHz = 20000.0f);

} // namespace quench::audio
