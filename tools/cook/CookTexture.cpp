#include "Cook.h"

#include <astcenc.h>
#include <bc7enc.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <shared_mutex>

namespace quench::cook {
namespace {

using assets::PixelFormat;
using assets::TextureImage;

constexpr std::uint32_t kMaxSide = 16384;
constexpr std::array<std::uint8_t, 12> kKtx2Magic{0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};

bool isKtx2(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= kKtx2Magic.size() && std::equal(kKtx2Magic.begin(), kKtx2Magic.end(), bytes.begin());
}

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float linearToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

std::uint8_t quantize(float v) { return std::uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

// RGBA as floats; the color channels in linear light when the image is sRGB, so that filtering averages light.
struct Plane {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> texels;
};

Plane toPlane(const Image& image, bool srgb) {
    std::array<float, 256> table{};
    for (int i = 0; i < 256; ++i) table[std::size_t(i)] = float(i) / 255.0f;
    std::array<float, 256> linear = table;
    if (srgb)
        for (float& v : linear) v = srgbToLinear(v);
    Plane p{image.width, image.height, std::vector<float>(image.rgba.size())};
    for (std::size_t i = 0; i < image.rgba.size(); ++i) p.texels[i] = (i % 4 < 3 ? linear : table)[image.rgba[i]];
    return p;
}

Image toImage(const Plane& p, bool srgb) {
    Image image{p.width, p.height, std::vector<std::uint8_t>(p.texels.size())};
    for (std::size_t i = 0; i < p.texels.size(); ++i)
        image.rgba[i] = quantize(srgb && i % 4 < 3 ? linearToSrgb(p.texels[i]) : p.texels[i]);
    return image;
}

// A 2 x 2 box filter; an odd last row or column is folded into the texels before it (the edge is clamped).
Plane half(const Plane& p) {
    Plane h{std::max(p.width / 2, 1u), std::max(p.height / 2, 1u), {}};
    h.texels.resize(std::size_t(h.width) * h.height * 4);
    for (std::uint32_t y = 0; y < h.height; ++y)
        for (std::uint32_t x = 0; x < h.width; ++x) {
            const std::uint32_t x0 = std::min(2 * x, p.width - 1), x1 = std::min(2 * x + 1, p.width - 1);
            const std::uint32_t y0 = std::min(2 * y, p.height - 1), y1 = std::min(2 * y + 1, p.height - 1);
            for (std::uint32_t c = 0; c < 4; ++c) {
                const auto at = [&](std::uint32_t sx, std::uint32_t sy) {
                    return p.texels[(std::size_t(sy) * p.width + sx) * 4 + c];
                };
                h.texels[(std::size_t(y) * h.width + x) * 4 + c] =
                    0.25f * (at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1));
            }
        }
    return h;
}

void note(std::vector<std::string>* notes, std::string text) {
    if (notes) notes->push_back(std::move(text));
}

constexpr astcenc_swizzle kRgba{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

// In 5.7.1 astcenc_context_alloc rewrites the encoder's global angular tables, which every compressing context reads:
// contexts are allocated under the exclusive lock and compress under the shared one.
std::shared_mutex g_astcTables;

// astc-encoder contexts of one configuration: a context compresses one image at a time, so each job takes one of its
// own and gives it back. Each is allocated from the configuration, not from a parent context: in 5.7.1
// astcenc_context_alloc converts the quality limit of a parent's configuration, which is already converted, again.
class AstcContexts {
public:
    explicit AstcContexts(bool srgb) {
        m_status = astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_MEDIUM, 0,
                                       &m_config);
    }
    ~AstcContexts() {
        for (astcenc_context* context : m_all) astcenc_context_free(context);
    }
    AstcContexts(const AstcContexts&) = delete;
    AstcContexts& operator=(const AstcContexts&) = delete;

    astcenc_context* take(astcenc_error& status) {
        std::lock_guard lock(m_mutex);
        status = m_status;
        if (!m_free.empty()) {
            astcenc_context* context = m_free.back();
            m_free.pop_back();
            return context;
        }
        astcenc_context* context = nullptr;
        if (status == ASTCENC_SUCCESS) {
            std::unique_lock tables(g_astcTables);
            status = astcenc_context_alloc(&m_config, 1, &context, nullptr);
        }
        if (status != ASTCENC_SUCCESS) return nullptr;
        m_all.push_back(context);
        return context;
    }
    void give(astcenc_context* context) {
        std::lock_guard lock(m_mutex);
        m_free.push_back(context);
    }

private:
    astcenc_config m_config{};
    astcenc_error m_status = ASTCENC_SUCCESS;
    std::mutex m_mutex;
    std::vector<astcenc_context*> m_all, m_free;
};

// Strips of whole block rows, each compressed on its own: ASTC blocks do not depend on each other, so the output is
// the same for any split.
Result<std::vector<std::uint8_t>> encodeAstcWith(AstcContexts& contexts, const Image& image, JobSystem& jobs) {
    const std::uint32_t blocksX = (image.width + 3) / 4, blocksY = (image.height + 3) / 4;
    std::vector<std::uint8_t> out(std::size_t(blocksX) * blocksY * 16);
    std::atomic<int> failure{ASTCENC_SUCCESS};
    const Job job = jobs.parallelFor(blocksY, 1, [&](std::uint32_t begin, std::uint32_t end) {
        astcenc_error status = ASTCENC_SUCCESS;
        astcenc_context* context = contexts.take(status);
        if (context) {
            const std::uint32_t y0 = begin * 4, y1 = std::min(end * 4, image.height);
            void* slice = const_cast<std::uint8_t*>(&image.rgba[std::size_t(y0) * image.width * 4]);
            astcenc_image strip{image.width, y1 - y0, 1, ASTCENC_TYPE_U8, &slice};
            {
                std::shared_lock tables(g_astcTables);
                status = astcenc_compress_image(context, &strip, &kRgba, &out[std::size_t(begin) * blocksX * 16],
                                                std::size_t(end - begin) * blocksX * 16, 0);
            }
            astcenc_compress_reset(context);
            contexts.give(context);
        }
        if (status != ASTCENC_SUCCESS) failure = int(status);
    });
    jobs.wait(job);
    if (failure != ASTCENC_SUCCESS)
        return Error{
            std::format("cook: ASTC encoding failed ({})", astcenc_get_error_string(astcenc_error(failure.load())))};
    return out;
}

} // namespace

Result<Image> decodeImage(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > std::size_t(INT_MAX)) return Error{"cook: empty or too large image file"};
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &width, &height, &channels, 4);
    if (!pixels) return Error{std::format("cook: cannot decode the image ({})", stbi_failure_reason())};
    Image image{std::uint32_t(width), std::uint32_t(height), {}};
    image.rgba.assign(pixels, pixels + std::size_t(width) * std::size_t(height) * 4);
    stbi_image_free(pixels);
    if (image.width > kMaxSide || image.height > kMaxSide)
        return Error{std::format("cook: image of {} x {} is larger than {}", width, height, kMaxSide)};
    return image;
}

std::vector<std::uint8_t> encodeBc7(const Image& image, bool perceptual, JobSystem& jobs) {
    static std::once_flag once;
    std::call_once(once, [] { bc7enc_compress_block_init(); });
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    if (!perceptual) bc7enc_compress_block_params_init_linear_weights(&params);
    params.m_uber_level = 1;
    params.m_max_partitions = BC7ENC_MAX_PARTITIONS;

    const std::uint32_t blocksX = (image.width + 3) / 4, blocksY = (image.height + 3) / 4;
    std::vector<std::uint8_t> out(std::size_t(blocksX) * blocksY * 16);
    const Job job = jobs.parallelFor(blocksY, 1, [&](std::uint32_t begin, std::uint32_t end) {
        std::array<std::uint8_t, 64> block{};
        for (std::uint32_t by = begin; by < end; ++by)
            for (std::uint32_t bx = 0; bx < blocksX; ++bx) {
                for (std::uint32_t py = 0; py < 4; ++py)
                    for (std::uint32_t px = 0; px < 4; ++px) {
                        const std::uint32_t sx = std::min(bx * 4 + px, image.width - 1);
                        const std::uint32_t sy = std::min(by * 4 + py, image.height - 1);
                        std::memcpy(&block[(py * 4 + px) * 4], &image.rgba[(std::size_t(sy) * image.width + sx) * 4],
                                    4);
                    }
                bc7enc_compress_block(&out[(std::size_t(by) * blocksX + bx) * 16], block.data(), &params);
            }
    });
    jobs.wait(job);
    return out;
}

Result<std::vector<std::uint8_t>> encodeAstc(const Image& image, bool srgb, JobSystem& jobs) {
    if (image.width == 0 || image.height == 0 || image.rgba.size() != std::size_t(image.width) * image.height * 4)
        return Error{"cook: empty or inconsistent image"};
    AstcContexts contexts(srgb);
    return encodeAstcWith(contexts, image, jobs);
}

Result<Image> decodeAstc(const TextureImage& texture, std::uint32_t level) {
    const bool srgb = texture.format == PixelFormat::Astc4x4Srgb;
    if (!srgb && texture.format != PixelFormat::Astc4x4) return Error{"cook: not an ASTC 4x4 texture"};
    if (level >= texture.mipLevels) return Error{std::format("cook: no level {}", level)};
    astcenc_config config{};
    astcenc_error status = astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, 4, 4, 1,
                                               ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY, &config);
    astcenc_context* context = nullptr;
    if (status == ASTCENC_SUCCESS) status = astcenc_context_alloc(&config, 1, &context, nullptr);
    if (status != ASTCENC_SUCCESS)
        return Error{std::format("cook: ASTC decoder: {}", astcenc_get_error_string(status))};
    Image image{std::max(texture.width >> level, 1u), std::max(texture.height >> level, 1u), {}};
    image.rgba.resize(std::size_t(image.width) * image.height * 4);
    void* slice = image.rgba.data();
    astcenc_image out{image.width, image.height, 1, ASTCENC_TYPE_U8, &slice};
    const std::span<const std::uint8_t> blocks = texture.level(level);
    status = astcenc_decompress_image(context, blocks.data(), blocks.size(), &out, &kRgba, 0);
    astcenc_context_free(context);
    if (status != ASTCENC_SUCCESS)
        return Error{std::format("cook: ASTC decoding: {}", astcenc_get_error_string(status))};
    return image;
}

namespace {

// Level 0 (halved to options.maxSize) and, with options.mips, the chain below it.
Result<std::vector<Image>> makeLevels(const Image& image, const TextureOptions& options) {
    if (image.width == 0 || image.height == 0 || image.rgba.size() != std::size_t(image.width) * image.height * 4)
        return Error{"cook: empty or inconsistent image"};
    if (image.width > kMaxSide || image.height > kMaxSide)
        return Error{std::format("cook: image of {} x {} is larger than {}", image.width, image.height, kMaxSide)};

    Plane plane = toPlane(image, options.srgb);
    bool resized = false;
    while (options.maxSize > 0 && (plane.width > options.maxSize || plane.height > options.maxSize)) {
        plane = half(plane);
        resized = true;
    }
    std::vector<Image> levels;
    levels.push_back(resized ? toImage(plane, options.srgb) : image);
    if (options.mips)
        while (plane.width > 1 || plane.height > 1) {
            plane = half(plane);
            levels.push_back(toImage(plane, options.srgb));
        }
    return levels;
}

// Level 0 of a BC7 or ASTC 4x4 file as RGBA8 (Basis Universal encodes from it again).
Result<Image> decodeLevel0(const TextureImage& texture) {
    if (texture.format == PixelFormat::Astc4x4 || texture.format == PixelFormat::Astc4x4Srgb)
        return decodeAstc(texture, 0);
    TextureImage first = texture;
    first.mipLevels = 1;
    first.data.resize(std::size_t(assets::levelBytes(texture.format, texture.width, texture.height)));
    Result<TextureImage> rgba = assets::decodeBc7(first);
    if (!rgba) return Error{rgba.error()};
    return Image{rgba->width, rgba->height, std::move(rgba->data)};
}

} // namespace

assets::BasisCodec basisCodec(const TextureOptions& options) {
    switch (options.basis) {
    case BasisMode::Etc1s: return assets::BasisCodec::Etc1s;
    case BasisMode::Uastc: return assets::BasisCodec::Uastc;
    case BasisMode::Auto: break;
    }
    return options.srgb ? assets::BasisCodec::Etc1s : assets::BasisCodec::Uastc;
}

Result<TextureImage> cookImage(const Image& image, const TextureOptions& options, JobSystem& jobs,
                               std::vector<std::string>* notes) {
    if (options.format == TextureFormat::Basis) return Error{"cook: Basis Universal files come from cookImageFile"};
    Result<std::vector<Image>> made = makeLevels(image, options);
    if (!made) return Error{made.error()};
    const std::vector<Image>& levels = *made;

    TextureFormat format = options.format;
    if (format != TextureFormat::Rgba8 && (levels[0].width % 4 != 0 || levels[0].height % 4 != 0)) {
        note(notes, std::format("{} x {} is not a multiple of 4: RGBA8 instead of {}", levels[0].width,
                                levels[0].height, format == TextureFormat::Bc7 ? "BC7" : "ASTC 4x4"));
        format = TextureFormat::Rgba8;
    }
    TextureImage out;
    out.width = levels[0].width;
    out.height = levels[0].height;
    out.mipLevels = std::uint32_t(levels.size());
    if (format == TextureFormat::Bc7)
        out.format = options.srgb ? PixelFormat::Bc7Srgb : PixelFormat::Bc7;
    else if (format == TextureFormat::Astc4x4)
        out.format = options.srgb ? PixelFormat::Astc4x4Srgb : PixelFormat::Astc4x4;
    else
        out.format = options.srgb ? PixelFormat::Rgba8Srgb : PixelFormat::Rgba8;
    std::optional<AstcContexts> astc;
    if (format == TextureFormat::Astc4x4) astc.emplace(options.srgb);
    for (const Image& level : levels) {
        if (format == TextureFormat::Bc7) {
            const std::vector<std::uint8_t> blocks = encodeBc7(level, options.srgb, jobs);
            out.data.insert(out.data.end(), blocks.begin(), blocks.end());
        } else if (format == TextureFormat::Astc4x4) {
            Result<std::vector<std::uint8_t>> blocks = encodeAstcWith(*astc, level, jobs);
            if (!blocks) return Error{blocks.error()};
            out.data.insert(out.data.end(), blocks->begin(), blocks->end());
        } else {
            out.data.insert(out.data.end(), level.rgba.begin(), level.rgba.end());
        }
    }
    return out;
}

Result<TextureImage> cookTexture(std::span<const std::uint8_t> source, const TextureOptions& options, JobSystem& jobs,
                                 std::vector<std::string>* notes) {
    if (options.format == TextureFormat::Basis) return Error{"cook: Basis Universal files come from cookTextureFile"};
    if (!isKtx2(source)) {
        Result<Image> image = decodeImage(source);
        if (!image) return Error{image.error()};
        return cookImage(*image, options, jobs, notes);
    }
    if (assets::isBasisKtx2(source)) {
        // Its own levels, transcoded: what the runtime would make of it on a device that samples the format.
        Result<assets::BasisInfo> info = assets::readBasisInfo(source);
        if (!info) return Error{info.error()};
        PixelFormat target = options.format == TextureFormat::Bc7       ? PixelFormat::Bc7
                             : options.format == TextureFormat::Astc4x4 ? PixelFormat::Astc4x4
                                                                        : PixelFormat::Rgba8;
        if (target != PixelFormat::Rgba8 && (info->width % 4 != 0 || info->height % 4 != 0)) {
            note(notes, std::format("{} x {} is not a multiple of 4: RGBA8 instead of {}", info->width, info->height,
                                    assets::pixelFormatInfo(target).name));
            target = PixelFormat::Rgba8;
        }
        if (info->srgb != options.srgb) note(notes, "a Basis Universal file keeps its own transfer function");
        return assets::transcodeBasis(source, target);
    }
    Result<TextureImage> file = assets::readKtx2(source);
    if (!file) return Error{file.error()};
    const assets::PixelFormatInfo& info = assets::pixelFormatInfo(file->format);
    if (info.blockSize > 1) {
        if (info.srgb != options.srgb)
            note(notes, std::format("a {} KTX2 file keeps its own transfer function", info.name));
        return file;
    }
    // RGBA8: level 0 is the source; the file's sRGB flag decides how it is filtered.
    const std::span<const std::uint8_t> level0 = file->level(0);
    const Image image{file->width, file->height, std::vector<std::uint8_t>(level0.begin(), level0.end())};
    TextureOptions fileOptions = options;
    fileOptions.srgb = info.srgb;
    return cookImage(image, fileOptions, jobs, notes);
}

Result<std::vector<std::uint8_t>> cookImageFile(const Image& image, const TextureOptions& options, JobSystem& jobs,
                                                std::vector<std::string>* notes) {
    if (options.format != TextureFormat::Basis) {
        Result<TextureImage> cooked = cookImage(image, options, jobs, notes);
        if (!cooked) return Error{cooked.error()};
        return assets::writeKtx2(*cooked);
    }
    Result<std::vector<Image>> levels = makeLevels(image, options);
    if (!levels) return Error{levels.error()};
    const Image& first = levels->front();
    if (first.width % 4 != 0 || first.height % 4 != 0)
        note(notes, std::format("{} x {} is not a multiple of 4: devices get RGBA8 from the Basis Universal file",
                                first.width, first.height));
    return encodeBasis(*levels, options.srgb, basisCodec(options));
}

Result<std::vector<std::uint8_t>> cookTextureFile(std::span<const std::uint8_t> source, const TextureOptions& options,
                                                  JobSystem& jobs, std::vector<std::string>* notes) {
    if (options.format != TextureFormat::Basis) {
        Result<TextureImage> cooked = cookTexture(source, options, jobs, notes);
        if (!cooked) return Error{cooked.error()};
        return assets::writeKtx2(*cooked);
    }
    if (!isKtx2(source)) {
        Result<Image> image = decodeImage(source);
        if (!image) return Error{image.error()};
        return cookImageFile(*image, options, jobs, notes);
    }
    if (assets::isBasisKtx2(source)) {
        Result<assets::BasisInfo> info = assets::readBasisInfo(source);
        if (!info) return Error{info.error()};
        if (info->srgb != options.srgb) note(notes, "a Basis Universal file keeps its own transfer function");
        return std::vector<std::uint8_t>(source.begin(), source.end());
    }
    Result<TextureImage> file = assets::readKtx2(source);
    if (!file) return Error{file.error()};
    const assets::PixelFormatInfo& info = assets::pixelFormatInfo(file->format);
    TextureOptions fileOptions = options;
    fileOptions.srgb = info.srgb;
    if (info.blockSize > 1) {
        if (info.srgb != options.srgb)
            note(notes, std::format("a {} KTX2 file keeps its own transfer function", info.name));
        note(notes, std::format("a {} KTX2 file is decoded and encoded again (compressed twice)", info.name));
        Result<Image> image = decodeLevel0(*file);
        if (!image) return Error{image.error()};
        return cookImageFile(*image, fileOptions, jobs, notes);
    }
    const std::span<const std::uint8_t> level0 = file->level(0);
    const Image image{file->width, file->height, std::vector<std::uint8_t>(level0.begin(), level0.end())};
    return cookImageFile(image, fileOptions, jobs, notes);
}

std::string describeTexture(std::span<const std::uint8_t> file) {
    if (assets::isBasisKtx2(file)) {
        Result<assets::BasisInfo> info = assets::readBasisInfo(file);
        if (!info) return info.error();
        return std::format("{} x {}, {} levels, basis {}{}{}{}", info->width, info->height, info->mipLevels,
                           assets::basisCodecName(info->codec), info->zstd ? " (zstd)" : "", info->srgb ? ", srgb" : "",
                           info->alpha ? ", alpha" : "");
    }
    Result<TextureImage> image = assets::readKtx2(file);
    if (!image) return image.error();
    return std::format("{} x {}, {} levels, {}", image->width, image->height, image->mipLevels,
                       assets::pixelFormatInfo(image->format).name);
}

} // namespace quench::cook
