// Basis Universal KTX2 files from the cook's own mip levels, through Basis Universal's encoder (v2.50,
// cmake/CookToolchain.cmake).
#include "Cook.h"

#include <basisu_comp.h>
#include <basisu_gpu_texture.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <mutex>

namespace quench::cook {
namespace {

void initEncoder() {
    static std::once_flag once;
    std::call_once(once, [] { basisu::basisu_encoder_init(false, false); });
}

basisu::image toBasis(const Image& image) { return basisu::image(image.rgba.data(), image.width, image.height, 4); }

} // namespace

Result<std::vector<std::uint8_t>> encodeBasis(std::span<const Image> levels, bool srgb, assets::BasisCodec codec) {
    if (levels.empty()) return Error{"cook: no levels to encode"};
    for (const Image& level : levels)
        if (level.width == 0 || level.height == 0 || level.rgba.size() != std::size_t(level.width) * level.height * 4)
            return Error{"cook: empty or inconsistent image"};
    initEncoder();

    basisu::basis_compressor_params params;
    params.set_format_mode(codec == assets::BasisCodec::Uastc ? basist::basis_tex_format::cUASTC_LDR_4x4
                                                              : basist::basis_tex_format::cETC1S);
    params.set_srgb_options(srgb);
    params.m_source_images.push_back(toBasis(levels[0]));
    if (levels.size() > 1) {
        params.m_source_mipmap_images.resize(1);
        for (std::size_t i = 1; i < levels.size(); ++i) params.m_source_mipmap_images[0].push_back(toBasis(levels[i]));
    }
    params.m_mip_gen = false;
    if (codec == assets::BasisCodec::Etc1s) params.m_quality_level = basisu::BASISU_DEFAULT_QUALITY;
    params.m_create_ktx2_file = true;
    params.m_ktx2_uastc_supercompression = basist::KTX2_SS_ZSTANDARD;
    params.m_read_source_images = false;
    params.m_write_output_basis_or_ktx2_files = false;
    params.m_status_output = false;
    params.m_print_stats = false;
    params.m_compute_stats = false;
    params.m_multithreading = false;
    basisu::job_pool pool(1); // the calling thread only
    params.m_pJob_pool = &pool;

    basisu::basis_compressor compressor;
    if (!compressor.init(params)) return Error{"cook: Basis Universal encoder rejected its parameters"};
    const basisu::basis_compressor::error_code status = compressor.process();
    if (status != basisu::basis_compressor::cECSuccess)
        return Error{std::format("cook: Basis Universal {} encoding failed (error {})", assets::basisCodecName(codec),
                                 int(status))};
    const basisu::uint8_vec& file = compressor.get_output_ktx2_file();
    return std::vector<std::uint8_t>(file.begin(), file.end());
}

Result<Image> decodeEtc2(const assets::TextureImage& texture, std::uint32_t level) {
    const assets::PixelFormat format = texture.format;
    const bool rgb = format == assets::PixelFormat::Etc2Rgb8 || format == assets::PixelFormat::Etc2Rgb8Srgb;
    const bool rgba = format == assets::PixelFormat::Etc2Rgba8 || format == assets::PixelFormat::Etc2Rgba8Srgb;
    if (!rgb && !rgba) return Error{"cook: not an ETC2 texture"};
    if (level >= texture.mipLevels) return Error{std::format("cook: no level {}", level)};
    initEncoder();
    Image image{texture.levelWidth(level), texture.levelHeight(level), {}};
    image.rgba.resize(std::size_t(image.width) * image.height * 4);
    const std::span<const std::uint8_t> blocks = texture.level(level);
    const std::uint32_t blocksX = (image.width + 3) / 4, blocksY = (image.height + 3) / 4;
    const std::size_t blockBytes = rgba ? 16 : 8;
    basisu::color_rgba texels[16];
    for (std::uint32_t by = 0; by < blocksY; ++by)
        for (std::uint32_t bx = 0; bx < blocksX; ++bx) {
            const std::uint8_t* block = blocks.data() + (std::size_t(by) * blocksX + bx) * blockBytes;
            if (!basisu::unpack_block(rgba ? basisu::texture_format::cETC2_RGBA : basisu::texture_format::cETC2_RGB,
                                      block, texels, false))
                return Error{std::format("cook: ETC2 block {}, {} of level {} does not decode", bx, by, level)};
            for (std::uint32_t y = 0; y < 4 && by * 4 + y < image.height; ++y)
                for (std::uint32_t x = 0; x < 4 && bx * 4 + x < image.width; ++x) {
                    std::uint8_t* out = &image.rgba[(std::size_t(by * 4 + y) * image.width + bx * 4 + x) * 4];
                    const basisu::color_rgba& c = texels[y * 4 + x];
                    out[0] = c.r;
                    out[1] = c.g;
                    out[2] = c.b;
                    out[3] = rgba ? c.a : 255;
                }
        }
    return image;
}

} // namespace quench::cook
