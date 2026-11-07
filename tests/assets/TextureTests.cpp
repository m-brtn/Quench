#include "Test.h"

#include "BasisTestImages.h"

#include <quench/assets/AssetId.h>
#include <quench/assets/Texture.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace quench;
using namespace quench::assets;

namespace quench::test {
// BasisTestData.cpp (tests/CMakeLists.txt): the files of assets/data.
std::span<const std::uint8_t> basisColorEtc1s();
std::span<const std::uint8_t> basisAlphaEtc1s();
std::span<const std::uint8_t> basisColorUastc();
std::span<const std::uint8_t> basisAlphaUastc();
std::span<const std::uint8_t> basisNormalsUastc();
std::span<const std::uint8_t> basisOddUastc();
std::span<const std::uint8_t> basisGreenEtc1s();
std::span<const std::uint8_t> basisBlueUastc();
} // namespace quench::test

namespace {

TextureImage gradient(PixelFormat format, std::uint32_t width, std::uint32_t height, std::uint32_t levels) {
    TextureImage image;
    image.format = format;
    image.width = width;
    image.height = height;
    image.mipLevels = levels;
    std::size_t size = 0;
    for (std::uint32_t i = 0; i < levels; ++i) size += levelBytes(format, image.levelWidth(i), image.levelHeight(i));
    image.data.resize(size);
    for (std::size_t i = 0; i < size; ++i) image.data[i] = std::uint8_t(i * 7 + 3);
    return image;
}

// A BC7 mode 6 block of one color: both endpoints (r7, g7, b7, a7) with p-bit p, every index 0.
std::array<std::uint8_t, 16> solidBc7(std::uint32_t r7, std::uint32_t g7, std::uint32_t b7, std::uint32_t a7,
                                      std::uint32_t p) {
    std::array<std::uint8_t, 16> block{};
    std::uint32_t bit = 0;
    const auto put = [&](std::uint32_t value, std::uint32_t bits) {
        for (std::uint32_t i = 0; i < bits; ++i, ++bit)
            if (value >> i & 1u) block[bit / 8] |= std::uint8_t(1u << (bit % 8));
    };
    put(1u << 6, 7);
    for (std::uint32_t c : {r7, g7, b7, a7}) {
        put(c, 7);
        put(c, 7);
    }
    put(p, 1);
    put(p, 1);
    return block; // the 63 index bits stay 0
}

} // namespace

QUENCH_TEST(asset_id_stable) {
    const AssetId a = assetId("textures/rock.ktx2");
    CHECK(a && a == assetId("textures/rock.ktx2"), "same path, same id");
    CHECK(a != assetId("textures/rock2.ktx2") && a != assetId("Textures/rock.ktx2"), "different paths collide");
    const std::string text = toString(a);
    CHECK(text.size() == 16 && text.find_first_not_of("0123456789abcdef") == std::string::npos, "toString '%s'",
          text.c_str());
    CHECK(!AssetId{}, "the empty id is false");
}

QUENCH_TEST(ktx2_round_trip) {
    for (PixelFormat format : {PixelFormat::Rgba8, PixelFormat::Rgba8Srgb, PixelFormat::Bc7, PixelFormat::Bc7Srgb,
                               PixelFormat::Astc4x4, PixelFormat::Astc4x4Srgb, PixelFormat::Etc2Rgb8,
                               PixelFormat::Etc2Rgb8Srgb, PixelFormat::Etc2Rgba8, PixelFormat::Etc2Rgba8Srgb}) {
        const TextureImage image = gradient(format, 20, 12, 5);
        const Result<std::vector<std::uint8_t>> file = writeKtx2(image);
        REQUIRE(file.ok(), "%s: write: %s", pixelFormatInfo(format).name, file.error().c_str());
        const Result<std::vector<std::uint8_t>> again = writeKtx2(image);
        CHECK(again.ok() && *again == *file, "%s: two writes differ", pixelFormatInfo(format).name);
        const Result<TextureImage> read = readKtx2(*file);
        REQUIRE(read.ok(), "%s: read: %s", pixelFormatInfo(format).name, read.error().c_str());
        CHECK(read->format == format && read->width == 20 && read->height == 12 && read->mipLevels == 5,
              "%s: header %ux%u, %u levels", pixelFormatInfo(format).name, read->width, read->height, read->mipLevels);
        CHECK(read->data == image.data, "%s: level data differ", pixelFormatInfo(format).name);

        const Result<TextureImage> skipped = readKtx2(*file, 2);
        REQUIRE(skipped.ok(), "skip: %s", skipped.error().c_str());
        CHECK(skipped->width == 5 && skipped->height == 3 && skipped->mipLevels == 3, "skip 2: %ux%u, %u levels",
              skipped->width, skipped->height, skipped->mipLevels);
        CHECK(std::memcmp(skipped->data.data(), image.data.data() + image.levelOffset(2), skipped->data.size()) == 0,
              "skipped levels differ");
    }
}

QUENCH_TEST(ktx2_rejects_corrupt) {
    const TextureImage image = gradient(PixelFormat::Bc7Srgb, 16, 16, 5);
    const std::vector<std::uint8_t> file = writeKtx2(image).value();
    CHECK(!readKtx2(std::span(file).first(40)), "truncated header accepted");
    for (std::size_t size = 0; size < file.size(); size += 13)
        CHECK(!readKtx2(std::span(file).first(size)), "prefix of %zu bytes accepted", size);
    std::vector<std::uint8_t> bad = file;
    bad[0] = 'X';
    CHECK(!readKtx2(bad), "bad magic accepted");
    bad = file;
    bad[12] = 99; // vkFormat
    const Result<TextureImage> unknown = readKtx2(bad);
    CHECK(!unknown && unknown.error().find("99") != std::string::npos, "unknown format: %s",
          unknown ? "accepted" : unknown.error().c_str());
    bad = file;
    bad[40] = 9; // level count above the full chain of 16 x 16
    CHECK(!readKtx2(bad), "level count accepted");
    bad = file;
    bad[23] = 0x40; // width 1 << 30
    CHECK(!readKtx2(bad), "huge width accepted");
    bad = file;
    bad[21] = 0x10; // width 4112: more level data than the file holds
    CHECK(!readKtx2(bad), "levels larger than the file accepted");
    // Every single byte flip is rejected or read without touching memory outside the file (checked under asan).
    for (std::size_t i = 0; i < file.size(); i += 3) {
        bad = file;
        bad[i] ^= 0x5a;
        (void)readKtx2(bad);
    }
}

QUENCH_TEST(bc7_decode_solid_block) {
    TextureImage image;
    image.format = PixelFormat::Bc7Srgb;
    image.width = 6; // two blocks wide, the second one cut off
    image.height = 3;
    image.mipLevels = 1;
    const auto block = solidBc7(100, 50, 25, 127, 1);
    image.data.insert(image.data.end(), block.begin(), block.end());
    image.data.insert(image.data.end(), block.begin(), block.end());
    const Result<TextureImage> rgba = decodeBc7(image);
    REQUIRE(rgba.ok(), "decode: %s", rgba.error().c_str());
    CHECK(rgba->format == PixelFormat::Rgba8Srgb && rgba->width == 6 && rgba->height == 3 &&
              rgba->data.size() == 6 * 3 * 4,
          "decoded %s %ux%u, %zu bytes", pixelFormatInfo(rgba->format).name, rgba->width, rgba->height,
          rgba->data.size());
    for (std::size_t i = 0; i + 3 < rgba->data.size(); i += 4)
        CHECK(rgba->data[i] == 201 && rgba->data[i + 1] == 101 && rgba->data[i + 2] == 51 && rgba->data[i + 3] == 255,
              "pixel %zu: %u %u %u %u", i / 4, rgba->data[i], rgba->data[i + 1], rgba->data[i + 2], rgba->data[i + 3]);
}

// ---- Basis Universal: the files of assets/data (BasisTestImages.h says what each one holds)

namespace {

std::span<const std::uint8_t> basisTestData(std::string_view name) {
    if (name == "color_etc1s.ktx2") return test::basisColorEtc1s();
    if (name == "alpha_etc1s.ktx2") return test::basisAlphaEtc1s();
    if (name == "color_uastc.ktx2") return test::basisColorUastc();
    if (name == "alpha_uastc.ktx2") return test::basisAlphaUastc();
    if (name == "normals_uastc.ktx2") return test::basisNormalsUastc();
    if (name == "odd_uastc.ktx2") return test::basisOddUastc();
    if (name == "green_etc1s.ktx2") return test::basisGreenEtc1s();
    if (name == "blue_uastc.ktx2") return test::basisBlueUastc();
    return {};
}

std::uint32_t fullChain(std::uint32_t width, std::uint32_t height) {
    return std::uint32_t(std::bit_width(std::max(width, height)));
}

const char* formatName(PixelFormat format) { return pixelFormatInfo(format).name; }

double millisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

QUENCH_TEST(basis_reads_info) {
    for (const test::BasisTestFile& entry : test::basisTestFiles()) {
        const std::span<const std::uint8_t> file = basisTestData(entry.name);
        REQUIRE(!file.empty(), "%s is not embedded", entry.name);
        const Result<BasisInfo> info = readBasisInfo(file);
        REQUIRE(isBasisKtx2(file) && info, "%s: %s", entry.name,
                info ? "not a Basis Universal file" : info.error().c_str());
        CHECK(info->codec == (entry.uastc ? BasisCodec::Uastc : BasisCodec::Etc1s) &&
                  info->width == entry.image.width && info->height == entry.image.height &&
                  info->mipLevels == fullChain(entry.image.width, entry.image.height) && info->srgb == entry.srgb &&
                  info->alpha == entry.alpha && info->zstd == entry.uastc,
              "%s: %s %ux%u, %u levels, sRGB %d, alpha %d, zstd %d", entry.name, basisCodecName(info->codec),
              info->width, info->height, info->mipLevels, info->srgb, info->alpha, info->zstd);
        const Result<TextureImage> plain = readKtx2(file);
        CHECK(!plain && plain.error().find("Basis Universal") != std::string::npos, "%s: readKtx2: %s", entry.name,
              plain ? "read" : plain.error().c_str());
    }
    const std::vector<std::uint8_t> bc7 = writeKtx2(gradient(PixelFormat::Bc7, 16, 16, 5)).value();
    CHECK(!isBasisKtx2(bc7) && !readBasisInfo(bc7), "a BC7 file taken for Basis Universal");
}

QUENCH_TEST(basis_target_by_support) {
    const TextureSupport none{}, bc7{.bc7 = true}, astc{.astc4x4 = true}, etc2{.etc2 = true};
    const TextureSupport desktop{.bc7 = true, .astc4x4 = true}, all{.bc7 = true, .astc4x4 = true, .etc2 = true};
    const BasisInfo uastc{.codec = BasisCodec::Uastc, .width = 64, .height = 64, .mipLevels = 7};
    BasisInfo etc1s = uastc;
    etc1s.codec = BasisCodec::Etc1s;
    BasisInfo uastcAlpha = uastc, etc1sAlpha = etc1s, srgb = uastc, odd = etc1s;
    uastcAlpha.alpha = etc1sAlpha.alpha = true;
    srgb.srgb = true;
    odd.width = 6;
    odd.height = 5;
    odd.mipLevels = 3;
    struct Row {
        const char* name;
        BasisInfo info;
        TextureSupport support;
        PixelFormat expected;
    };
    const Row rows[] = {
        {"UASTC", uastc, all, PixelFormat::Astc4x4},
        {"UASTC", uastc, bc7, PixelFormat::Bc7},
        {"UASTC", uastc, etc2, PixelFormat::Etc2Rgb8},
        {"UASTC", uastc, none, PixelFormat::Rgba8},
        {"UASTC with alpha", uastcAlpha, etc2, PixelFormat::Etc2Rgba8},
        {"ETC1S", etc1s, all, PixelFormat::Etc2Rgb8},
        {"ETC1S", etc1s, desktop, PixelFormat::Astc4x4},
        {"ETC1S", etc1s, astc, PixelFormat::Astc4x4},
        {"ETC1S", etc1s, bc7, PixelFormat::Bc7},
        {"ETC1S", etc1s, none, PixelFormat::Rgba8},
        {"ETC1S with alpha", etc1sAlpha, all, PixelFormat::Etc2Rgba8},
        {"sRGB UASTC", srgb, all, PixelFormat::Astc4x4Srgb},
        {"sRGB UASTC", srgb, none, PixelFormat::Rgba8Srgb},
        {"6 x 5 ETC1S", odd, all, PixelFormat::Rgba8},
    };
    for (const Row& row : rows) {
        const PixelFormat target = basisTarget(row.info, row.support);
        CHECK(target == row.expected, "%s with BC7 %d, ASTC %d, ETC2 %d: %s, not %s", row.name, row.support.bc7,
              row.support.astc4x4, row.support.etc2, formatName(target), formatName(row.expected));
    }
}

// Every file to every target: the format, the levels and their sizes; RGBA8 and BC7 (decoded) against the image the
// file was made of. The runtime has no ASTC or ETC2 decoder: the cook's cases compare those.
QUENCH_TEST(basis_transcodes) {
    for (const test::BasisTestFile& entry : test::basisTestFiles()) {
        const std::span<const std::uint8_t> file = basisTestData(entry.name);
        const std::uint32_t levels = fullChain(entry.image.width, entry.image.height);
        const bool blocks = entry.image.width % 4 == 0 && entry.image.height % 4 == 0;
        const double floor = entry.floor;
        for (const PixelFormat target : {PixelFormat::Rgba8, PixelFormat::Bc7, PixelFormat::Astc4x4,
                                         entry.alpha ? PixelFormat::Etc2Rgba8 : PixelFormat::Etc2Rgb8}) {
            const auto start = std::chrono::steady_clock::now();
            const Result<TextureImage> image = transcodeBasis(file, target);
            const double ms = millisecondsSince(start);
            if (!blocks && pixelFormatInfo(target).blockSize > 1) {
                CHECK(!image, "%s to %s: transcoded", entry.name, formatName(target));
                continue;
            }
            REQUIRE(image, "%s to %s: %s", entry.name, formatName(target), image ? "" : image.error().c_str());
            const PixelFormatInfo& info = pixelFormatInfo(image->format);
            CHECK(info.srgb == entry.srgb && info.blockBytes == pixelFormatInfo(target).blockBytes &&
                      image->width == entry.image.width && image->height == entry.image.height &&
                      image->mipLevels == levels && image->data.size() == image->levelOffset(levels),
                  "%s to %s: %s %ux%u, %u levels, %zu bytes", entry.name, formatName(target), info.name, image->width,
                  image->height, image->mipLevels, image->data.size());
            std::string quality;
            const Result<TextureImage> decoded = target == PixelFormat::Rgba8 ? image
                                                 : target == PixelFormat::Bc7 ? decodeBc7(*image)
                                                                              : Result<TextureImage>(Error{});
            if (decoded) {
                const std::span<const std::uint8_t> level0 = decoded->level(0);
                if (entry.normals) {
                    const auto [mean, largest] = test::basisNormalError(level0, entry.image.rgba);
                    CHECK(mean < floor, "%s to %s: mean normal error %.2f deg", entry.name, formatName(target), mean);
                    quality = std::format(": normals {:.2f} / {:.2f} deg", mean, largest);
                } else {
                    const double rgb = test::basisPsnr(level0, entry.image.rgba, 3);
                    CHECK(rgb > floor, "%s to %s: RGB %.2f dB", entry.name, formatName(target), rgb);
                    quality = std::format(": RGB {:.2f} dB", rgb);
                    if (entry.alpha) {
                        std::vector<std::uint8_t> a, b;
                        for (std::size_t i = 3; i < level0.size(); i += 4) {
                            a.insert(a.end(), {level0[i], 0, 0, 0});
                            b.insert(b.end(), {entry.image.rgba[i], 0, 0, 0});
                        }
                        const double alpha = test::basisPsnr(a, b, 1);
                        CHECK(alpha > 36.0, "%s to %s: alpha %.2f dB", entry.name, formatName(target), alpha);
                        quality += std::format(", alpha {:.2f} dB", alpha);
                    }
                }
            }
            std::printf("%-18s to %-10s %6.3f ms%s\n", entry.name, formatName(target), ms, quality.c_str());
        }
    }
}

QUENCH_TEST(basis_prepare_texture) {
    const TextureSupport none{}, bc7{.bc7 = true}, all{.bc7 = true, .astc4x4 = true, .etc2 = true};
    // Basis Universal files: transcoded to basisTarget(), RGBA8 counted as decoded on the CPU.
    const Result<PreparedTexture> uastc = prepareTexture(test::basisColorUastc(), all);
    CHECK(uastc && uastc->image.format == PixelFormat::Astc4x4Srgb && uastc->basis == BasisCodec::Uastc &&
              !uastc->decodedOnCpu,
          "UASTC on every format: %s", uastc ? formatName(uastc->image.format) : uastc.error().c_str());
    const Result<PreparedTexture> etc1s = prepareTexture(test::basisAlphaEtc1s(), bc7);
    CHECK(etc1s && etc1s->image.format == PixelFormat::Bc7Srgb && etc1s->basis == BasisCodec::Etc1s &&
              !etc1s->decodedOnCpu,
          "ETC1S on BC7: %s", etc1s ? formatName(etc1s->image.format) : etc1s.error().c_str());
    const Result<PreparedTexture> rgba = prepareTexture(test::basisNormalsUastc(), none, 2);
    CHECK(rgba && rgba->image.format == PixelFormat::Rgba8 && rgba->decodedOnCpu && rgba->image.width == 16 &&
              rgba->image.mipLevels == 5,
          "UASTC without block formats, 2 levels left out: %s",
          rgba ? formatName(rgba->image.format) : rgba.error().c_str());
    const Result<PreparedTexture> odd = prepareTexture(test::basisOddUastc(), all);
    CHECK(odd && odd->image.format == PixelFormat::Rgba8Srgb && odd->decodedOnCpu, "6 x 5 UASTC: %s",
          odd ? formatName(odd->image.format) : odd.error().c_str());
    // Plain files: as they are, BC7 decoded where the device cannot sample it, ASTC and ETC2 refused there.
    const std::vector<std::uint8_t> bc7File = writeKtx2(gradient(PixelFormat::Bc7Srgb, 16, 16, 5)).value();
    const Result<PreparedTexture> asIs = prepareTexture(bc7File, bc7);
    CHECK(asIs && asIs->image.format == PixelFormat::Bc7Srgb && !asIs->basis && !asIs->decodedOnCpu, "BC7 on BC7: %s",
          asIs ? formatName(asIs->image.format) : asIs.error().c_str());
    const Result<PreparedTexture> decoded = prepareTexture(bc7File, none);
    CHECK(decoded && decoded->image.format == PixelFormat::Rgba8Srgb && decoded->decodedOnCpu, "BC7 without BC7: %s",
          decoded ? formatName(decoded->image.format) : decoded.error().c_str());
    for (const PixelFormat format : {PixelFormat::Astc4x4, PixelFormat::Etc2Rgb8}) {
        const std::vector<std::uint8_t> plain = writeKtx2(gradient(format, 16, 16, 5)).value();
        CHECK(!prepareTexture(plain, bc7), "%s on BC7 only: prepared", formatName(format));
        CHECK(prepareTexture(plain, all), "%s with support: refused", formatName(format));
    }
}

// Truncated, edited and flipped files are errors (or transcode to something), never reads outside the file (checked
// under asan).
QUENCH_TEST(basis_rejects_corrupt) {
    for (const std::span<const std::uint8_t> source : {test::basisColorEtc1s(), test::basisColorUastc()}) {
        const std::vector<std::uint8_t> file(source.begin(), source.end());
        for (std::size_t size = 0; size < file.size(); size += 11) {
            const std::span<const std::uint8_t> prefix = std::span(file).first(size);
            CHECK(!readBasisInfo(prefix) && !transcodeBasis(prefix, PixelFormat::Rgba8), "prefix of %zu bytes accepted",
                  size);
        }
        std::vector<std::uint8_t> bad = file;
        bad[12] = 37; // a vkFormat (R8G8B8A8_UNORM): a plain file then
        CHECK(!isBasisKtx2(bad) && !readBasisInfo(bad), "a format accepted");
        bad = file;
        bad[28] = 4; // pixel depth: a 3D texture
        CHECK(!readBasisInfo(bad), "a 3D texture accepted");
        bad = file;
        bad[32] = 2; // layer count
        CHECK(!readBasisInfo(bad), "two layers accepted");
        bad = file;
        bad[36] = 6; // face count
        CHECK(!readBasisInfo(bad), "a cube map accepted");
        bad = file;
        bad[44] = 3; // supercompression: zlib
        CHECK(!readBasisInfo(bad), "zlib supercompression accepted");
        for (std::size_t i = 0; i < file.size(); i += 5) {
            bad = file;
            bad[i] ^= 0x5a;
            (void)transcodeBasis(bad, PixelFormat::Bc7);
            (void)transcodeBasis(bad, PixelFormat::Rgba8);
        }
    }
}

// The time of one file of 64 x 64 with mips per target (the median of many): printed for the device test app's runs.
// Small files are dominated by the per-file work (the header, ETC1S's codebooks); the cook's comparison measures the
// time per texel on larger textures.
QUENCH_TEST(basis_transcode_speed) {
#ifdef NDEBUG
    constexpr std::size_t kRepeats = 101;
#else
    constexpr std::size_t kRepeats = 5;
#endif
    const std::pair<const char*, std::span<const std::uint8_t>> files[] = {{"ETC1S", test::basisColorEtc1s()},
                                                                           {"UASTC", test::basisColorUastc()}};
    for (const auto& [name, file] : files) {
        std::string line;
        for (const PixelFormat target :
             {PixelFormat::Bc7, PixelFormat::Astc4x4, PixelFormat::Etc2Rgb8, PixelFormat::Rgba8}) {
            std::vector<double> ms;
            for (std::size_t i = 0; i < kRepeats; ++i) {
                const auto start = std::chrono::steady_clock::now();
                REQUIRE(transcodeBasis(file, target), "%s to %s failed", name, formatName(target));
                ms.push_back(millisecondsSince(start));
            }
            std::nth_element(ms.begin(), ms.begin() + kRepeats / 2, ms.end());
            line += std::format("{} {} {:.3f}", line.empty() ? "" : ",", formatName(target), ms[kRepeats / 2]);
        }
        std::printf("transcode a 64 x 64 %s file, ms:%s\n", name, line.c_str());
    }
}
