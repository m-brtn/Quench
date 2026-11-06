#pragma once

// WAV files of the mix: offline captures (--audio-capture) and test data.

#include <quench/core/FileSystem.h>

#include <cstdint>
#include <span>

namespace quench::audio {

enum class WavSampleFormat : std::uint8_t { Pcm16, Float32 };

// A RIFF WAVE file of interleaved float samples; PCM16 clamps to [-1, 1] and rounds.
fs::Bytes encodeWav(std::span<const float> samples, std::uint32_t channels, std::uint32_t sampleRate,
                    WavSampleFormat format = WavSampleFormat::Pcm16);

} // namespace quench::audio
