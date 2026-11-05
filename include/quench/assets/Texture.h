#pragma once

#include <quench/core/Result.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Cooked textures: KTX 2.0 files (the Khronos texture container) of one 2D image with its mip chain, in one of the
// formats below, without supercompression. quench_cook writes them; the runtime reads them and, on a device that
// cannot sample a block format, decodes it to RGBA8 (BC7 only so far; ASTC and Basis Universal transcoding are later
// work for Android and the web).
namespace quench::assets {

enum class PixelFormat : std::uint8_t { Rgba8, Rgba8Srgb, Bc7, Bc7Srgb, Astc4x4, Astc4x4Srgb };

struct PixelFormatInfo {
    const char* name;
    std::uint32_t vkFormat; // VkFormat, as KTX2 stores it
    std::uint8_t blockSize; // texels per block side (1 for RGBA8)
    std::uint8_t blockBytes;
    bool srgb;
};

const PixelFormatInfo& pixelFormatInfo(PixelFormat format);
// Bytes of one level of width x height texels (whole blocks).
std::uint64_t levelBytes(PixelFormat format, std::uint32_t width, std::uint32_t height);

// A texture with its mip levels in one buffer, level 0 (the largest) first, each level's block rows tightly packed.
struct TextureImage {
    PixelFormat format = PixelFormat::Rgba8;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipLevels = 0;
    std::vector<std::uint8_t> data;

    std::uint32_t levelWidth(std::uint32_t level) const;
    std::uint32_t levelHeight(std::uint32_t level) const;
    std::uint64_t levelOffset(std::uint32_t level) const;
    std::span<const std::uint8_t> level(std::uint32_t level) const;
};

// Checks the header, the format and every level's size. skipLevels drops that many of the largest levels (a lower
// texture quality) when the file has more.
Result<TextureImage> readKtx2(std::span<const std::uint8_t> file, std::uint32_t skipLevels = 0);
// Levels are stored smallest first, as the format requires. The output depends only on the image and `writer`.
Result<std::vector<std::uint8_t>> writeKtx2(const TextureImage& image, std::string_view writer = "quench_cook");

// BC7 to RGBA8 (the sRGB flag is kept), every level: the fallback where BC textures cannot be sampled.
Result<TextureImage> decodeBc7(const TextureImage& image);

} // namespace quench::assets
