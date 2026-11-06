#pragma once

// miniaudio's decoders (WAV, FLAC, MP3) behind a small interface: from bytes in memory or from a quench::fs reader,
// to interleaved float frames at the file's rate, mono or stereo (more channels are mixed down to stereo).

#include <quench/audio/Sound.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Result.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace quench::audio::detail {

class Decoder {
public:
    // `bytes` must outlive the decoder.
    static Result<std::unique_ptr<Decoder>> fromMemory(std::span<const std::uint8_t> bytes);
    // `name` picks the decoder to try first by its extension and names the file in errors.
    static Result<std::unique_ptr<Decoder>> fromReader(std::unique_ptr<fs::Reader> reader, std::string_view name);
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    const Format& format() const { return m_format; }
    // Total frames, when the file says.
    std::optional<std::uint64_t> length() const;
    // Up to `frames` frames into `out`; 0 at the end.
    Result<std::uint32_t> read(float* out, std::uint32_t frames);
    Result<void> seekToStart();

private:
    struct Impl;
    explicit Decoder(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
    Format m_format;
};

const char* resultName(int maResult);

} // namespace quench::audio::detail
