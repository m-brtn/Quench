#pragma once

// The KTX 2.0 header layout shared by Texture.cpp (plain files) and Basis.cpp (Basis Universal files).

#include <cstddef>
#include <cstdint>

namespace quench::assets::detail::ktx2 {

constexpr std::uint8_t kIdentifier[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
constexpr std::size_t kHeaderBytes = 80; // identifier + 9 u32 + index (4 u32 + 2 u64)
constexpr std::size_t kLevelEntryBytes = 24;
constexpr std::uint32_t kMaxLevels = 16;
constexpr std::uint32_t kMaxExtent = 1u << (kMaxLevels - 1); // the full chain of the largest texture has 16 levels

inline std::uint32_t get32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
inline std::uint64_t get64(const std::uint8_t* p) {
    return std::uint64_t(get32(p)) | std::uint64_t(get32(p + 4)) << 32;
}

} // namespace quench::assets::detail::ktx2
