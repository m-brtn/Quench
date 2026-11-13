#include <quench/audio/Wav.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace quench::audio {
namespace {

void put16(fs::Bytes& out, std::uint16_t v) {
    out.push_back(std::uint8_t(v));
    out.push_back(std::uint8_t(v >> 8));
}

void put32(fs::Bytes& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i)));
}

void tag(fs::Bytes& out, const char (&name)[5]) { out.insert(out.end(), name, name + 4); }

void header(fs::Bytes& out, bool pcm, std::uint32_t channels, std::uint32_t sampleRate, std::uint32_t bytesPerSample,
            std::uint32_t dataBytes) {
    out.reserve(44 + dataBytes);
    tag(out, "RIFF");
    put32(out, 36 + dataBytes);
    tag(out, "WAVE");
    tag(out, "fmt ");
    put32(out, 16);
    put16(out, pcm ? 1 : 3); // PCM, IEEE float
    put16(out, std::uint16_t(channels));
    put32(out, sampleRate);
    put32(out, sampleRate * channels * bytesPerSample);
    put16(out, std::uint16_t(channels * bytesPerSample));
    put16(out, std::uint16_t(bytesPerSample * 8));
    tag(out, "data");
    put32(out, dataBytes);
}

} // namespace

fs::Bytes encodeWav(std::span<const float> samples, std::uint32_t channels, std::uint32_t sampleRate,
                    WavSampleFormat format) {
    const bool pcm = format == WavSampleFormat::Pcm16;
    const std::uint32_t bytesPerSample = pcm ? 2 : 4;
    fs::Bytes out;
    header(out, pcm, channels, sampleRate, bytesPerSample, std::uint32_t(samples.size() * bytesPerSample));
    for (float s : samples) {
        if (pcm) {
            const float clamped = std::isnan(s) ? 0.0f : std::clamp(s, -1.0f, 1.0f);
            put16(out, std::uint16_t(std::int16_t(std::lround(clamped * 32767.0f))));
        } else {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &s, 4);
            put32(out, bits);
        }
    }
    return out;
}

fs::Bytes encodeWavPcm16(std::span<const std::int16_t> samples, std::uint32_t channels, std::uint32_t sampleRate) {
    fs::Bytes out;
    header(out, true, channels, sampleRate, 2, std::uint32_t(samples.size() * 2));
    for (std::int16_t s : samples) put16(out, std::uint16_t(s));
    return out;
}

} // namespace quench::audio
