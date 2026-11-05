// quench_cook's library: textures (mips, BC7, the RGBA8 fallback, KTX2 pass-through) and models (the demo's glTF and
// GLB sources), and that the output does not depend on the number of threads.

#include "Test.h"

#include "Cook.h"

#include <quench/assets/Model.h>
#include <quench/assets/Texture.h>
#include <quench/core/Jobs.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace quench;
using namespace quench::cook;

namespace {

const std::filesystem::path kDemoSources = std::filesystem::path(QUENCH_TEST_SOURCE_DIR) / "examples/demo/assets_src";

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// Smooth gradients with a few hard edges, opaque.
Image testImage(std::uint32_t width, std::uint32_t height) {
    Image image{width, height, {}};
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            const bool stripe = (x / 8 + y / 8) % 2 == 0;
            image.rgba.insert(image.rgba.end(),
                              {std::uint8_t(x * 255 / std::max(width - 1, 1u)),
                               std::uint8_t(y * 255 / std::max(height - 1, 1u)), std::uint8_t(stripe ? 200 : 40), 255});
        }
    return image;
}

std::vector<std::uint8_t> modelBytes(const assets::ModelData& model) {
    Result<std::vector<std::uint8_t>> bytes = assets::writeModel(model);
    CHECK(bytes, "writeModel: %s", bytes ? "" : bytes.error().c_str());
    return bytes ? *bytes : std::vector<std::uint8_t>{};
}

} // namespace

QUENCH_TEST(cook_bc7_matches_the_source) {
    JobSystem jobs;
    const Image image = testImage(64, 32);
    Result<assets::TextureImage> cooked = cookImage(image, {.format = TextureFormat::Bc7, .srgb = false}, jobs);
    REQUIRE(cooked, "%s", cooked.error().c_str());
    CHECK(cooked->format == assets::PixelFormat::Bc7 && cooked->width == 64 && cooked->height == 32 &&
              cooked->mipLevels == 7,
          "%s %ux%u, %u levels", assets::pixelFormatInfo(cooked->format).name, cooked->width, cooked->height,
          cooked->mipLevels);
    Result<assets::TextureImage> decoded = assets::decodeBc7(*cooked);
    REQUIRE(decoded, "%s", decoded.error().c_str());
    const std::span<const std::uint8_t> level0 = decoded->level(0);
    REQUIRE(level0.size() == image.rgba.size(), "%zu bytes", level0.size());
    double sum = 0.0;
    int largest = 0;
    for (std::size_t i = 0; i < level0.size(); ++i) {
        const int d = std::abs(int(level0[i]) - int(image.rgba[i]));
        sum += d;
        largest = std::max(largest, d);
    }
    const double mean = sum / double(level0.size());
    CHECK(mean < 2.0 && largest < 32, "BC7 error: mean %.3f, largest %d", mean, largest);
}

QUENCH_TEST(cook_mips_filter_in_linear_light) {
    JobSystem jobs(JobSystemDesc{.workerThreads = 0});
    // 4 x 4 black and white texels: the 1 x 1 level is the average.
    Image image{4, 4, {}};
    for (int i = 0; i < 16; ++i) {
        const std::uint8_t v = (i % 4 + i / 4) % 2 ? 255 : 0;
        image.rgba.insert(image.rgba.end(), {v, v, v, 255});
    }
    Result<assets::TextureImage> srgb = cookImage(image, {.format = TextureFormat::Rgba8, .srgb = true}, jobs);
    Result<assets::TextureImage> linear = cookImage(image, {.format = TextureFormat::Rgba8, .srgb = false}, jobs);
    REQUIRE(srgb && linear, "cook failed");
    CHECK(srgb->format == assets::PixelFormat::Rgba8Srgb && srgb->mipLevels == 3, "%s, %u levels",
          assets::pixelFormatInfo(srgb->format).name, srgb->mipLevels);
    // Half of the light: 0.5 linear is 188 in sRGB, 128 in a linear texture.
    const std::uint8_t s = srgb->level(2)[0], l = linear->level(2)[0];
    CHECK(std::abs(int(s) - 188) <= 1 && std::abs(int(l) - 128) <= 1 && srgb->level(2)[3] == 255,
          "1 x 1 level: sRGB %d, linear %d", s, l);
}

QUENCH_TEST(cook_rgba8_when_not_a_multiple_of_4) {
    JobSystem jobs;
    std::vector<std::string> notes;
    Result<assets::TextureImage> cooked = cookImage(testImage(6, 5), {}, jobs, &notes);
    REQUIRE(cooked, "%s", cooked.error().c_str());
    CHECK(cooked->format == assets::PixelFormat::Rgba8Srgb && cooked->mipLevels == 3 && notes.size() == 1,
          "%s, %u levels, %zu notes", assets::pixelFormatInfo(cooked->format).name, cooked->mipLevels, notes.size());
    CHECK(cooked->levelWidth(1) == 3 && cooked->levelHeight(1) == 2 && cooked->levelWidth(2) == 1 &&
              cooked->levelHeight(2) == 1,
          "level 1 %ux%u", cooked->levelWidth(1), cooked->levelHeight(1));

    Result<assets::TextureImage> small = cookImage(testImage(64, 64), {.maxSize = 16}, jobs);
    REQUIRE(small, "%s", small.error().c_str());
    CHECK(small->width == 16 && small->height == 16 && small->mipLevels == 5, "max size 16: %ux%u, %u levels",
          small->width, small->height, small->mipLevels);
    CHECK(!cookImage(Image{4, 4, {}}, {}, jobs), "an empty image is an error");
}

QUENCH_TEST(cook_ktx2_sources) {
    JobSystem jobs;
    Result<assets::TextureImage> bc7 = cookImage(testImage(16, 16), {}, jobs);
    REQUIRE(bc7, "%s", bc7.error().c_str());
    Result<std::vector<std::uint8_t>> file = assets::writeKtx2(*bc7);
    REQUIRE(file, "%s", file.error().c_str());
    // A block-compressed file is passed through unchanged.
    std::vector<std::string> notes;
    Result<assets::TextureImage> passed = cookTexture(*file, {.srgb = false}, jobs, &notes);
    REQUIRE(passed, "%s", passed.error().c_str());
    CHECK(passed->format == bc7->format && passed->data == bc7->data && notes.size() == 1,
          "pass-through: %s, %zu notes", assets::pixelFormatInfo(passed->format).name, notes.size());

    // RGBA8 level 0 is cooked again, with the file's transfer function.
    assets::TextureImage rgba{.format = assets::PixelFormat::Rgba8, .width = 8, .height = 8, .mipLevels = 1};
    rgba.data = testImage(8, 8).rgba;
    file = assets::writeKtx2(rgba);
    REQUIRE(file, "%s", file.error().c_str());
    Result<assets::TextureImage> recooked = cookTexture(*file, {}, jobs);
    REQUIRE(recooked, "%s", recooked.error().c_str());
    CHECK(recooked->format == assets::PixelFormat::Bc7 && recooked->mipLevels == 4, "re-cooked: %s, %u levels",
          assets::pixelFormatInfo(recooked->format).name, recooked->mipLevels);

    const std::uint8_t garbage[] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(!cookTexture(garbage, {}, jobs), "garbage is an error");
}

QUENCH_TEST(cook_png_and_jpeg) {
    JobSystem jobs;
    Result<Image> png = decodeImage(readFile(kDemoSources / "tiles/tile_0.png"));
    REQUIRE(png, "%s", png.error().c_str());
    CHECK(png->width == 64 && png->height == 64 && png->rgba.size() == 64 * 64 * 4, "tile_0.png %ux%u", png->width,
          png->height);
    // pillar.glb embeds a JPEG base color and a PNG metallic-roughness texture.
    Result<CookedModel> pillar = cookModel(kDemoSources / "models/pillar.glb", "models/pillar", {}, jobs);
    REQUIRE(pillar, "%s", pillar.error().c_str());
    REQUIRE(pillar->textures.size() == 2, "%zu textures", pillar->textures.size());
    const CookedTexture& color = pillar->textures[0];
    const CookedTexture& data = pillar->textures[1];
    CHECK(color.assetPath == "models/pillar_0.ktx2" && color.image.format == assets::PixelFormat::Bc7Srgb &&
              color.image.width == 128 && color.image.height == 64,
          "base color %s: %s %ux%u", color.assetPath.c_str(), assets::pixelFormatInfo(color.image.format).name,
          color.image.width, color.image.height);
    CHECK(data.assetPath == "models/pillar_1_linear.ktx2" && data.image.format == assets::PixelFormat::Bc7,
          "metallic-roughness %s: %s", data.assetPath.c_str(), assets::pixelFormatInfo(data.image.format).name);
    REQUIRE(pillar->model.materials.size() == 1, "%zu materials", pillar->model.materials.size());
    const assets::ModelMaterial& material = pillar->model.materials[0];
    CHECK(material.baseColorTexture == color.assetPath && material.metallicRoughnessTexture == data.assetPath,
          "material textures '%s', '%s'", material.baseColorTexture.c_str(), material.metallicRoughnessTexture.c_str());
    CHECK(!decodeImage(std::span<const std::uint8_t>()), "an empty file is an error");
}

QUENCH_TEST(cook_models) {
    JobSystem jobs;
    // crate.gltf: a separate .bin and PNG, one material, base at y = 0.
    Result<CookedModel> crate = cookModel(kDemoSources / "models/crate.gltf", "models/crate", {}, jobs);
    REQUIRE(crate, "%s", crate.error().c_str());
    REQUIRE(crate->model.meshes.size() == 1, "%zu meshes", crate->model.meshes.size());
    const assets::ModelMesh& box = crate->model.meshes[0];
    CHECK(box.lods.size() >= 1 && box.lods[0].indexCount == 36, "crate: %zu LODs, LOD 0 %u indices", box.lods.size(),
          box.lods.empty() ? 0u : box.lods[0].indexCount);
    CHECK(std::abs(crate->model.boundsMin.y) < 1e-4f && std::abs(crate->model.boundsMax.y - 1.0f) < 1e-4f,
          "crate bounds y %.3f..%.3f", crate->model.boundsMin.y, crate->model.boundsMax.y);
    CHECK(crate->inputs.size() == 3 && crate->textures.size() == 1 &&
              crate->textures[0].assetPath == "models/crate_0.ktx2",
          "crate: %zu inputs, %zu textures", crate->inputs.size(), crate->textures.size());

    // ball.gltf: a data URI buffer, vertex colors, no normals (computed), 1280 triangles simplified into LODs.
    Result<CookedModel> ball = cookModel(kDemoSources / "models/ball.gltf", "models/ball", {.lods = 3}, jobs);
    REQUIRE(ball, "%s", ball.error().c_str());
    REQUIRE(ball->model.meshes.size() == 1 && !ball->model.meshes[0].lods.empty(), "%zu meshes",
            ball->model.meshes.size());
    const assets::ModelMesh& sphere = ball->model.meshes[0];
    CHECK(sphere.lods[0].indexCount == 1280 * 3, "ball LOD 0: %u indices", sphere.lods[0].indexCount);
    CHECK(sphere.lods.size() >= 2 && sphere.lods[1].indexCount < sphere.lods[0].indexCount * 3 / 4 &&
              sphere.lods[1].firstIndex == sphere.lods[0].indexCount,
          "ball: %zu LODs, LOD 1 %u indices", sphere.lods.size(),
          sphere.lods.size() > 1 ? sphere.lods[1].indexCount : 0u);
    bool unitNormals = true, colored = false;
    for (const assets::ModelVertex& v : sphere.vertices) {
        unitNormals = unitNormals && std::abs(glm::length(v.normal) - 1.0f) < 1e-3f;
        colored = colored || v.color.r != v.color.g;
        // Outward normals: the sphere's center is 0.5 m above the origin.
        unitNormals = unitNormals && glm::dot(v.normal, v.position - glm::vec3(0.0f, 0.5f, 0.0f)) > 0.0f;
    }
    CHECK(unitNormals && colored, "ball vertices: unit outward normals %d, vertex colors %d", unitNormals, colored);

    // What the cook writes reads back as the same model.
    const std::vector<std::uint8_t> bytes = modelBytes(ball->model);
    Result<assets::ModelData> read = assets::readModel(bytes);
    REQUIRE(read, "%s", read.error().c_str());
    CHECK(read->meshes.size() == 1 && read->meshes[0].vertices.size() == sphere.vertices.size() &&
              read->meshes[0].lods.size() == sphere.lods.size(),
          "read back %zu meshes", read->meshes.size());

    CHECK(!cookModel(kDemoSources / "models/missing.gltf", "models/missing", {}, jobs), "a missing file is an error");
}

QUENCH_TEST(cook_output_independent_of_threads) {
    JobSystem one(JobSystemDesc{.workerThreads = 0});
    JobSystem many(JobSystemDesc{.workerThreads = 3});
    const Image image = testImage(128, 64);
    Result<assets::TextureImage> a = cookImage(image, {}, one);
    Result<assets::TextureImage> b = cookImage(image, {}, many);
    REQUIRE(a && b, "cook failed");
    Result<std::vector<std::uint8_t>> fileA = assets::writeKtx2(*a), fileB = assets::writeKtx2(*b);
    REQUIRE(fileA && fileB, "writeKtx2 failed");
    CHECK(*fileA == *fileB, "texture: %zu and %zu bytes differ", fileA->size(), fileB->size());

    for (const char* name : {"models/pillar.glb", "models/ball.gltf"}) {
        Result<CookedModel> x = cookModel(kDemoSources / name, "models/m", {}, one);
        Result<CookedModel> y = cookModel(kDemoSources / name, "models/m", {}, many);
        REQUIRE(x && y, "%s: cook failed", name);
        CHECK(modelBytes(x->model) == modelBytes(y->model), "%s: the models differ", name);
        bool same = x->textures.size() == y->textures.size();
        for (std::size_t i = 0; same && i < x->textures.size(); ++i)
            same = x->textures[i].image.data == y->textures[i].image.data;
        CHECK(same, "%s: the textures differ", name);
    }
}
