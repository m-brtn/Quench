#include "Decoder.h"

#include <miniaudio.h>

#include <format>
#include <string>

namespace quench::audio::detail {
namespace {

// A miniaudio virtual file system over one quench::fs reader: the decoder opens "its" file and reads it in pieces.
struct ReaderVfs {
    ma_vfs_callbacks callbacks; // first: miniaudio calls through the ma_vfs* it is given
    std::unique_ptr<fs::Reader> reader;
    std::int64_t cursor = 0;
};

ReaderVfs* vfsOf(ma_vfs* vfs) { return static_cast<ReaderVfs*>(vfs); }

ma_result vfsOpen(ma_vfs* vfs, const char*, ma_uint32 mode, ma_vfs_file* file) {
    if (mode & MA_OPEN_MODE_WRITE) return MA_ACCESS_DENIED;
    vfsOf(vfs)->cursor = 0;
    *file = vfs;
    return MA_SUCCESS;
}

ma_result vfsOpenW(ma_vfs*, const wchar_t*, ma_uint32, ma_vfs_file*) { return MA_NOT_IMPLEMENTED; }
ma_result vfsClose(ma_vfs*, ma_vfs_file) { return MA_SUCCESS; }

ma_result vfsRead(ma_vfs* vfs, ma_vfs_file, void* dst, size_t size, size_t* got) {
    ReaderVfs& self = *vfsOf(vfs);
    Result<std::size_t> n = self.reader->read(std::uint64_t(self.cursor), {static_cast<std::uint8_t*>(dst), size});
    if (!n) {
        if (got) *got = 0;
        return MA_IO_ERROR;
    }
    self.cursor += std::int64_t(*n);
    if (got) *got = *n;
    return *n == 0 && size > 0 ? MA_AT_END : MA_SUCCESS;
}

ma_result vfsWrite(ma_vfs*, ma_vfs_file, const void*, size_t, size_t*) { return MA_ACCESS_DENIED; }

ma_result vfsSeek(ma_vfs* vfs, ma_vfs_file, ma_int64 offset, ma_seek_origin origin) {
    ReaderVfs& self = *vfsOf(vfs);
    const auto size = std::int64_t(self.reader->size());
    const std::int64_t base = origin == ma_seek_origin_start ? 0 : origin == ma_seek_origin_end ? size : self.cursor;
    if (base + offset < 0) return MA_INVALID_ARGS;
    self.cursor = base + offset;
    return MA_SUCCESS;
}

ma_result vfsTell(ma_vfs* vfs, ma_vfs_file, ma_int64* cursor) {
    *cursor = vfsOf(vfs)->cursor;
    return MA_SUCCESS;
}

ma_result vfsInfo(ma_vfs* vfs, ma_vfs_file, ma_file_info* info) {
    info->sizeInBytes = vfsOf(vfs)->reader->size();
    return MA_SUCCESS;
}

} // namespace

struct Decoder::Impl {
    ma_decoder decoder{};
    bool initialized = false;
    std::unique_ptr<ReaderVfs> vfs; // null for a decoder over memory
    std::string name;

    ~Impl() {
        if (initialized) ma_decoder_uninit(&decoder);
    }
};

const char* resultName(int maResult) { return ma_result_description(ma_result(maResult)); }

Decoder::Decoder(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {
    m_format.channels = m_impl->decoder.outputChannels;
    m_format.sampleRate = m_impl->decoder.outputSampleRate;
}

Decoder::~Decoder() = default;

namespace {

// Opens with the file's channel count, again with two channels if it has more; open(config) does the opening.
template <class Open>
Result<void> openDecoder(ma_decoder& decoder, bool& initialized, Open open, const std::string& name) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_result result = open(config);
    if (result != MA_SUCCESS)
        return Error{std::format("{}: not a WAV, FLAC or MP3 file ({})", name, resultName(result))};
    initialized = true;
    if (decoder.outputChannels > 2) {
        ma_decoder_uninit(&decoder);
        initialized = false;
        config = ma_decoder_config_init(ma_format_f32, 2, 0);
        result = open(config);
        if (result != MA_SUCCESS)
            return Error{std::format("{}: cannot mix down to stereo ({})", name, resultName(result))};
        initialized = true;
    }
    if (decoder.outputChannels == 0 || decoder.outputSampleRate == 0)
        return Error{std::format("{}: no channels or sample rate", name)};
    return {};
}

} // namespace

Result<std::unique_ptr<Decoder>> Decoder::fromMemory(std::span<const std::uint8_t> bytes) {
    auto impl = std::make_unique<Impl>();
    impl->name = "sound";
    Result<void> opened = openDecoder(
        impl->decoder, impl->initialized,
        [&](const ma_decoder_config& config) {
            return ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &impl->decoder);
        },
        impl->name);
    if (!opened) return Error{opened.error()};
    return std::unique_ptr<Decoder>(new Decoder(std::move(impl)));
}

Result<std::unique_ptr<Decoder>> Decoder::fromReader(std::unique_ptr<fs::Reader> reader, std::string_view name) {
    auto impl = std::make_unique<Impl>();
    impl->name = std::string(name);
    impl->vfs = std::make_unique<ReaderVfs>();
    impl->vfs->callbacks = {vfsOpen, vfsOpenW, vfsClose, vfsRead, vfsWrite, vfsSeek, vfsTell, vfsInfo};
    impl->vfs->reader = std::move(reader);
    Result<void> opened = openDecoder(
        impl->decoder, impl->initialized,
        [&](const ma_decoder_config& config) {
            return ma_decoder_init_vfs(impl->vfs.get(), impl->name.c_str(), &config, &impl->decoder);
        },
        impl->name);
    if (!opened) return Error{opened.error()};
    return std::unique_ptr<Decoder>(new Decoder(std::move(impl)));
}

std::optional<std::uint64_t> Decoder::length() const {
    ma_uint64 frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&m_impl->decoder, &frames) != MA_SUCCESS || frames == 0)
        return std::nullopt;
    return std::uint64_t(frames);
}

Result<std::uint32_t> Decoder::read(float* out, std::uint32_t frames) {
    ma_uint64 got = 0;
    const ma_result result = ma_decoder_read_pcm_frames(&m_impl->decoder, out, frames, &got);
    if (result != MA_SUCCESS && result != MA_AT_END)
        return Error{std::format("{}: decoding failed ({})", m_impl->name, resultName(result))};
    return std::uint32_t(got);
}

Result<void> Decoder::seekToStart() {
    const ma_result result = ma_decoder_seek_to_pcm_frame(&m_impl->decoder, 0);
    if (result != MA_SUCCESS) return Error{std::format("{}: cannot seek ({})", m_impl->name, resultName(result))};
    return {};
}

} // namespace quench::audio::detail
