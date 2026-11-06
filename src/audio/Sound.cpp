#include <quench/audio/Sound.h>

#include "Decoder.h"

#include <format>

namespace quench::audio {

Result<SoundData> decodeSound(std::span<const std::uint8_t> file) {
    Result<std::unique_ptr<detail::Decoder>> decoder = detail::Decoder::fromMemory(file);
    if (!decoder) return Error{decoder.error()};
    SoundData sound;
    sound.format = (*decoder)->format();
    const std::uint32_t channels = sound.format.channels;
    const std::uint64_t maxFrames = kMaxSoundBytes / (sizeof(float) * channels);
    if (std::optional<std::uint64_t> length = (*decoder)->length()) {
        if (*length > maxFrames)
            return Error{std::format("{:.0f} s is too long for a sound in memory: stream it",
                                     double(*length) / sound.format.sampleRate)};
        sound.samples.reserve(std::size_t(*length) * channels);
    }
    constexpr std::uint32_t kChunk = 16384;
    std::uint64_t frames = 0;
    for (;;) {
        sound.samples.resize(std::size_t(frames + kChunk) * channels);
        Result<std::uint32_t> got = (*decoder)->read(sound.samples.data() + frames * channels, kChunk);
        if (!got) return Error{got.error()};
        frames += *got;
        if (*got == 0) break;
        if (frames > maxFrames)
            return Error{std::format("more than {:.0f} s: too long for a sound in memory, stream it",
                                     double(maxFrames) / sound.format.sampleRate)};
    }
    sound.samples.resize(std::size_t(frames) * channels);
    sound.samples.shrink_to_fit();
    if (frames == 0) return Error{"no audio frames"};
    return sound;
}

Result<void> SoundAsset::decode(fs::Bytes bytes) {
    Result<SoundData> sound = decodeSound(bytes);
    if (!sound) return Error{sound.error()};
    m_pending = std::make_shared<const SoundData>(std::move(sound).value());
    return {};
}

Result<void> SoundAsset::finish() {
    m_data = std::move(m_pending);
    return {};
}

} // namespace quench::audio
