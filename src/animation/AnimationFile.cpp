#include "AnimationFile.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <bit>
#include <format>

namespace quench::animation::detail {

static_assert(std::endian::native == std::endian::little, "cooked animation files are little endian");

std::vector<std::uint8_t> wrapPayload(std::uint32_t magic, std::uint32_t version,
                                      std::span<const std::uint8_t> payload) {
    ByteWriter w;
    w.u32(magic);
    w.u32(version);
    const std::uint64_t size = payload.size();
    const std::uint64_t hash = XXH64(payload.data(), payload.size(), 0);
    w.bytes(&size, 8);
    w.bytes(&hash, 8);
    w.bytes(payload.data(), payload.size());
    return std::move(w.out);
}

Result<Payload> unwrapPayload(std::uint32_t magic, std::uint32_t latest, std::span<const std::uint8_t> file,
                              const char* what) {
    ByteReader r(file);
    std::uint32_t fileMagic = 0, version = 0;
    std::uint64_t size = 0, hash = 0;
    if (!r.u32(fileMagic) || fileMagic != magic) return Error{std::format("not a {}", what)};
    if (!r.u32(version) || version == 0 || version > latest)
        return Error{latest == 1 ? std::format("{} version {} (this engine reads 1)", what, version)
                                 : std::format("{} version {} (this engine reads 1 to {})", what, version, latest)};
    if (!r.bytes(&size, 8) || !r.bytes(&hash, 8)) return Error{std::format("truncated {} header", what)};
    if (size != r.remaining())
        return Error{std::format("corrupt {}: {} payload bytes, the header says {}", what, r.remaining(), size)};
    const std::span<const std::uint8_t> payload = file.subspan(24);
    if (XXH64(payload.data(), payload.size(), 0) != hash) return Error{std::format("corrupt {}: checksum", what)};
    return Payload{payload, version};
}

std::size_t SpanStream::Read(void* buffer, std::size_t size) {
    const std::size_t n = std::min(size, m_data.size() - m_at);
    if (n < size) m_overrun = true;
    std::memcpy(buffer, m_data.data() + m_at, n);
    if (n < size) std::memset(static_cast<std::uint8_t*>(buffer) + n, 0, size - n);
    m_at += n;
    return n;
}

int SpanStream::Seek(int offset, Origin origin) {
    const std::int64_t base = origin == kSet ? 0 : origin == kEnd ? std::int64_t(m_data.size()) : std::int64_t(m_at);
    const std::int64_t at = base + offset;
    if (at < 0 || at > std::int64_t(m_data.size())) return -1;
    m_at = std::size_t(at);
    return 0;
}

} // namespace quench::animation::detail
