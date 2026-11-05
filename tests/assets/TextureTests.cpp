#include "Test.h"

#include <quench/assets/AssetId.h>
#include <quench/assets/Texture.h>

#include <array>
#include <cstring>

using namespace quench;
using namespace quench::assets;

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
                               PixelFormat::Astc4x4, PixelFormat::Astc4x4Srgb}) {
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
