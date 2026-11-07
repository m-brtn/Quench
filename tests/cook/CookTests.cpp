// quench_cook's library: textures (mips, BC7, ASTC 4x4, the RGBA8 fallback, KTX2 pass-through), models (the demo's
// glTF and GLB sources), skeletons, clips and skinned models (a small glTF written by the test), retargeting (small
// skeletons in memory, and the demo's mannequin's walk on its strider), and that the output does not depend on the
// number of threads.

#include "Test.h"

#include "Cook.h"

#include <quench/animation/AnimationSystem.h>
#include <quench/assets/AssetManager.h>
#include <quench/assets/Model.h>
#include <quench/assets/Texture.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Jobs.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
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

// Peak signal to noise ratio of two RGBA8 images over all four channels, in dB (99 when they are the same).
double psnr(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double d = double(a[i]) - double(b[i]);
        sum += d * d;
    }
    return sum == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / (sum / double(a.size())));
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

QUENCH_TEST(cook_astc_matches_the_source) {
    JobSystem jobs;
    Result<Image> planks = decodeImage(readFile(kDemoSources / "models/crate_planks.png"));
    REQUIRE(planks, "%s", planks ? "" : planks.error().c_str());
    const std::pair<const char*, Image> images[] = {{"gradients", testImage(64, 32)}, {"planks", *planks}};
    for (const auto& [name, image] : images)
        for (const bool srgb : {true, false}) {
            Result<assets::TextureImage> astc =
                cookImage(image, {.format = TextureFormat::Astc4x4, .srgb = srgb}, jobs);
            Result<assets::TextureImage> bc7 = cookImage(image, {.format = TextureFormat::Bc7, .srgb = srgb}, jobs);
            REQUIRE(astc && bc7, "%s: cook failed", name);
            CHECK(astc->format == (srgb ? assets::PixelFormat::Astc4x4Srgb : assets::PixelFormat::Astc4x4) &&
                      astc->width == image.width && astc->mipLevels == bc7->mipLevels &&
                      astc->data.size() == bc7->data.size(),
                  "%s: %s %ux%u, %u levels, %zu bytes", name, assets::pixelFormatInfo(astc->format).name, astc->width,
                  astc->height, astc->mipLevels, astc->data.size());
            Result<Image> decoded = decodeAstc(*astc, 0);
            Result<assets::TextureImage> bc7Decoded = assets::decodeBc7(*bc7);
            REQUIRE(decoded && bc7Decoded, "%s: decode failed", name);
            const double astcDb = psnr(decoded->rgba, image.rgba), bc7Db = psnr(bc7Decoded->level(0), image.rgba);
            std::printf("%s %s: PSNR ASTC 4x4 %.2f dB, BC7 %.2f dB\n", name, srgb ? "sRGB" : "linear", astcDb, bc7Db);
            CHECK(astcDb >= 40.0, "%s %s: ASTC PSNR %.2f dB", name, srgb ? "sRGB" : "linear", astcDb);
            // The smallest level is one block of the source's average.
            Result<Image> last = decodeAstc(*astc, astc->mipLevels - 1);
            REQUIRE(last && last->width == 1 && last->height == 1, "%s: last level", name);
        }

    // A KTX2 file of ASTC blocks reads back the same and is passed through by the cook.
    Result<assets::TextureImage> astc = cookImage(testImage(16, 16), {.format = TextureFormat::Astc4x4}, jobs);
    REQUIRE(astc, "%s", astc.error().c_str());
    Result<std::vector<std::uint8_t>> file = assets::writeKtx2(*astc);
    REQUIRE(file, "%s", file.error().c_str());
    Result<assets::TextureImage> read = assets::readKtx2(*file);
    REQUIRE(read, "%s", read.error().c_str());
    CHECK(read->format == assets::PixelFormat::Astc4x4Srgb && read->mipLevels == 5 && read->data == astc->data,
          "KTX2: %s, %u levels", assets::pixelFormatInfo(read->format).name, read->mipLevels);
    Result<assets::TextureImage> passed = cookTexture(*file, {.format = TextureFormat::Bc7}, jobs);
    CHECK(passed && passed->data == astc->data, "an ASTC KTX2 file is passed through");
    CHECK(!encodeAstc(Image{4, 4, {}}, true, jobs), "an empty image is an error");
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
    notes.clear();
    cooked = cookImage(testImage(6, 8), {.format = TextureFormat::Astc4x4}, jobs, &notes);
    REQUIRE(cooked, "%s", cooked.error().c_str());
    CHECK(cooked->format == assets::PixelFormat::Rgba8Srgb && notes.size() == 1 &&
              notes[0].find("ASTC") != std::string::npos,
          "ASTC: %s, %zu notes", assets::pixelFormatInfo(cooked->format).name, notes.size());

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
    for (const bool srgb : {true, false}) {
        Result<assets::TextureImage> c = cookImage(image, {.format = TextureFormat::Astc4x4, .srgb = srgb}, one);
        Result<assets::TextureImage> d = cookImage(image, {.format = TextureFormat::Astc4x4, .srgb = srgb}, many);
        REQUIRE(c && d, "ASTC cook failed");
        CHECK(c->data == d->data, "ASTC %s: the blocks differ", srgb ? "sRGB" : "linear");
    }

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

// ---- skins and animations

namespace {

// A small skinned glTF with a separate .bin: root > hip > knee > socket (no mesh, below a joint), a body mesh
// skinned to root, hip and knee, and two animations: "bend" (the knee's rotation linear, the hip's translation
// step, the root's translation a cubic spline with zero tangents) and "pose" (one key).
struct GltfWriter {
    std::vector<std::uint8_t> bin;
    std::vector<std::string> views, accessors;

    template <class T>
    int add(const std::vector<T>& data, int count, const char* type, int componentType, const std::string& extra = "") {
        while (bin.size() % 4 != 0) bin.push_back(0);
        const std::size_t offset = bin.size();
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
        bin.insert(bin.end(), bytes, bytes + data.size() * sizeof(T));
        views.push_back(
            std::format(R"({{"buffer": 0, "byteOffset": {}, "byteLength": {}}})", offset, data.size() * sizeof(T)));
        accessors.push_back(std::format(R"({{"bufferView": {}, "componentType": {}, "count": {}, "type": "{}"{}}})",
                                        views.size() - 1, componentType, count, type, extra));
        return int(accessors.size() - 1);
    }
};

constexpr int kFloat = 5126, kUnsignedByte = 5121, kUnsignedShort = 5123;

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) out += (out.empty() ? "" : ", ") + item;
    return out;
}

std::filesystem::path writeSkinnedGltf(const std::filesystem::path& dir, std::string_view settings) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    GltfWriter w;
    const int positions = w.add(std::vector<float>{0, 0, 0, 0.1f, 0, 0, 0, 1, 0, 0.1f, 1, 0}, 4, "VEC3", kFloat,
                                R"(, "min": [0, 0, 0], "max": [0.1, 1, 0])");
    const int normals = w.add(std::vector<float>{0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1}, 4, "VEC3", kFloat);
    const int joints =
        w.add(std::vector<std::uint8_t>{2, 1, 0, 0, 2, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, 4, "VEC4", kUnsignedByte);
    const int weights =
        w.add(std::vector<float>{0.75f, 0.25f, 0, 0, 0.75f, 0.25f, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, 4, "VEC4", kFloat);
    const int indices = w.add(std::vector<std::uint16_t>{0, 1, 2, 2, 1, 3}, 6, "SCALAR", kUnsignedShort);
    std::vector<float> ibm;
    for (const float y : {0.0f, -1.0f, -0.5f}) // the joints' bind positions are y 0, 1 and 0.5
        ibm.insert(ibm.end(), {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, y, 0, 1});
    const int inverseBind = w.add(ibm, 3, "MAT4", kFloat);
    const float s = std::sqrt(0.5f);
    const int linearTimes = w.add(std::vector<float>{0, 1}, 2, "SCALAR", kFloat, R"(, "min": [0], "max": [1])");
    const int kneeRotations = w.add(std::vector<float>{0, 0, 0, 1, s, 0, 0, s}, 2, "VEC4", kFloat);
    const int stepTimes = w.add(std::vector<float>{0, 0.5f}, 2, "SCALAR", kFloat, R"(, "min": [0], "max": [0.5])");
    const int hipTranslations = w.add(std::vector<float>{0, 1, 0, 0, 1.2f, 0}, 2, "VEC3", kFloat);
    const int cubicTimes = w.add(std::vector<float>{0, 1}, 2, "SCALAR", kFloat, R"(, "min": [0], "max": [1])");
    const int rootCubic =
        w.add(std::vector<float>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0}, 6, "VEC3", kFloat);
    const int poseTimes = w.add(std::vector<float>{0}, 1, "SCALAR", kFloat, R"(, "min": [0], "max": [0])");
    const int poseRotation = w.add(std::vector<float>{s, 0, 0, s}, 1, "VEC4", kFloat);

    const std::string json = std::format(
        R"({{"asset": {{"version": "2.0"}}, "scene": 0, "scenes": [{{"nodes": [0, 4]}}],
"nodes": [{{"name": "root", "children": [1]}}, {{"name": "hip", "translation": [0, 1, 0], "children": [2]}},
  {{"name": "knee", "translation": [0, -0.5, 0], "children": [3]}}, {{"name": "socket", "translation": [0.1, 0, 0]}},
  {{"name": "body", "mesh": 0, "skin": 0}}],
"skins": [{{"joints": [0, 1, 2], "inverseBindMatrices": {}}}],
"meshes": [{{"primitives": [{{"attributes": {{"POSITION": {}, "NORMAL": {}, "JOINTS_0": {}, "WEIGHTS_0": {}}},
  "indices": {}, "material": 0}}]}}],
"materials": [{{"name": "m"}}],
"animations": [
  {{"name": "bend", "channels": [{{"sampler": 0, "target": {{"node": 2, "path": "rotation"}}}},
    {{"sampler": 1, "target": {{"node": 1, "path": "translation"}}}},
    {{"sampler": 2, "target": {{"node": 0, "path": "translation"}}}}],
   "samplers": [{{"input": {}, "output": {}, "interpolation": "LINEAR"}},
    {{"input": {}, "output": {}, "interpolation": "STEP"}}, {{"input": {}, "output": {}, "interpolation": "CUBICSPLINE"}}]}},
  {{"name": "pose", "channels": [{{"sampler": 0, "target": {{"node": 2, "path": "rotation"}}}}],
   "samplers": [{{"input": {}, "output": {}}}]}}],
"buffers": [{{"uri": "skinned.bin", "byteLength": {}}}],
"bufferViews": [{}],
"accessors": [{}]}})",
        inverseBind, positions, normals, joints, weights, indices, linearTimes, kneeRotations, stepTimes,
        hipTranslations, cubicTimes, rootCubic, poseTimes, poseRotation, w.bin.size(), join(w.views),
        join(w.accessors));
    std::ofstream(dir / "skinned.gltf", std::ios::binary) << json;
    std::ofstream(dir / "skinned.bin", std::ios::binary)
        .write(reinterpret_cast<const char*>(w.bin.data()), std::streamsize(w.bin.size()));
    if (!settings.empty()) std::ofstream(dir / "skinned.anim.json", std::ios::binary) << settings;
    return dir / "skinned.gltf";
}

constexpr std::string_view kSettings =
    R"({"clips": {"bend": {"loop": false, "events": [{"time": 0.5, "name": "step", "value": 2}]}}})";

std::filesystem::path tmpPath(const char* name) { return std::filesystem::path(quench::test::tmpDir()) / name; }

// Model-space joint positions of a cooked clip at normalized times, through the asset system.
struct Sampled {
    glm::vec3 root, hip;
    glm::vec3 kneeUp; // the knee's y axis
};

// Writes a cooked skeleton and clip set into `dir` and calls fn(manager, animation, character) with a character
// whose one state plays `clip`, after its assets have loaded.
template <class F>
void withCooked(const std::filesystem::path& dir, const CookedAnimation& skeleton, const CookedAnimation& clips,
                std::string_view clip, bool loop, F&& fn) {
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "s.qskel", std::ios::binary)
        .write(reinterpret_cast<const char*>(skeleton.file.data()), std::streamsize(skeleton.file.size()));
    std::ofstream(dir / "s.qanim", std::ios::binary)
        .write(reinterpret_cast<const char*>(clips.file.data()), std::streamsize(clips.file.size()));
    std::ofstream(dir / "g.json", std::ios::binary) << std::format(
        R"({{"layers": [{{"name": "base", "states": [{{"name": "s", "clip": "{}", "loop": {}}}]}}]}})", clip, loop);
    fs::setBackend(fs::makeStdBackend(dir, {}));
    {
        JobSystem jobs(JobSystemDesc{.workerThreads = 0});
        assets::AssetManager manager(jobs);
        animation::AnimationSystem anim(manager, jobs);
        animation::CharacterDesc desc;
        desc.skeleton = anim.loadSkeleton("s.qskel");
        desc.clips.push_back(anim.loadClips("s.qanim"));
        desc.graph = anim.loadGraph("g.json");
        const animation::CharacterId id = anim.addCharacter(std::move(desc));
        for (int i = 0; i < 100 && !manager.idle(); ++i) manager.update();
        fn(manager, anim, id);
    }
    fs::setBackend(nullptr);
}

std::vector<Sampled> sampleCooked(const std::filesystem::path& dir, const CookedAnimation& skeleton,
                                  const CookedAnimation& clips, std::span<const float> phases) {
    std::vector<Sampled> out;
    withCooked(dir, skeleton, clips, "bend", false,
               [&](assets::AssetManager& manager, animation::AnimationSystem& anim, animation::CharacterId id) {
                   for (const float phase : phases) {
                       anim.setPhase(id, phase);
                       manager.update();
                       anim.update(0.0f);
                       const std::span<const glm::mat4> m = anim.modelMatrices(id);
                       const auto at = [&](const char* name) { return m[std::size_t(anim.findJoint(id, name))]; };
                       if (!anim.state(id).bound || m.empty()) break;
                       out.push_back({glm::vec3(at("root")[3]), glm::vec3(at("hip")[3]), glm::vec3(at("knee")[1])});
                   }
               });
    return out;
}

// Plays a cooked clip for `frames` updates of `dt` and records the named joints' world positions per frame: their
// model positions moved by the root motion added up (the clips played here do not turn).
std::vector<std::vector<glm::vec3>> playCooked(const std::filesystem::path& dir, const CookedAnimation& skeleton,
                                               const CookedAnimation& clips, std::string_view clip,
                                               std::span<const char* const> joints, int frames, float dt) {
    std::vector<std::vector<glm::vec3>> out;
    withCooked(dir, skeleton, clips, clip, true,
               [&](assets::AssetManager& manager, animation::AnimationSystem& anim, animation::CharacterId id) {
                   glm::vec3 moved(0.0f);
                   for (int frame = 0; frame <= frames; ++frame) {
                       manager.update();
                       anim.update(frame == 0 ? 0.0f : dt);
                       const std::span<const glm::mat4> m = anim.modelMatrices(id);
                       if (!anim.state(id).bound || m.empty()) return;
                       moved += anim.rootMotion(id).translation;
                       std::vector<glm::vec3>& row = out.emplace_back();
                       for (const char* name : joints) {
                           const int joint = anim.findJoint(id, name);
                           CHECK(joint >= 0, "%s: no joint %s", std::string(clip).c_str(), name);
                           row.push_back(joint >= 0 ? moved + glm::vec3(m[std::size_t(joint)][3]) : glm::vec3(0.0f));
                       }
                   }
               });
    return out;
}

const std::filesystem::path kCharacters = QUENCH_TEST_CHARACTER_DIR; // empty when the build had no Python

} // namespace

QUENCH_TEST(cook_skeleton_from_skins) {
    const std::filesystem::path gltf = writeSkinnedGltf(tmpPath("cook_skeleton"), "");
    Result<CookedAnimation> cooked = cookSkeleton(gltf);
    REQUIRE(cooked, "%s", cooked ? "" : cooked.error().c_str());
    CHECK(cooked->names == (std::vector<std::string>{"root", "hip", "knee", "socket"}),
          "joints: the skin's, and the socket below the knee (%zu)", cooked->names.size());
    Result<std::shared_ptr<const animation::Skeleton>> skeleton = animation::readSkeleton(cooked->file);
    REQUIRE(skeleton, "%s", skeleton ? "" : skeleton.error().c_str());
    const animation::Skeleton& s = **skeleton;
    CHECK(s.parent(s.findJoint("socket")) == s.findJoint("knee") && s.parent(s.findJoint("hip")) == s.findJoint("root"),
          "parents");
    CHECK(std::abs(s.restModelMatrices()[std::size_t(s.findJoint("knee"))][3].y - 0.5f) < 1e-6f, "the knee at rest");
    CHECK(cooked->inputs.size() == 2, "inputs: the glTF and its .bin (%zu)", cooked->inputs.size());
    Result<CookedAnimation> again = cookSkeleton(gltf);
    CHECK(again && again->file == cooked->file, "two cooks write the same bytes");
    CHECK(!cookSkeleton(tmpPath("cook_skeleton") / "missing.gltf"), "a missing file is an error");
}

QUENCH_TEST(cook_animation_keys_and_settings) {
    const std::filesystem::path dir = tmpPath("cook_animation");
    const std::filesystem::path gltf = writeSkinnedGltf(dir, kSettings);
    Result<CookedAnimation> skeleton = cookSkeleton(gltf);
    Result<CookedAnimation> clips = cookAnimation(gltf, {});
    REQUIRE(skeleton && clips, "%s", !skeleton ? skeleton.error().c_str() : clips ? "" : clips.error().c_str());
    CHECK(clips->names == (std::vector<std::string>{"bend", "pose"}), "%zu clips", clips->names.size());
    CHECK(std::ranges::any_of(clips->notes, [](const std::string& n) { return n.find("one pose") != n.npos; }),
          "the one-key clip is noted");
    CHECK(std::ranges::any_of(clips->inputs, [](const auto& p) { return p.filename() == "skinned.anim.json"; }),
          "the settings are an input (%zu inputs)", clips->inputs.size());
    Result<std::shared_ptr<const animation::ClipSet>> set = animation::readClipSet(clips->file);
    REQUIRE(set, "%s", set ? "" : set.error().c_str());
    const animation::Clip* bend = (*set)->find("bend");
    REQUIRE(bend && (*set)->find("pose"), "both clips");
    CHECK(std::abs(bend->duration() - 1.0f) < 1e-6f && !bend->loops() && (*set)->find("pose")->loops(),
          "bend: %.3f s, loops %d", bend->duration(), bend->loops());
    CHECK(bend->events().size() == 1 && bend->events()[0].name == "step" && bend->events()[0].value == 2.0f,
          "bend's event from the settings");
    Result<CookedAnimation> again = cookAnimation(gltf, {});
    CHECK(again && again->file == clips->file, "two cooks write the same bytes");

    // Linear (the knee), step (the hip) and the cubic spline (the root, h01(t) = 3t^2 - 2t^3) at a quarter, half and
    // three quarters.
    const float phases[] = {0.25f, 0.5f, 0.75f};
    const std::vector<Sampled> s = sampleCooked(dir, *skeleton, *clips, phases);
    REQUIRE(s.size() == 3, "sampled %zu times", s.size());
    const float rootX[] = {0.15625f, 0.5f, 0.84375f}, hipY[] = {1.0f, 1.2f, 1.2f};
    const float kneeAngle[] = {0.25f * 1.5707964f, 0.5f * 1.5707964f, 0.75f * 1.5707964f};
    for (int i = 0; i < 3; ++i) {
        const float angle = std::atan2(s[std::size_t(i)].kneeUp.z, s[std::size_t(i)].kneeUp.y);
        CHECK(std::abs(s[std::size_t(i)].root.x - rootX[i]) < 3e-3f, "t %.2f: root x %.4f, expected %.4f", phases[i],
              s[std::size_t(i)].root.x, rootX[i]);
        CHECK(std::abs(s[std::size_t(i)].hip.y - s[std::size_t(i)].root.y - hipY[i]) < 3e-3f,
              "t %.2f: hip y %.4f, expected %.4f", phases[i], s[std::size_t(i)].hip.y, hipY[i]);
        CHECK(std::abs(angle - kneeAngle[i]) < 0.01f, "t %.2f: knee angle %.4f, expected %.4f", phases[i], angle,
              kneeAngle[i]);
    }
}

QUENCH_TEST(cook_animation_errors) {
    const std::filesystem::path dir = tmpPath("cook_animation_errors");
    const std::filesystem::path gltf = writeSkinnedGltf(dir, R"({"clips": {"bend": {"loops": true}}})");
    Result<CookedAnimation> unknownKey = cookAnimation(gltf, {});
    CHECK(!unknownKey && unknownKey.error().find("loops") != std::string::npos, "an unknown setting: %s",
          unknownKey ? "accepted" : unknownKey.error().c_str());
    std::ofstream(dir / "skinned.anim.json", std::ios::binary) << R"({"clips": {"walk": {"loop": true}}})";
    CHECK(!cookAnimation(gltf, {}), "settings for a clip the file does not have");
    std::ofstream(dir / "skinned.anim.json", std::ios::binary) << R"({"clips": )";
    CHECK(!cookAnimation(gltf, {}), "broken JSON");
    CHECK(!cookAnimation(dir / "missing.gltf", {}), "a missing file");
    AnimationOptions otherSkeleton;
    otherSkeleton.skeleton = dir / "missing.gltf";
    CHECK(!cookAnimation(gltf, otherSkeleton), "a missing skeleton file");
    std::filesystem::remove(dir / "skinned.anim.json");
    CHECK(cookAnimation(gltf, {}), "without settings");
}

// ---- retargeting

namespace {

using animation::ClipDesc;
using animation::SkeletonDesc;

glm::quat axisAngle(float angle, glm::vec3 axis) { return glm::angleAxis(angle, glm::normalize(axis)); }

template <class V>
V sampleKeys(const std::vector<animation::Key<V>>& keys, float time, const V& rest) {
    if (keys.empty()) return rest;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    std::size_t b = 1;
    while (keys[b].time < time) ++b;
    const float u = (time - keys[b - 1].time) / (keys[b].time - keys[b - 1].time);
    if constexpr (std::is_same_v<V, glm::quat>) {
        const glm::quat to = glm::dot(keys[b - 1].value, keys[b].value) < 0.0f ? -keys[b].value : keys[b].value;
        return glm::normalize(keys[b - 1].value * (1.0f - u) + to * u);
    } else {
        return keys[b - 1].value + (keys[b].value - keys[b - 1].value) * u;
    }
}

// Model matrices of a clip description at a time (linear keys, normalized-linear rotations, as the runtime).
std::vector<glm::mat4> modelAt(const SkeletonDesc& skeleton, const ClipDesc& clip, float time) {
    std::vector<glm::mat4> out(skeleton.joints.size());
    for (std::size_t j = 0; j < out.size(); ++j) {
        const animation::Transform& rest = skeleton.joints[j].rest;
        const glm::mat4 m =
            glm::translate(glm::mat4(1.0f), sampleKeys(clip.tracks[j].translations, time, rest.translation)) *
            glm::mat4_cast(sampleKeys(clip.tracks[j].rotations, time, rest.rotation));
        const int p = skeleton.joints[j].parent;
        out[j] = p >= 0 ? out[std::size_t(p)] * m : m;
    }
    return out;
}

glm::vec3 at(const std::vector<glm::mat4>& model, int joint) { return glm::vec3(model[std::size_t(joint)][3]); }

// root > hip > knee > foot, and an arm hanging from the hip (arm > hand); one clip that walks, bends the knee,
// sways the hip and swings the arm, with root motion and an event.
SkeletonDesc sourceSkeleton() {
    SkeletonDesc s;
    s.joints = {{"root", -1, {}},
                {"hip", 0, {{0, 1, 0}}},
                {"knee", 1, {{0, -0.5f, 0}}},
                {"foot", 2, {{0, -0.5f, 0}}},
                {"arm", 1, {{0.2f, 0.5f, 0}}},
                {"hand", 4, {{0, -0.3f, 0}}}};
    return s;
}

ClipDesc sourceClip() {
    ClipDesc c;
    c.name = "walk";
    c.duration = 1.0f;
    c.tracks.resize(6);
    c.tracks[0].translations = {{0.0f, {0, 0, 0}}, {1.0f, {1, 0, 0}}};
    c.tracks[1].translations = {{0.0f, {0, 1, 0}}, {0.5f, {0, 0.9f, 0}}, {1.0f, {0, 1, 0}}};
    c.tracks[1].rotations = {
        {0.0f, {1, 0, 0, 0}}, {0.5f, axisAngle(0.3f, {0, 1, 0}) * axisAngle(0.1f, {0, 0, 1})}, {1.0f, {1, 0, 0, 0}}};
    c.tracks[2].rotations = {
        {0.0f, {1, 0, 0, 0}}, {0.5f, axisAngle(0.8f, {1, 0, 0})}, {1.0f, axisAngle(0.2f, {1, 0, 0})}};
    c.tracks[4].rotations = {{0.0f, axisAngle(0.2f, {0, 0, 1})}, {1.0f, axisAngle(-0.6f, {1, 0, 0})}};
    c.events = {{0.25f, "footstep", 1.0f}};
    c.rootMotion = animation::RootMotionDesc{"root", false};
    return c;
}

} // namespace

QUENCH_TEST(cook_retarget_map_and_errors) {
    Result<RetargetMap> map = parseRetargetMap(
        R"({"joints": {"Root": "root", "Hips": "pelvis"}, "translations": ["Root", "Hips"], "scale": "Hips"})");
    REQUIRE(map, "%s", map ? "" : map.error().c_str());
    CHECK(map->joints.size() == 2 &&
              std::ranges::find(map->joints, std::pair<std::string, std::string>{"Hips", "pelvis"}) !=
                  map->joints.end(),
          "joints: %zu", map->joints.size());
    CHECK(map->translations == (std::vector<std::string>{"Root", "Hips"}) && map->scaleJoint == "Hips" &&
              map->scale == 0.0f,
          "translations and the scale joint");
    Result<RetargetMap> number = parseRetargetMap(R"({"joints": {"a": "b"}, "scale": 1.5})");
    CHECK(number && number->scale == 1.5f && number->scaleJoint.empty(), "a scale as a number");
    for (const char* text :
         {R"({"joints": {"a": "b"}, "scales": 2})", R"({"joints": {}})", R"({"joints": {"a": 3}})",
          R"({"joints": {"a": "b"}, "translations": ["c"]})", R"({"joints": {"a": "b"}, "scale": -1})",
          R"({"joints": {"a": "b"}, "scale": "c"})", R"({"joints": )", R"(["a"])"})
        CHECK(!parseRetargetMap(text), "accepted: %s", text);

    const SkeletonDesc source = sourceSkeleton();
    const ClipDesc clip = sourceClip();
    const auto retarget = [&](std::string_view text, std::span<const ClipDesc> clips) -> Result<std::vector<ClipDesc>> {
        Result<RetargetMap> m = parseRetargetMap(text);
        if (!m) return Error{"the map: " + m.error()};
        return retargetClips(source, source, clips, *m, nullptr);
    };
    const ClipDesc one[] = {clip};
    Result<std::vector<ClipDesc>> missingTarget = retarget(R"({"joints": {"Ankle": "foot", "root": "root"}})", one);
    CHECK(!missingTarget && missingTarget.error().find("Ankle") != std::string::npos, "a joint the target lacks: %s",
          missingTarget ? "accepted" : missingTarget.error().c_str());
    Result<std::vector<ClipDesc>> missingSource = retarget(R"({"joints": {"foot": "ankle", "root": "root"}})", one);
    CHECK(!missingSource && missingSource.error().find("ankle") != std::string::npos, "a joint the source lacks: %s",
          missingSource ? "accepted" : missingSource.error().c_str());
    Result<std::vector<ClipDesc>> noRootMotion = retarget(R"({"joints": {"hip": "hip"}})", one);
    CHECK(!noRootMotion && noRootMotion.error().find("root motion") != std::string::npos,
          "the root motion joint not mapped: %s", noRootMotion ? "accepted" : noRootMotion.error().c_str());
    ClipDesc short_ = clip;
    short_.tracks.pop_back();
    const ClipDesc shortOne[] = {short_};
    CHECK(!retarget(R"({"joints": {"root": "root"}})", shortOne), "a clip with too few tracks");
}

QUENCH_TEST(cook_retarget_onto_itself) {
    const SkeletonDesc skeleton = sourceSkeleton();
    const ClipDesc clips[] = {sourceClip()};
    Result<RetargetMap> map = parseRetargetMap(R"({"joints": {"root": "root", "hip": "hip", "knee": "knee",
        "foot": "foot", "arm": "arm", "hand": "hand"}, "translations": ["root", "hip"]})");
    REQUIRE(map, "%s", map ? "" : map.error().c_str());
    std::vector<std::string> notes;
    Result<std::vector<ClipDesc>> out = retargetClips(skeleton, skeleton, clips, *map, &notes);
    REQUIRE(out && out->size() == 1, "%s", out ? "" : out.error().c_str());
    const ClipDesc& c = (*out)[0];
    CHECK(notes.empty(), "no notes (%zu)", notes.size());
    CHECK(c.name == "walk" && c.duration == 1.0f && c.loop && c.events.size() == 1 && c.events[0].name == "footstep" &&
              c.rootMotion && c.rootMotion->joint == "root",
          "name, duration, loop, events and root motion copied");
    float worst = 0.0f;
    for (int i = 0; i <= 60; ++i) {
        const float t = float(i) / 60.0f;
        const std::vector<glm::mat4> a = modelAt(skeleton, clips[0], t), b = modelAt(skeleton, c, t);
        for (std::size_t j = 0; j < a.size(); ++j)
            for (int k = 0; k < 4; ++k) worst = std::max(worst, glm::length(a[j][k] - b[j][k]));
    }
    std::printf("retarget onto itself: largest difference %.7f\n", worst);
    CHECK(worst < 5e-4f, "the same pose at 61 times: largest difference %.6f", worst);
}

QUENCH_TEST(cook_retarget_proportions_and_rest_poses) {
    // The target: other names, 1.5 times the size, the knee's rest turned by half a turn about X (its child lies
    // along its +Y), an unmapped joint above a T-pose arm whose rest points along +X, 1.5 times as long.
    const SkeletonDesc source = sourceSkeleton();
    SkeletonDesc target;
    target.joints = {{"Root", -1, {}},
                     {"Hips", 0, {{0, 1.5f, 0}}},
                     {"Knee", 1, {{0, -0.75f, 0}, axisAngle(glm::pi<float>(), {1, 0, 0})}},
                     {"Foot", 2, {{0, 0.75f, 0}}},
                     {"Chest", 1, {{0, 0.2f, 0}}},
                     {"Arm", 4, {{0.3f, 0.25f, 0}, axisAngle(-0.5f * glm::pi<float>(), {0, 0, 1})}},
                     {"Hand", 5, {{0, 0.45f, 0}}}};
    const ClipDesc clips[] = {sourceClip()};
    Result<RetargetMap> map = parseRetargetMap(R"({"joints": {"Root": "root", "Hips": "hip", "Knee": "knee",
        "Foot": "foot", "Arm": "arm", "Hand": "hand"}, "translations": ["Root", "Hips"], "scale": "Hips"})");
    REQUIRE(map, "%s", map ? "" : map.error().c_str());
    std::vector<std::string> notes;
    Result<std::vector<ClipDesc>> out = retargetClips(source, target, clips, *map, &notes);
    REQUIRE(out && out->size() == 1, "%s", out ? "" : out.error().c_str());
    const ClipDesc& c = (*out)[0];
    CHECK(c.rootMotion && c.rootMotion->joint == "Root", "the root motion joint renamed");
    CHECK(notes.size() == 1 && notes[0].find("Chest") != std::string::npos, "the unmapped joint is noted");

    // At the keys (30 per second and the source's) and between them: the foot 1.5 times as far from the origin
    // (the root motion, the hips' height and the leg scaled), the hand 1.5 times as far from the arm's joint.
    float atKeys = 0.0f, between = 0.0f, chest = 0.0f;
    for (int i = 0; i <= 60; ++i) {
        const float t = float(i) / 60.0f;
        const std::vector<glm::mat4> s = modelAt(source, clips[0], t), r = modelAt(target, c, t);
        const float e = std::max(glm::length(at(r, 3) - 1.5f * at(s, 3)),
                                 glm::length((at(r, 6) - at(r, 5)) - 1.5f * (at(s, 5) - at(s, 4))));
        (i % 2 == 0 ? atKeys : between) = std::max(i % 2 == 0 ? atKeys : between, e);
        chest = std::max(
            chest, glm::length(glm::vec3(r[4] * glm::vec4(0, 1, 0, 0)) - glm::vec3(r[1] * glm::vec4(0, 1, 0, 0))));
    }
    std::printf("retarget proportions: end effectors at the keys %.7f m, between them %.7f m, unmapped joint %.7f\n",
                atKeys, between, chest);
    CHECK(atKeys < 1e-5f, "at the keys: largest end effector error %.6f m", atKeys);
    CHECK(between < 5e-4f, "between the keys: largest end effector error %.6f m", between);
    CHECK(chest < 1e-5f, "the unmapped joint keeps its rest relative to its parent (%.6f)", chest);
}

QUENCH_TEST(cook_retarget_mannequin_onto_strider) {
    if (kCharacters.empty()) QUENCH_SKIP("the build found no Python: the demo's characters were not written");
    const std::filesystem::path mannequin = kCharacters / "mannequin.gltf", strider = kCharacters / "strider.gltf";
    Result<CookedAnimation> mSkeleton = cookSkeleton(mannequin), sSkeleton = cookSkeleton(strider);
    Result<CookedAnimation> mClips = cookAnimation(mannequin, {});
    AnimationOptions options;
    options.skeleton = strider;
    options.retarget = kCharacters / "strider.retarget.json";
    Result<CookedAnimation> sClips = cookAnimation(mannequin, options);
    REQUIRE(mSkeleton && sSkeleton && mClips && sClips, "%s",
            !mSkeleton   ? mSkeleton.error().c_str()
            : !sSkeleton ? sSkeleton.error().c_str()
            : !mClips    ? mClips.error().c_str()
            : sClips     ? ""
                         : sClips.error().c_str());
    CHECK(sClips->names == mClips->names, "the same %zu clips", sClips->names.size());
    CHECK(std::ranges::any_of(sClips->notes, [](const std::string& n) { return n.find("Spine2") != n.npos; }),
          "the unmapped Spine2 is noted");
    CHECK(std::ranges::any_of(sClips->inputs, [](const auto& p) { return p.filename() == "strider.retarget.json"; }),
          "the bone map is an input (%zu inputs)", sClips->inputs.size());
    Result<CookedAnimation> again = cookAnimation(mannequin, options);
    CHECK(again && again->file == sClips->file, "two cooks write the same bytes");
    AnimationOptions noSkeleton;
    noSkeleton.retarget = options.retarget;
    CHECK(!cookAnimation(mannequin, noSkeleton), "a bone map without the target's skeleton");

    // Two cycles of the walk at 120 Hz on both characters, through the runtime.
    Result<std::shared_ptr<const animation::ClipSet>> set = animation::readClipSet(mClips->file);
    REQUIRE(set && (*set)->find("walk"), "the walk");
    const float dt = 1.0f / 120.0f, period = (*set)->find("walk")->duration();
    const int frames = int(std::lround(2.0f * period / dt));
    const char* const mJoints[] = {"pelvis",     "thigh_l", "foot_l",     "thigh_r", "foot_r",
                                   "upperarm_l", "hand_l",  "upperarm_r", "hand_r"};
    const char* const sJoints[] = {"Hips",    "LeftUpLeg", "LeftFoot", "RightUpLeg", "RightFoot",
                                   "LeftArm", "LeftHand",  "RightArm", "RightHand"};
    const auto m = playCooked(tmpPath("retarget_mannequin"), *mSkeleton, *mClips, "walk", mJoints, frames, dt);
    const auto s = playCooked(tmpPath("retarget_strider"), *sSkeleton, *sClips, "walk", sJoints, frames, dt);
    REQUIRE(m.size() == std::size_t(frames + 1) && s.size() == m.size(), "played %zu and %zu frames", m.size(),
            s.size());

    // The strider's legs and hips are 1.15 times the mannequin's, its arms 1.1 times: its hips' height, its feet in
    // world space (with the root motion), its feet from its hip joints and its hands from its shoulders scale so.
    constexpr float kLeg = 1.15f, kArm = 1.1f;
    float hips = 0.0f, feet = 0.0f, legs = 0.0f, arms = 0.0f;
    for (std::size_t f = 0; f < m.size(); ++f) {
        hips = std::max(hips, std::abs(s[f][0].y - kLeg * m[f][0].y));
        for (std::size_t side = 0; side < 2; ++side) {
            const std::size_t hip = 1 + 2 * side, foot = 2 + 2 * side, arm = 5 + 2 * side, hand = 6 + 2 * side;
            feet = std::max(feet, glm::length(s[f][foot] - kLeg * m[f][foot]));
            legs = std::max(legs, glm::length((s[f][foot] - s[f][hip]) - kLeg * (m[f][foot] - m[f][hip])));
            arms = std::max(arms, glm::length((s[f][hand] - s[f][arm]) - kArm * (m[f][hand] - m[f][arm])));
        }
    }
    const glm::vec3 mTravel = m.back()[0] - m.front()[0], sTravel = s.back()[0] - s.front()[0];
    // Foot contact: the frames where the mannequin's foot stays within 0.5 mm of the last frame's position (planted),
    // how far each foot moves per frame over them, and how far it gets from where its run of planted frames started
    // (the drift of a stance).
    int planted = 0, stances = 0;
    float mSlide = 0.0f, sSlide = 0.0f, mDrift = 0.0f, sDrift = 0.0f;
    for (const std::size_t foot : {std::size_t(2), std::size_t(4)}) {
        constexpr std::size_t kNone = ~std::size_t(0);
        std::size_t start = kNone;
        for (std::size_t f = 1; f < m.size(); ++f) {
            if (glm::length(m[f][foot] - m[f - 1][foot]) >= 5e-4f) {
                start = kNone;
                continue;
            }
            if (start == kNone) {
                start = f - 1;
                ++stances;
            }
            ++planted;
            mSlide = std::max(mSlide, glm::length(m[f][foot] - m[f - 1][foot]));
            sSlide = std::max(sSlide, glm::length(s[f][foot] - s[f - 1][foot]));
            mDrift = std::max(mDrift, glm::length(m[f][foot] - m[start][foot]));
            sDrift = std::max(sDrift, glm::length(s[f][foot] - s[start][foot]));
        }
    }
    std::printf("retarget walk: hips %.4f, feet %.4f, legs %.4f, arms %.4f m; travel %.4f / %.4f m; %d planted "
                "frames in %d stances, slide per frame %.5f / %.5f m, drift %.4f / %.4f m (strider / mannequin)\n",
                hips, feet, legs, arms, glm::length(sTravel), glm::length(mTravel), planted, stances, sSlide, mSlide,
                sDrift, mDrift);
    CHECK(hips < 3e-3f, "the hips' height: largest error %.4f m", hips);
    CHECK(legs < 5e-3f, "the feet from the hip joints: largest error %.4f m", legs);
    CHECK(arms < 5e-3f, "the hands from the shoulders: largest error %.4f m", arms);
    CHECK(feet < 1.5e-2f, "the feet in world space: largest error %.4f m", feet);
    CHECK(std::abs(glm::length(sTravel) - kLeg * glm::length(mTravel)) < 5e-3f, "two cycles: %.4f m, expected %.4f m",
          glm::length(sTravel), kLeg * glm::length(mTravel));
    CHECK(planted > frames / 2, "planted foot frames: %d of %d", planted, 2 * frames);
    CHECK(sSlide < 1e-3f, "the strider's planted feet: largest motion per frame %.5f m", sSlide);
    CHECK(sDrift < kLeg * mDrift + 3e-3f, "the strider's stances drift %.4f m, the mannequin's %.4f m", sDrift, mDrift);
}

QUENCH_TEST(cook_skinned_model) {
    JobSystem jobs(JobSystemDesc{.workerThreads = 0});
    const std::filesystem::path gltf = writeSkinnedGltf(tmpPath("cook_skinned_model"), "");
    Result<CookedModel> cooked = cookModel(gltf, "m", {}, jobs);
    REQUIRE(cooked, "%s", cooked ? "" : cooked.error().c_str());
    const assets::ModelData& model = cooked->model;
    REQUIRE(model.skins.size() == 1 && model.meshes.size() == 1, "%zu skins, %zu meshes", model.skins.size(),
            model.meshes.size());
    CHECK(model.skins[0].joints == (std::vector<std::string>{"root", "hip", "knee"}), "the skin's joints");
    CHECK(model.skins[0].inverseBindMatrices.size() == 3 &&
              std::abs(model.skins[0].inverseBindMatrices[1][3].y + 1.0f) < 1e-6f,
          "inverse bind matrices");
    const assets::ModelMesh& mesh = model.meshes[0];
    REQUIRE(mesh.skin == 0 && mesh.skinning.size() == mesh.vertices.size(), "skinned mesh: skin %d, %zu weights",
            mesh.skin, mesh.skinning.size());
    bool sums = true, split = false;
    for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
        const assets::ModelSkinVertex& w = mesh.skinning[v];
        sums = sums && w.weights[0] + w.weights[1] + w.weights[2] + w.weights[3] == 65535;
        if (mesh.vertices[v].position.y < 0.5f)
            split = split || (w.joints[0] == 2 && w.weights[0] == 49151 && w.joints[1] == 1 && w.weights[1] == 16384);
    }
    CHECK(sums && split, "weights sum to 65535 (%d), 0.75 / 0.25 quantized (%d)", sums, split);
    Result<assets::ModelData> read = assets::readModel(modelBytes(model));
    CHECK(read && read->skins.size() == 1 && read->meshes[0].skinning.size() == mesh.skinning.size(),
          "a skinned model reads back");
    Result<CookedModel> scaled = cookModel(gltf, "m", {.scale = 2.0f}, jobs);
    CHECK(!scaled && scaled.error().find("--scale") != std::string::npos, "scale with skins: %s",
          scaled ? "accepted" : scaled.error().c_str());
}

// A quad with uvs: MikkTSpace tangents where the file has none (along +u, the bitangent toward -v), the file's own
// where it has TANGENT (here deliberately not MikkTSpace's), turned around under a mirroring node.
QUENCH_TEST(cook_model_tangents) {
    JobSystem jobs(JobSystemDesc{.workerThreads = 0});
    const std::filesystem::path dir = tmpPath("cook_tangents");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto write = [&](const char* name, bool tangents, const char* node) {
        GltfWriter w;
        const int positions = w.add(std::vector<float>{0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1}, 4, "VEC3", kFloat,
                                    R"(, "min": [0, 0, 0], "max": [1, 0, 1])");
        const int normals = w.add(std::vector<float>{0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0}, 4, "VEC3", kFloat);
        const int uvs = w.add(std::vector<float>{0, 0, 1, 0, 0, 1, 1, 1}, 4, "VEC2", kFloat);
        const int given =
            w.add(std::vector<float>{0, 0, 1, -1, 0, 0, 1, -1, 0, 0, 1, -1, 0, 0, 1, -1}, 4, "VEC4", kFloat);
        const int indices = w.add(std::vector<std::uint16_t>{0, 2, 1, 1, 2, 3}, 6, "SCALAR", kUnsignedShort);
        const std::string attributes =
            std::format(R"("POSITION": {}, "NORMAL": {}, "TEXCOORD_0": {}{})", positions, normals, uvs,
                        tangents ? std::format(R"(, "TANGENT": {})", given) : std::string());
        const std::string json = std::format(
            R"({{"asset": {{"version": "2.0"}}, "scene": 0, "scenes": [{{"nodes": [0]}}], "nodes": [{{"mesh": 0{}}}],
"meshes": [{{"primitives": [{{"attributes": {{{}}}, "indices": {}}}]}}],
"buffers": [{{"uri": "{}.bin", "byteLength": {}}}], "bufferViews": [{}], "accessors": [{}]}})",
            node, attributes, indices, name, w.bin.size(), join(w.views), join(w.accessors));
        std::ofstream(dir / std::format("{}.gltf", name), std::ios::binary) << json;
        std::ofstream(dir / std::format("{}.bin", name), std::ios::binary)
            .write(reinterpret_cast<const char*>(w.bin.data()), std::streamsize(w.bin.size()));
        return dir / std::format("{}.gltf", name);
    };
    const auto tangentsOf = [&](const std::filesystem::path& gltf) {
        Result<CookedModel> cooked = cookModel(gltf, "m", {.lods = 1}, jobs);
        std::vector<glm::vec4> out;
        if (cooked && cooked->model.meshes.size() == 1)
            for (const assets::ModelVertex& v : cooked->model.meshes[0].vertices) out.push_back(v.tangent);
        return out;
    };
    const auto all = [](const std::vector<glm::vec4>& tangents, const glm::vec4& expected) {
        bool same = !tangents.empty();
        for (const glm::vec4& t : tangents) same = same && glm::length(t - expected) < 1e-4f;
        return same;
    };
    CHECK(all(tangentsOf(write("generated", false, "")), {1, 0, 0, 1}), "generated: +u with w 1");
    CHECK(all(tangentsOf(write("given", true, "")), {0, 0, 1, -1}), "the file's tangents kept");
    CHECK(all(tangentsOf(write("mirrored", true, R"(, "scale": [-1, 1, 1])")), {0, 0, 1, 1}),
          "a mirroring node turns w around");
}

// Blend shapes: a 3 x 3 grid on the xz plane, its center vertex doubled (the copy used by one quad, its "bump" offset
// different, so welding must keep it), two targets ("bump" from extras.targetNames: the center up, normal and
// tangent offsets; the second unnamed: a shear in x, positions only), on two nodes (the second scaled by 2 in x, its
// own weights).
QUENCH_TEST(cook_model_morph_targets) {
    JobSystem jobs(JobSystemDesc{.workerThreads = 0});
    const std::filesystem::path dir = tmpPath("cook_morph_targets");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::vector<glm::vec3> grid;
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) grid.push_back({0.5f * float(i), 0.0f, 0.5f * float(j)});
    grid.push_back(grid[4]);
    const auto bumpNormal = [](const glm::vec3& p) { return 0.6f * glm::vec3(p.x - 0.5f, 0.0f, p.z - 0.5f); };
    const auto write = [&](const char* name, bool given) {
        GltfWriter w;
        std::vector<float> positions, normals, uvs, tangents, bump, bumpNormals, bumpTangents, shear;
        for (std::size_t v = 0; v < grid.size(); ++v) {
            const glm::vec3 p = grid[v];
            positions.insert(positions.end(), {p.x, p.y, p.z});
            normals.insert(normals.end(), {0, 1, 0});
            uvs.insert(uvs.end(), {p.x, p.z});
            tangents.insert(tangents.end(), {1, 0, 0, 1});
            bump.insert(bump.end(), {0, v == 4 ? 0.5f : v == 9 ? 0.25f : 0.0f, 0});
            const glm::vec3 n = bumpNormal(p);
            bumpNormals.insert(bumpNormals.end(), {n.x, n.y, n.z});
            bumpTangents.insert(bumpTangents.end(), {0, 0, 0.5f});
            shear.insert(shear.end(), {0.1f * p.z, 0, 0});
        }
        std::vector<std::uint16_t> indices;
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const auto at = [&](int x, int z) {
                    const int v = z * 3 + x;
                    return std::uint16_t(v == 4 && i == 1 && j == 1 ? 9 : v);
                };
                indices.insert(indices.end(),
                               {at(i, j), at(i, j + 1), at(i + 1, j), at(i + 1, j), at(i, j + 1), at(i + 1, j + 1)});
            }
        const int count = int(grid.size());
        const std::string bounds = R"(, "min": [0, 0, 0], "max": [1, 0, 1])";
        const int p = w.add(positions, count, "VEC3", kFloat, bounds);
        const int n = w.add(normals, count, "VEC3", kFloat);
        const int uv = w.add(uvs, count, "VEC2", kFloat);
        const int t = w.add(tangents, count, "VEC4", kFloat);
        const int b = w.add(bump, count, "VEC3", kFloat, R"(, "min": [0, 0, 0], "max": [0, 0.5, 0])");
        const int bn = w.add(bumpNormals, count, "VEC3", kFloat);
        const int bt = w.add(bumpTangents, count, "VEC3", kFloat);
        const int s = w.add(shear, count, "VEC3", kFloat, R"(, "min": [0, 0, 0], "max": [0.1, 0, 0])");
        const int i = w.add(indices, int(indices.size()), "SCALAR", kUnsignedShort);
        const std::string attributes =
            given ? std::format(R"("POSITION": {}, "NORMAL": {}, "TEXCOORD_0": {}, "TANGENT": {})", p, n, uv, t)
                  : std::format(R"("POSITION": {}, "TEXCOORD_0": {})", p, uv);
        const std::string bumpTarget =
            given ? std::format(R"({{"POSITION": {}, "NORMAL": {}, "TANGENT": {}}})", b, bn, bt)
                  : std::format(R"({{"POSITION": {}}})", b);
        const std::string json = std::format(
            R"({{"asset": {{"version": "2.0"}}, "scene": 0, "scenes": [{{"nodes": [0, 1]}}],
"nodes": [{{"mesh": 0}}, {{"mesh": 0, "translation": [3, 0, 0], "scale": [2, 1, 1], "weights": [0, 1]}}],
"meshes": [{{"name": "sheet", "weights": [0.25, 0], "extras": {{"targetNames": ["bump"]}},
  "primitives": [{{"attributes": {{{}}}, "indices": {}, "targets": [{}, {{"POSITION": {}}}]}}]}}],
"buffers": [{{"uri": "{}.bin", "byteLength": {}}}], "bufferViews": [{}], "accessors": [{}]}})",
            attributes, i, bumpTarget, s, name, w.bin.size(), join(w.views), join(w.accessors));
        std::ofstream(dir / std::format("{}.gltf", name), std::ios::binary) << json;
        std::ofstream(dir / std::format("{}.bin", name), std::ios::binary)
            .write(reinterpret_cast<const char*>(w.bin.data()), std::streamsize(w.bin.size()));
        return dir / std::format("{}.gltf", name);
    };

    Result<CookedModel> cooked = cookModel(write("given", true), "m", {}, jobs);
    REQUIRE(cooked, "%s", cooked ? "" : cooked.error().c_str());
    REQUIRE(cooked->model.meshes.size() == 2, "%zu meshes (one per node)", cooked->model.meshes.size());
    for (std::size_t node = 0; node < 2; ++node) {
        const assets::ModelMesh& mesh = cooked->model.meshes[node];
        REQUIRE(mesh.vertices.size() == 10 && mesh.morphTargets.size() == 2, "node %zu: %zu vertices, %zu targets",
                node, mesh.vertices.size(), mesh.morphTargets.size());
        const assets::ModelMorphTarget& bump = mesh.morphTargets[0];
        const assets::ModelMorphTarget& shear = mesh.morphTargets[1];
        CHECK(bump.name == "bump" && shear.name == "sheet.target1", "node %zu: target names '%s', '%s'", node,
              bump.name.c_str(), shear.name.c_str());
        const float scaleX = node == 0 ? 1.0f : 2.0f;
        CHECK(bump.weight == (node == 0 ? 0.25f : 0.0f) && shear.weight == (node == 0 ? 0.0f : 1.0f),
              "node %zu: default weights %.2f, %.2f (the mesh's, the node's own)", node, bump.weight, shear.weight);
        bool offsets = bump.offsets.size() == 10 && shear.offsets.size() == 10, centers = true;
        int up = 0, half = 0;
        float normalError = 0.0f, tangentError = 0.0f;
        for (std::size_t v = 0; offsets && v < 10; ++v) {
            const assets::ModelVertex& vertex = mesh.vertices[v];
            const glm::vec3 source((vertex.position.x - (node == 0 ? 0.0f : 3.0f)) / scaleX, 0.0f, vertex.position.z);
            const bool center = glm::length(source - grid[4]) < 1e-5f;
            const float dy = bump.offsets[v].position.y;
            if (center) {
                up += dy == 0.5f;
                half += dy == 0.25f;
            } else
                centers = centers && dy == 0.0f;
            offsets = offsets && bump.offsets[v].position.x == 0.0f &&
                      glm::length(shear.offsets[v].position - glm::vec3(scaleX * 0.1f * source.z, 0, 0)) < 1e-6f &&
                      shear.offsets[v].normal == glm::vec3(0.0f) && shear.offsets[v].tangent == glm::vec3(0.0f);
            // The renderer's normalize(n + dn) and normalize(t + dt) are the file's morphed vectors, transformed.
            const glm::vec3 inverseTranspose(1.0f / scaleX, 1.0f, 1.0f), linear(scaleX, 1.0f, 1.0f);
            const glm::vec3 normal = glm::normalize(inverseTranspose * (glm::vec3(0, 1, 0) + bumpNormal(source)));
            const glm::vec3 tangent = glm::normalize(linear * glm::vec3(1, 0, 0.5f));
            normalError =
                std::max(normalError, glm::length(glm::normalize(vertex.normal + bump.offsets[v].normal) - normal));
            tangentError =
                std::max(tangentError,
                         glm::length(glm::normalize(glm::vec3(vertex.tangent) + bump.offsets[v].tangent) - tangent));
        }
        CHECK(offsets && centers && up == 1 && half == 1,
              "node %zu: position offsets (the doubled center kept apart: %d at 0.5, %d at 0.25)", node, up, half);
        CHECK(normalError < 1e-5f && tangentError < 1e-5f, "node %zu: morphed normals off by %.2e, tangents by %.2e",
              node, normalError, tangentError);
    }
    Result<assets::ModelData> read = assets::readModel(modelBytes(cooked->model));
    CHECK(read && read->meshes.size() == 2 && read->meshes[1].morphTargets.size() == 2 &&
              read->meshes[1].morphTargets[1].offsets[3].position ==
                  cooked->model.meshes[1].morphTargets[1].offsets[3].position,
          "the cooked blend shapes read back");

    // Without the file's normals the targets' normals are computed like the base's, from the moved positions (the
    // shear keeps the plane flat); without its tangents (MikkTSpace) the tangent offsets are 0.
    Result<CookedModel> computed = cookModel(write("computed", false), "m", {.lods = 1}, jobs);
    REQUIRE(computed && computed->model.meshes.size() == 2, "%s",
            computed ? "not two meshes" : computed.error().c_str());
    const assets::ModelMesh& mesh = computed->model.meshes[0];
    REQUIRE(mesh.morphTargets.size() == 2 && mesh.morphTargets[0].offsets.size() == mesh.vertices.size(), "%zu targets",
            mesh.morphTargets.size());
    bool unit = true, flat = true, tangents = true, tilted = false;
    for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
        const glm::vec3 n = mesh.vertices[v].normal + mesh.morphTargets[0].offsets[v].normal;
        unit = unit && std::abs(glm::length(n) - 1.0f) < 1e-4f;
        flat = flat && glm::length(mesh.morphTargets[1].offsets[v].normal) < 1e-6f;
        tangents = tangents && mesh.morphTargets[0].offsets[v].tangent == glm::vec3(0.0f);
        // The edge's middle vertex at z = 0 faces away from the raised center.
        if (glm::length(mesh.vertices[v].position - glm::vec3(0.5f, 0.0f, 0.0f)) < 1e-5f) tilted = n.z < -0.1f;
    }
    CHECK(unit && flat && tangents && tilted, "computed: unit %d, the shear flat %d, tangent offsets 0 %d, tilted %d",
          unit, flat, tangents, tilted);

    // The rest of the cook is unchanged: a mesh without targets has none.
    Result<CookedModel> crate = cookModel(kDemoSources / "models/crate.gltf", "models/crate", {}, jobs);
    CHECK(crate && crate->model.meshes.size() == 1 && crate->model.meshes[0].morphTargets.empty(),
          "a model without blend shapes");
}
