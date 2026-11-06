#pragma once

// The container of .qskel and .qanim files, and byte streams for ozz-animation archives.
//
// Layout (little endian): u32 magic, u32 version, u64 payload size, u64 XXH64 of the payload, the payload. Inside a
// payload, values are 4-byte aligned like in .qmodel files: strings and blobs are a u32 size, the bytes, padding.

#include <quench/core/Result.h>

#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace quench::animation::detail {

inline constexpr std::uint32_t kSkeletonMagic = 0x4C4B5351; // "QSKL"
inline constexpr std::uint32_t kClipSetMagic = 0x4D4E4151;  // "QANM"
inline constexpr std::uint32_t kFileVersion = 1;

std::vector<std::uint8_t> wrapPayload(std::uint32_t magic, std::span<const std::uint8_t> payload);
// Checks magic, version, size and checksum; `what` names the file kind in errors.
Result<std::span<const std::uint8_t>> unwrapPayload(std::uint32_t magic, std::span<const std::uint8_t> file,
                                                    const char* what);

class ByteWriter {
public:
    void u32(std::uint32_t v) { bytes(&v, 4); }
    void f32(float v) { bytes(&v, 4); }
    void string(const std::string& s) { blob({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}); }
    void blob(std::span<const std::uint8_t> data) {
        u32(std::uint32_t(data.size()));
        bytes(data.data(), data.size());
        while (out.size() % 4 != 0) out.push_back(0);
    }
    void bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        out.insert(out.end(), p, p + size);
    }
    std::vector<std::uint8_t> out;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data) : m_data(data) {}
    bool u32(std::uint32_t& v) { return bytes(&v, 4); }
    bool f32(float& v) { return bytes(&v, 4); }
    bool string(std::string& s, std::size_t maxSize = 4096) {
        std::span<const std::uint8_t> b;
        if (!blob(b) || b.size() > maxSize) return false;
        s.assign(reinterpret_cast<const char*>(b.data()), b.size());
        return true;
    }
    bool blob(std::span<const std::uint8_t>& data) {
        std::uint32_t size = 0;
        if (!u32(size) || size > remaining()) return false;
        data = m_data.subspan(m_at, size);
        m_at = std::min(m_data.size(), (m_at + size + 3) & ~std::size_t(3));
        return true;
    }
    bool bytes(void* data, std::size_t size) {
        if (size > remaining()) return false;
        std::memcpy(data, m_data.data() + m_at, size);
        m_at += size;
        return true;
    }
    std::size_t remaining() const { return m_data.size() - m_at; }

private:
    std::span<const std::uint8_t> m_data;
    std::size_t m_at = 0;
};

// Read-only ozz stream over bytes in memory.
class SpanStream final : public ozz::io::Stream {
public:
    explicit SpanStream(std::span<const std::uint8_t> data) : m_data(data) {}
    bool opened() const override { return true; }
    std::size_t Read(void* buffer, std::size_t size) override;
    std::size_t Write(const void*, std::size_t) override { return 0; }
    int Seek(int offset, Origin origin) override;
    int Tell() const override { return int(m_at); }
    std::size_t Size() const override { return m_data.size(); }
    bool atEnd() const { return m_at == m_data.size(); }
    bool overrun() const { return m_overrun; }

private:
    std::span<const std::uint8_t> m_data;
    std::size_t m_at = 0;
    bool m_overrun = false;
};

// Saves one ozz object (Skeleton, Animation, tracks) into bytes.
template <class T>
std::vector<std::uint8_t> saveOzz(const T& object) {
    ozz::io::MemoryStream stream;
    {
        ozz::io::OArchive archive(&stream);
        archive << object;
    }
    std::vector<std::uint8_t> bytes(stream.Size());
    stream.Seek(0, ozz::io::Stream::kSet);
    stream.Read(bytes.data(), bytes.size());
    return bytes;
}

// Loads one ozz object; false when the tag differs, the archive is short or has bytes left over.
template <class T>
bool loadOzz(std::span<const std::uint8_t> bytes, T& object) {
    SpanStream stream(bytes);
    ozz::io::IArchive archive(&stream);
    if (!archive.TestTag<T>()) return false;
    archive >> object;
    return !stream.overrun() && stream.atEnd();
}

} // namespace quench::animation::detail
