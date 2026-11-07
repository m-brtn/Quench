#pragma once

#include <quench/core/Result.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Cooked textures: KTX 2.0 files (the Khronos texture container) of one 2D image with its mip chain, either in one of
// the formats below without supercompression, or in Basis Universal's ETC1S or UASTC 4x4 (one file for every device).
// quench_cook writes them; the runtime reads them (prepareTexture) and gives the device what it can sample: a plain
// file as it is (BC7 decoded to RGBA8 where BC textures cannot be sampled), a Basis Universal file transcoded on the
// loading job to BC7, ASTC 4x4 or ETC2, or to RGBA8 where the device has none of them.
namespace quench::assets {

// ETC2: RGB8 (8 bytes per block, opaque) and RGBA8 (16 bytes: EAC alpha, then the ETC2 color block); only Basis
// Universal files are transcoded to them.
enum class PixelFormat : std::uint8_t {
    Rgba8,
    Rgba8Srgb,
    Bc7,
    Bc7Srgb,
    Astc4x4,
    Astc4x4Srgb,
    Etc2Rgb8,
    Etc2Rgb8Srgb,
    Etc2Rgba8,
    Etc2Rgba8Srgb,
};

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

// ------------------------------------------------------------------ Basis Universal
// KTX2 files of one 2D image in ETC1S (BasisLZ supercompression: small files, lower quality) or UASTC LDR 4x4
// (optionally with Zstandard supercompression: about the quality of BC7), transcoded with Basis Universal v2.50.

enum class BasisCodec : std::uint8_t { Etc1s, Uastc };
const char* basisCodecName(BasisCodec codec); // "etc1s", "uastc"

struct BasisInfo {
    BasisCodec codec = BasisCodec::Etc1s;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipLevels = 0;
    bool srgb = false;  // the file's transfer function
    bool alpha = false; // the file has an alpha channel (otherwise it is opaque)
    bool zstd = false;  // UASTC with Zstandard supercompression
};

// The formats a device can sample, as quench::gpu reports them (gpu::Caps; quench_assets has no GPU dependency).
struct TextureSupport {
    bool bc7 = false;
    bool astc4x4 = false;
    bool etc2 = false;
};

// A KTX2 file with Basis Universal data (by its header: no format, ETC1S or UASTC). Says nothing about plain files.
bool isBasisKtx2(std::span<const std::uint8_t> file);
// Checks the header, the levels and the codec (one 2D image, one layer, one face; ETC1S or UASTC LDR 4x4).
Result<BasisInfo> readBasisInfo(std::span<const std::uint8_t> file);
// What a file of `info` is transcoded to on a device with `support`, the first the device samples of:
//   UASTC: ASTC 4x4 (exact, UASTC is a subset of ASTC), BC7, ETC2 (RGB8 when opaque, RGBA8 with alpha; only about
//          ETC1S's quality);
//   ETC1S: ETC2 (RGB8 is exact), ASTC 4x4, BC7 (the same quality; ASTC transcodes faster);
// and RGBA8 when it samples none of them or when level 0 is not a multiple of 4 on both sides (Metal's rule for
// block formats). The sRGB variant when the file is sRGB.
PixelFormat basisTarget(const BasisInfo& info, const TextureSupport& support);
// Every level (after dropping skipLevels of the largest, as readKtx2) transcoded to `target`: one of the formats
// basisTarget returns, or RGBA8 for any file. Thread-safe; a file of 1024 x 1024 with mips takes milliseconds.
Result<TextureImage> transcodeBasis(std::span<const std::uint8_t> file, PixelFormat target,
                                    std::uint32_t skipLevels = 0);

// A KTX2 file made ready for a device with `support`: a plain file as readKtx2 reads it, BC7 decoded to RGBA8 where
// the device cannot sample BC7 (ASTC 4x4 there is an error: the runtime has no ASTC decoder); a Basis Universal file
// transcoded to basisTarget().
struct PreparedTexture {
    TextureImage image;
    std::optional<BasisCodec> basis; // the file's codec when it was a Basis Universal file
    bool decodedOnCpu = false;       // RGBA8 that the file did not hold: a block format the device cannot sample
};
Result<PreparedTexture> prepareTexture(std::span<const std::uint8_t> file, const TextureSupport& support,
                                       std::uint32_t skipLevels = 0);

} // namespace quench::assets
