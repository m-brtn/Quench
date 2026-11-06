#pragma once

// Sounds decoded into memory (effects, short loops): 32-bit float PCM at the file's sample rate, mono or stereo.
// WAV, FLAC and MP3 are decoded with miniaudio; files with more than two channels are mixed down to stereo. Long
// files (music) are streamed instead (AudioSystem::playStream).

#include <quench/assets/AssetManager.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Result.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace quench::audio {

struct Format {
    std::uint32_t channels = 1; // 1 or 2
    std::uint32_t sampleRate = 48000;
};

struct SoundData {
    Format format;
    std::vector<float> samples; // interleaved frames

    std::uint64_t frames() const { return format.channels ? samples.size() / format.channels : 0; }
    double duration() const { return format.sampleRate ? double(frames()) / format.sampleRate : 0.0; }
    std::uint64_t bytes() const { return samples.size() * sizeof(float); }
};

// Decoded sounds larger than this are refused (stream them instead).
inline constexpr std::uint64_t kMaxSoundBytes = 256ull << 20;

// A whole WAV, FLAC or MP3 file. A file that does not decode, or decodes to no frames, is an error.
Result<SoundData> decodeSound(std::span<const std::uint8_t> file);

class AudioSystem;

// A sound file loaded through the asset manager (type "sound", registered by the AudioSystem). It plays as silence
// while it loads and after a failed load (the asset manager logs the failure).
class SoundAsset final : public assets::Asset {
public:
    // Empty while loading and after a failure. A reload replaces it; sounds already playing keep the data they
    // started with.
    const std::shared_ptr<const SoundData>& data() const { return m_data; }

private:
    friend class AudioSystem;
    SoundAsset() = default;

    Result<void> decode(fs::Bytes bytes) override;
    std::uint64_t uploadBytes() const override { return 0; }
    Result<void> finish() override;
    void unload() override { m_data.reset(); }
    assets::AssetMemory residentMemory() const override { return {m_data ? m_data->bytes() : 0, 0}; }

    std::shared_ptr<const SoundData> m_data;
    std::shared_ptr<const SoundData> m_pending; // decoded on a job, made resident by finish()
};

} // namespace quench::audio
