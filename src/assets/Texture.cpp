// KTX2 reading and writing generalized from TrailForge's src/core/Ktx2.cpp (BC7 only there; here RGBA8, BC7, ASTC 4x4
// and ETC2, errors as values instead of exceptions). Basis Universal files: Basis.cpp.
#include <quench/assets/Texture.h>

#include "Ktx2Layout.h"

#include <bc7decomp.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <string>

namespace quench::assets {

using namespace detail::ktx2;

namespace {

// Khronos data format descriptor values (khr_df.h).
constexpr std::uint32_t kModelRgbsda = 1;
constexpr std::uint32_t kModelBc7 = 136;
constexpr std::uint32_t kModelEtc2 = 161;
constexpr std::uint32_t kModelAstc = 162;
constexpr std::uint32_t kPrimariesBt709 = 1;
constexpr std::uint32_t kTransferLinear = 1;
constexpr std::uint32_t kTransferSrgb = 2;
constexpr std::uint32_t kChannelEtc2Color = 2;
constexpr std::uint32_t kChannelAlpha = 15;
constexpr std::uint32_t kQualifierLinear = 1u << 4;

constexpr std::array<PixelFormatInfo, 10> kFormats{{
    {"rgba8", 37, 1, 4, false},
    {"rgba8_srgb", 43, 1, 4, true},
    {"bc7", 145, 4, 16, false},
    {"bc7_srgb", 146, 4, 16, true},
    {"astc4x4", 157, 4, 16, false},
    {"astc4x4_srgb", 158, 4, 16, true},
    {"etc2_rgb8", 147, 4, 8, false},
    {"etc2_rgb8_srgb", 148, 4, 8, true},
    {"etc2_rgba8", 151, 4, 16, false},
    {"etc2_rgba8_srgb", 152, 4, 16, true},
}};

void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(std::uint8_t(v >> (8 * i)));
}
void put64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(std::uint8_t(v >> (8 * i)));
}
void set32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + std::size_t(i)] = std::uint8_t(v >> (8 * i));
}
void set64(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[at + std::size_t(i)] = std::uint8_t(v >> (8 * i));
}
void pad(std::vector<std::uint8_t>& b, std::size_t align) {
    while (b.size() % align != 0) b.push_back(0);
}

const PixelFormatInfo* findVkFormat(std::uint32_t vkFormat) {
    for (const PixelFormatInfo& info : kFormats)
        if (info.vkFormat == vkFormat) return &info;
    return nullptr;
}

PixelFormat formatOf(const PixelFormatInfo& info) { return PixelFormat(&info - kFormats.data()); }

// The basic data format descriptor block of `format`: one sample per channel for RGBA8, one 128-bit sample for BC7
// and ASTC, one 64-bit sample per block half for ETC2 (EAC alpha, then the color block).
void writeDfd(std::vector<std::uint8_t>& b, PixelFormat format) {
    const PixelFormatInfo& info = pixelFormatInfo(format);
    const bool rgba = info.blockSize == 1;
    const bool etc2 = format >= PixelFormat::Etc2Rgb8;
    const std::uint32_t samples = rgba ? 4 : etc2 ? info.blockBytes / 8 : 1;
    const std::uint32_t blockSize = 24 + 16 * samples;
    const std::uint32_t model = rgba                                                             ? kModelRgbsda
                                : (format == PixelFormat::Bc7 || format == PixelFormat::Bc7Srgb) ? kModelBc7
                                : etc2                                                           ? kModelEtc2
                                                                                                 : kModelAstc;
    put32(b, 4 + blockSize); // dfdTotalSize
    put32(b, 0);             // vendorId 0 (Khronos), descriptorType 0 (basic)
    put32(b, 2u | (blockSize << 16));
    put32(b, model | (kPrimariesBt709 << 8) | ((info.srgb ? kTransferSrgb : kTransferLinear) << 16));
    const std::uint32_t dim = std::uint32_t(info.blockSize - 1);
    put32(b, dim | (dim << 8)); // texel block dimensions - 1
    put32(b, info.blockBytes);  // bytesPlane0
    put32(b, 0);                // bytesPlane4..7
    if (rgba) {
        for (std::uint32_t c = 0; c < 4; ++c) {
            std::uint32_t channel = c == 3 ? kChannelAlpha : c;
            if (c == 3 && info.srgb) channel |= kQualifierLinear; // alpha is linear in an sRGB format
            put32(b, (c * 8) | (7u << 16) | (channel << 24));
            put32(b, 0);   // sample position
            put32(b, 0);   // sampleLower
            put32(b, 255); // sampleUpper
        }
    } else if (etc2) {
        for (std::uint32_t s = 0; s < samples; ++s) {
            std::uint32_t channel = s + 1 < samples ? kChannelAlpha : kChannelEtc2Color;
            if (channel == kChannelAlpha && info.srgb) channel |= kQualifierLinear;
            put32(b, (s * 64) | (63u << 16) | (channel << 24));
            put32(b, 0);
            put32(b, 0);
            put32(b, 0xFFFFFFFFu);
        }
    } else {
        put32(b, 0u | (127u << 16)); // bitOffset 0, bitLength 128 - 1, channel 0 (BC7 color, ASTC data)
        put32(b, 0);
        put32(b, 0);
        put32(b, 0xFFFFFFFFu);
    }
}

} // namespace

const PixelFormatInfo& pixelFormatInfo(PixelFormat format) { return kFormats[std::size_t(format)]; }

std::uint64_t levelBytes(PixelFormat format, std::uint32_t width, std::uint32_t height) {
    const PixelFormatInfo& info = pixelFormatInfo(format);
    const std::uint64_t bw = (std::uint64_t(width) + info.blockSize - 1) / info.blockSize;
    const std::uint64_t bh = (std::uint64_t(height) + info.blockSize - 1) / info.blockSize;
    return bw * bh * info.blockBytes;
}

std::uint32_t TextureImage::levelWidth(std::uint32_t level) const { return std::max(width >> level, 1u); }
std::uint32_t TextureImage::levelHeight(std::uint32_t level) const { return std::max(height >> level, 1u); }

std::uint64_t TextureImage::levelOffset(std::uint32_t level) const {
    std::uint64_t offset = 0;
    for (std::uint32_t i = 0; i < level; ++i) offset += levelBytes(format, levelWidth(i), levelHeight(i));
    return offset;
}

std::span<const std::uint8_t> TextureImage::level(std::uint32_t index) const {
    const std::uint64_t offset = levelOffset(index);
    const std::uint64_t size = levelBytes(format, levelWidth(index), levelHeight(index));
    QUENCH_ASSERT(index < mipLevels && offset + size <= data.size(), "TextureImage::level out of range");
    return std::span(data).subspan(std::size_t(offset), std::size_t(size));
}

Result<TextureImage> readKtx2(std::span<const std::uint8_t> file, std::uint32_t skipLevels) {
    const std::uint8_t* p = file.data();
    if (file.size() < kHeaderBytes || std::memcmp(p, kIdentifier, sizeof(kIdentifier)) != 0)
        return Error{"not a KTX2 file"};
    const std::uint32_t vkFormat = get32(p + 12);
    const std::uint32_t typeSize = get32(p + 16);
    const std::uint32_t width = get32(p + 20);
    const std::uint32_t height = get32(p + 24);
    const std::uint32_t depth = get32(p + 28), layers = get32(p + 32), faces = get32(p + 36);
    const std::uint32_t levelCount = std::max(get32(p + 40), 1u);
    const std::uint32_t scheme = get32(p + 44);
    if (isBasisKtx2(file)) return Error{"a Basis Universal KTX2 file (read it with prepareTexture or transcodeBasis)"};
    const PixelFormatInfo* info = findVkFormat(vkFormat);
    if (!info) return Error{std::format("KTX2 format {} is not supported (RGBA8, BC7, ASTC 4x4, ETC2)", vkFormat)};
    if (depth != 0 || layers > 1 || faces != 1 || scheme != 0 || width == 0 || height == 0 || typeSize != 1)
        return Error{"unsupported KTX2 layout (needs one 2D image, one layer, one face, no supercompression)"};
    if (width > kMaxExtent || height > kMaxExtent)
        return Error{std::format("KTX2 of {} x {} is larger than {} x {}", width, height, kMaxExtent, kMaxExtent)};
    if (levelCount > kMaxLevels || file.size() < kHeaderBytes + levelCount * kLevelEntryBytes)
        return Error{std::format("bad KTX2 level count {}", levelCount)};
    if (width >> (levelCount - 1) == 0 && height >> (levelCount - 1) == 0)
        return Error{std::format("KTX2 has {} levels for {} x {}", levelCount, width, height)};

    const PixelFormat format = formatOf(*info);
    const std::uint32_t skip = std::min(skipLevels, levelCount - 1);
    TextureImage image;
    image.format = format;
    image.width = std::max(width >> skip, 1u);
    image.height = std::max(height >> skip, 1u);
    image.mipLevels = levelCount - skip;
    std::uint64_t total = 0;
    for (std::uint32_t i = skip; i < levelCount; ++i)
        total += levelBytes(format, std::max(width >> i, 1u), std::max(height >> i, 1u));
    // The levels lie inside the file: a larger total is a corrupt size, and reserving it could take gigabytes.
    if (total > file.size())
        return Error{std::format("KTX2 of {} x {} needs {} bytes, the file has {}", width, height, total, file.size())};
    image.data.reserve(std::size_t(total));
    for (std::uint32_t i = skip; i < levelCount; ++i) {
        const std::uint8_t* e = p + kHeaderBytes + i * kLevelEntryBytes;
        const std::uint64_t offset = get64(e), length = get64(e + 8);
        const std::uint64_t expected = levelBytes(format, std::max(width >> i, 1u), std::max(height >> i, 1u));
        if (offset > file.size() || length > file.size() - offset || length != expected)
            return Error{std::format("KTX2 level {} is out of range or {} bytes instead of {}", i, length, expected)};
        image.data.insert(image.data.end(), p + offset, p + offset + length);
    }
    return image;
}

Result<std::vector<std::uint8_t>> writeKtx2(const TextureImage& image, std::string_view writer) {
    if (image.width == 0 || image.height == 0 || image.mipLevels == 0 || image.mipLevels > kMaxLevels)
        return Error{std::format("KTX2: {} x {} with {} levels", image.width, image.height, image.mipLevels)};
    if (image.levelOffset(image.mipLevels) != image.data.size())
        return Error{std::format("KTX2: {} bytes of data, the levels need {}", image.data.size(),
                                 image.levelOffset(image.mipLevels))};
    const PixelFormatInfo& info = pixelFormatInfo(image.format);
    const std::uint32_t levels = image.mipLevels;
    std::vector<std::uint8_t> b(kIdentifier, kIdentifier + sizeof(kIdentifier));
    put32(b, info.vkFormat);
    put32(b, 1); // typeSize
    put32(b, image.width);
    put32(b, image.height);
    put32(b, 0); // pixelDepth
    put32(b, 0); // layerCount (not an array)
    put32(b, 1); // faceCount
    put32(b, levels);
    put32(b, 0); // supercompressionScheme
    const std::size_t indexAt = b.size();
    for (int i = 0; i < 4; ++i) put32(b, 0); // dfd offset / length, kvd offset / length
    put64(b, 0);                             // sgd offset
    put64(b, 0);                             // sgd length
    const std::size_t levelIndexAt = b.size();
    b.resize(b.size() + levels * kLevelEntryBytes, 0);

    const std::uint32_t dfdAt = std::uint32_t(b.size());
    writeDfd(b, image.format);
    const std::uint32_t dfdLength = std::uint32_t(b.size()) - dfdAt;

    // Key / value data: the writer name.
    const std::uint32_t kvdAt = std::uint32_t(b.size());
    const std::string_view key = "KTXwriter";
    put32(b, std::uint32_t(key.size() + 1 + writer.size() + 1));
    b.insert(b.end(), key.begin(), key.end());
    b.push_back(0);
    b.insert(b.end(), writer.begin(), writer.end());
    b.push_back(0);
    pad(b, 4);
    const std::uint32_t kvdLength = std::uint32_t(b.size()) - kvdAt;
    set32(b, indexAt, dfdAt);
    set32(b, indexAt + 4, dfdLength);
    set32(b, indexAt + 8, kvdAt);
    set32(b, indexAt + 12, kvdLength);

    // Level data, smallest level first, each aligned to lcm(texel block bytes, 4) (16 for every format here).
    for (std::uint32_t i = levels; i-- > 0;) {
        pad(b, 16);
        const std::size_t at = b.size();
        const std::span<const std::uint8_t> level = image.level(i);
        b.insert(b.end(), level.begin(), level.end());
        set64(b, levelIndexAt + i * kLevelEntryBytes, at);
        set64(b, levelIndexAt + i * kLevelEntryBytes + 8, level.size());
        set64(b, levelIndexAt + i * kLevelEntryBytes + 16, level.size());
    }
    return b;
}

Result<TextureImage> decodeBc7(const TextureImage& image) {
    if (image.format != PixelFormat::Bc7 && image.format != PixelFormat::Bc7Srgb)
        return Error{std::format("decodeBc7: the image is {}", pixelFormatInfo(image.format).name)};
    TextureImage out;
    out.format = image.format == PixelFormat::Bc7Srgb ? PixelFormat::Rgba8Srgb : PixelFormat::Rgba8;
    out.width = image.width;
    out.height = image.height;
    out.mipLevels = image.mipLevels;
    out.data.resize(std::size_t(out.levelOffset(out.mipLevels)));
    for (std::uint32_t level = 0; level < image.mipLevels; ++level) {
        const std::uint32_t w = image.levelWidth(level), h = image.levelHeight(level);
        const std::uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        const std::span<const std::uint8_t> blocks = image.level(level);
        std::uint8_t* dst = out.data.data() + out.levelOffset(level);
        bc7decomp::color_rgba texels[16];
        for (std::uint32_t by = 0; by < bh; ++by)
            for (std::uint32_t bx = 0; bx < bw; ++bx) {
                if (!bc7decomp::unpack_bc7(blocks.data() + (std::size_t(by) * bw + bx) * 16, texels))
                    std::memset(texels, 0, sizeof(texels));
                for (std::uint32_t y = 0; y < 4 && by * 4 + y < h; ++y)
                    for (std::uint32_t x = 0; x < 4 && bx * 4 + x < w; ++x)
                        std::memcpy(dst + (std::size_t(by * 4 + y) * w + bx * 4 + x) * 4, &texels[y * 4 + x], 4);
            }
    }
    return out;
}

} // namespace quench::assets
