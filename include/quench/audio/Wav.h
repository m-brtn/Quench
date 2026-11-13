#pragma once

// WAV files of the mix (offline captures, --audio-capture), of microphone recordings (uploads) and test data.

#include <quench/core/FileSystem.h>

#include <cstdint>
#include <span>

namespace quench::audio {

enum class WavSampleFormat : std::uint8_t { Pcm16, Float32 };

// A RIFF WAVE file of interleaved float samples; PCM16 clamps to [-1, 1] and rounds.
fs::Bytes encodeWav(std::span<const float> samples, std::uint32_t channels, std::uint32_t sampleRate,
                    WavSampleFormat format = WavSampleFormat::Pcm16);
// A 16-bit PCM RIFF WAVE file of interleaved samples as they are (a microphone's recording: MicRecording::pcm16).
// Reading one back from memory: decodeSound (Sound.h).
fs::Bytes encodeWavPcm16(std::span<const std::int16_t> samples, std::uint32_t channels, std::uint32_t sampleRate);

} // namespace quench::audio
