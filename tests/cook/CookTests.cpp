// quench_cook's library: textures (mips, BC7, the RGBA8 fallback, KTX2 pass-through), models (the demo's glTF and
// GLB sources), skeletons, clips and skinned models (a small glTF written by the test), and that the output does not
// depend on the number of threads.

#include "Test.h"

#include "Cook.h"

#include <quench/animation/AnimationSystem.h>
#include <quench/assets/AssetManager.h>
#include <quench/assets/Model.h>
#include <quench/assets/Texture.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Jobs.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
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

std::vector<Sampled> sampleCooked(const std::filesystem::path& dir, const CookedAnimation& skeleton,
                                  const CookedAnimation& clips, std::span<const float> phases) {
    std::ofstream(dir / "s.qskel", std::ios::binary)
        .write(reinterpret_cast<const char*>(skeleton.file.data()), std::streamsize(skeleton.file.size()));
    std::ofstream(dir / "s.qanim", std::ios::binary)
        .write(reinterpret_cast<const char*>(clips.file.data()), std::streamsize(clips.file.size()));
    std::ofstream(dir / "g.json", std::ios::binary)
        << R"({"layers": [{"name": "base", "states": [{"name": "s", "clip": "bend", "loop": false}]}]})";
    fs::setBackend(fs::makeStdBackend(dir, {}));
    std::vector<Sampled> out;
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
        for (const float phase : phases) {
            anim.setPhase(id, phase);
            manager.update();
            anim.update(0.0f);
            const std::span<const glm::mat4> m = anim.modelMatrices(id);
            const auto at = [&](const char* name) { return m[std::size_t(anim.findJoint(id, name))]; };
            if (!anim.state(id).bound || m.empty()) break;
            out.push_back({glm::vec3(at("root")[3]), glm::vec3(at("hip")[3]), glm::vec3(at("knee")[1])});
        }
    }
    fs::setBackend(nullptr);
    return out;
}

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
