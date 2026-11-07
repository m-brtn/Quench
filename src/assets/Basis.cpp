// Basis Universal KTX2 files: header checks, the transcode target by device support, transcoding (Basis Universal's
// ktx2_transcoder, cmake/QuenchBasisu.cmake), and prepareTexture for plain and Basis Universal files alike.
#include <quench/assets/Texture.h>

#include "Ktx2Layout.h"

#include <basisu_transcoder.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <mutex>

namespace quench::assets {

using namespace detail::ktx2;

namespace {

// Khronos data format descriptor color models of Basis Universal's codecs (khr_df.h).
constexpr std::uint32_t kModelEtc1s = 163;
constexpr std::uint32_t kModelUastc = 166;
constexpr std::uint32_t kSchemeBasisLz = 1;
constexpr std::uint32_t kSchemeZstd = 2;

void initTranscoder() {
    static std::once_flag once;
    std::call_once(once, [] { basist::basisu_transcoder_init(); });
}

PixelFormat withSrgb(PixelFormat format, bool srgb) {
    switch (format) {
    case PixelFormat::Rgba8:
    case PixelFormat::Rgba8Srgb: return srgb ? PixelFormat::Rgba8Srgb : PixelFormat::Rgba8;
    case PixelFormat::Bc7:
    case PixelFormat::Bc7Srgb: return srgb ? PixelFormat::Bc7Srgb : PixelFormat::Bc7;
    case PixelFormat::Astc4x4:
    case PixelFormat::Astc4x4Srgb: return srgb ? PixelFormat::Astc4x4Srgb : PixelFormat::Astc4x4;
    case PixelFormat::Etc2Rgb8:
    case PixelFormat::Etc2Rgb8Srgb: return srgb ? PixelFormat::Etc2Rgb8Srgb : PixelFormat::Etc2Rgb8;
    case PixelFormat::Etc2Rgba8:
    case PixelFormat::Etc2Rgba8Srgb: return srgb ? PixelFormat::Etc2Rgba8Srgb : PixelFormat::Etc2Rgba8;
    }
    return format;
}

// ETC2 RGB8 is ETC1S's own block format (ETC1 blocks are ETC2 RGB8 blocks); ETC2 RGBA8 puts an EAC alpha block before
// it, as the formats of the same names in Vulkan and Metal do.
basist::transcoder_texture_format transcoderFormat(PixelFormat format) {
    switch (withSrgb(format, false)) {
    case PixelFormat::Bc7: return basist::transcoder_texture_format::cTFBC7_RGBA;
    case PixelFormat::Astc4x4: return basist::transcoder_texture_format::cTFASTC_LDR_4x4_RGBA;
    case PixelFormat::Etc2Rgb8: return basist::transcoder_texture_format::cTFETC1_RGB;
    case PixelFormat::Etc2Rgba8: return basist::transcoder_texture_format::cTFETC2_RGBA;
    default: return basist::transcoder_texture_format::cTFRGBA32;
    }
}

// The transcoder, initialized and checked: one 2D image of ETC1S or UASTC LDR 4x4.
Result<BasisInfo> open(basist::ktx2_transcoder& transcoder, std::span<const std::uint8_t> file) {
    if (!isBasisKtx2(file)) return Error{"not a Basis Universal KTX2 file"};
    if (file.size() > 0xffffffffu) return Error{std::format("Basis Universal file of {} bytes", file.size())};
    initTranscoder();
    if (!transcoder.init(file.data(), std::uint32_t(file.size())))
        return Error{"corrupt Basis Universal KTX2 file (header, levels or data format descriptor)"};
    if (!transcoder.is_etc1s() && !transcoder.is_uastc())
        return Error{std::format("Basis Universal format {} is not supported (ETC1S, UASTC LDR 4x4)",
                                 basist::basis_get_tex_format_name(transcoder.get_basis_tex_format()))};
    const basist::ktx2_header& header = transcoder.get_header();
    if (header.m_pixel_depth > 1 || transcoder.get_layers() > 1 || transcoder.get_faces() != 1)
        return Error{"unsupported Basis Universal layout (needs one 2D image, one layer, one face)"};
    BasisInfo info;
    info.codec = transcoder.is_etc1s() ? BasisCodec::Etc1s : BasisCodec::Uastc;
    info.width = transcoder.get_width();
    info.height = transcoder.get_height();
    info.mipLevels = std::max(transcoder.get_levels(), 1u);
    info.srgb = transcoder.is_srgb();
    info.alpha = transcoder.get_has_alpha() != 0;
    info.zstd = header.m_supercompression_scheme == basist::KTX2_SS_ZSTANDARD;
    if (info.width == 0 || info.height == 0 || info.width > kMaxExtent || info.height > kMaxExtent)
        return Error{std::format("Basis Universal file of {} x {}", info.width, info.height)};
    if (info.mipLevels > kMaxLevels ||
        (info.width >> (info.mipLevels - 1) == 0 && info.height >> (info.mipLevels - 1) == 0))
        return Error{
            std::format("Basis Universal file has {} levels for {} x {}", info.mipLevels, info.width, info.height)};
    return info;
}

} // namespace

const char* basisCodecName(BasisCodec codec) { return codec == BasisCodec::Uastc ? "uastc" : "etc1s"; }

bool isBasisKtx2(std::span<const std::uint8_t> file) {
    if (file.size() < kHeaderBytes || std::memcmp(file.data(), kIdentifier, sizeof(kIdentifier)) != 0) return false;
    const std::uint8_t* p = file.data();
    if (get32(p + 12) != 0) return false; // VK_FORMAT_UNDEFINED
    const std::uint32_t scheme = get32(p + 44);
    if (scheme == kSchemeBasisLz) return true;
    const std::uint32_t dfdAt = get32(p + 48), dfdLength = get32(p + 52);
    if (dfdLength < 16 || dfdAt > file.size() || file.size() - dfdAt < 16) return false;
    const std::uint32_t model = p[dfdAt + 12];
    return (scheme == 0 || scheme == kSchemeZstd) && (model == kModelUastc || model == kModelEtc1s);
}

Result<BasisInfo> readBasisInfo(std::span<const std::uint8_t> file) {
    basist::ktx2_transcoder transcoder;
    return open(transcoder, file);
}

PixelFormat basisTarget(const BasisInfo& info, const TextureSupport& support) {
    if (info.width % 4 != 0 || info.height % 4 != 0) return withSrgb(PixelFormat::Rgba8, info.srgb);
    const PixelFormat etc2 = info.alpha ? PixelFormat::Etc2Rgba8 : PixelFormat::Etc2Rgb8;
    PixelFormat target = PixelFormat::Rgba8;
    if (info.codec == BasisCodec::Uastc) {
        target = support.astc4x4 ? PixelFormat::Astc4x4
                 : support.bc7   ? PixelFormat::Bc7
                 : support.etc2  ? etc2
                                 : PixelFormat::Rgba8;
    } else {
        target = support.etc2      ? etc2
                 : support.astc4x4 ? PixelFormat::Astc4x4
                 : support.bc7     ? PixelFormat::Bc7
                                   : PixelFormat::Rgba8;
    }
    return withSrgb(target, info.srgb);
}

Result<TextureImage> transcodeBasis(std::span<const std::uint8_t> file, PixelFormat target, std::uint32_t skipLevels) {
    basist::ktx2_transcoder transcoder;
    Result<BasisInfo> info = open(transcoder, file);
    if (!info) return Error{info.error()};
    const PixelFormatInfo& targetInfo = pixelFormatInfo(target);
    if (targetInfo.blockSize > 1 && (info->width % 4 != 0 || info->height % 4 != 0))
        return Error{std::format("Basis Universal file of {} x {}: {} needs multiples of 4", info->width, info->height,
                                 targetInfo.name)};
    if (!transcoder.start_transcoding()) return Error{"corrupt Basis Universal data (codebooks or supercompression)"};

    const std::uint32_t skip = std::min(skipLevels, info->mipLevels - 1);
    TextureImage image;
    image.format = withSrgb(target, info->srgb);
    image.width = std::max(info->width >> skip, 1u);
    image.height = std::max(info->height >> skip, 1u);
    image.mipLevels = info->mipLevels - skip;
    image.data.resize(std::size_t(image.levelOffset(image.mipLevels)));
    const basist::transcoder_texture_format format = transcoderFormat(target);
    const bool rgba = targetInfo.blockSize == 1;
    basist::ktx2_transcoder_state state;
    state.clear(); // it has no constructor: its cached level index starts as garbage
    for (std::uint32_t level = 0; level < image.mipLevels; ++level) {
        basist::ktx2_image_level_info levelInfo;
        if (!transcoder.get_image_level_info(levelInfo, level + skip, 0, 0) ||
            levelInfo.m_orig_width != image.levelWidth(level) || levelInfo.m_orig_height != image.levelHeight(level))
            return Error{std::format("Basis Universal level {} has an unexpected size", level + skip)};
        const std::uint32_t units = rgba ? levelInfo.m_orig_width * levelInfo.m_orig_height : levelInfo.m_total_blocks;
        std::uint8_t* out = image.data.data() + image.levelOffset(level);
        if (!transcoder.transcode_image_level(level + skip, 0, 0, out, units, format, 0, 0, 0, -1, -1, &state))
            return Error{std::format("transcoding level {} of a Basis Universal {} file to {} failed", level + skip,
                                     basisCodecName(info->codec), targetInfo.name)};
    }
    return image;
}

Result<PreparedTexture> prepareTexture(std::span<const std::uint8_t> file, const TextureSupport& support,
                                       std::uint32_t skipLevels) {
    PreparedTexture prepared;
    if (isBasisKtx2(file)) {
        Result<BasisInfo> info = readBasisInfo(file);
        if (!info) return Error{info.error()};
        const PixelFormat target = basisTarget(*info, support);
        Result<TextureImage> image = transcodeBasis(file, target, skipLevels);
        if (!image) return Error{image.error()};
        prepared.image = std::move(*image);
        prepared.basis = info->codec;
        prepared.decodedOnCpu = pixelFormatInfo(target).blockSize == 1;
        return prepared;
    }
    Result<TextureImage> image = readKtx2(file, skipLevels);
    if (!image) return Error{image.error()};
    const PixelFormat format = image->format;
    if ((format == PixelFormat::Bc7 || format == PixelFormat::Bc7Srgb) && !support.bc7) {
        image = decodeBc7(*image);
        if (!image) return Error{image.error()};
        prepared.decodedOnCpu = true;
    } else if ((format == PixelFormat::Astc4x4 || format == PixelFormat::Astc4x4Srgb) && !support.astc4x4) {
        return Error{"the device cannot sample ASTC 4x4 and the runtime has no ASTC decoder (cook BC7, RGBA8 or Basis "
                     "Universal for it)"};
    } else if (format >= PixelFormat::Etc2Rgb8 && !support.etc2) {
        return Error{std::format("the device cannot sample {} (cook BC7, RGBA8 or Basis Universal for it)",
                                 pixelFormatInfo(format).name)};
    }
    prepared.image = std::move(*image);
    return prepared;
}

} // namespace quench::assets
