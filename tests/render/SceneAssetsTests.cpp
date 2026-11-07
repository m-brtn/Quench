// SceneAssets on the GPU: cooked textures and models loaded through an AssetManager into a scene, checked by their
// pixels while loading, after loading and after a failure.

#include "GpuTest.h"

#include <quench/assets/AssetManager.h>
#include <quench/assets/Model.h>
#include <quench/assets/Texture.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Jobs.h>
#include <quench/render/MeshBuilder.h>
#include <quench/render/Projection.h>
#include <quench/render/Renderer.h>
#include <quench/render/SceneAssets.h>

#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace quench;
using namespace quench::render;
using test::Rgba8;

namespace quench::test {
// BasisTestData.cpp (tests/CMakeLists.txt): Basis Universal files of one color, 16 x 16 with mips, sRGB.
std::span<const std::uint8_t> basisGreenEtc1s(); // 20, 200, 20
std::span<const std::uint8_t> basisBlueUastc();  // 20, 20, 200
} // namespace quench::test

namespace {

constexpr std::uint32_t kSize = 64;

std::filesystem::path g_assets;

void writeFile(const std::string& path, std::span<const std::uint8_t> data) {
    std::filesystem::create_directories((g_assets / path).parent_path());
    std::ofstream(g_assets / path, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

// A clean asset directory per case, installed as the file system backend.
void useDir(const char* name) {
    g_assets = std::filesystem::path(quench::test::tmpDir()) / name / "assets";
    std::filesystem::remove_all(g_assets.parent_path());
    std::filesystem::create_directories(g_assets);
    fs::setBackend(fs::makeStdBackend(g_assets, {}));
}

// An RGBA8 KTX2 file of one color, with its mip chain.
std::vector<std::uint8_t> solidKtx2(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint32_t size) {
    assets::TextureImage image;
    image.format = assets::PixelFormat::Rgba8Srgb;
    image.width = image.height = size;
    for (std::uint32_t s = size; s > 0; s /= 2) {
        ++image.mipLevels;
        for (std::uint32_t i = 0; i < s * s; ++i) image.data.insert(image.data.end(), {r, g, b, 255});
    }
    Result<std::vector<std::uint8_t>> file = assets::writeKtx2(image);
    CHECK(file, "writeKtx2: %s", file ? "" : file.error().c_str());
    return file ? *file : std::vector<std::uint8_t>{};
}

// A BC7 KTX2 file of one color: mode 6 blocks with both endpoints (r7, g7, b7, a7 = 127, p-bit 1), every index 0.
std::vector<std::uint8_t> solidBc7Ktx2(std::uint32_t r7, std::uint32_t g7, std::uint32_t b7, std::uint32_t size) {
    std::array<std::uint8_t, 16> block{};
    std::uint32_t bit = 0;
    const auto put = [&](std::uint32_t value, std::uint32_t bits) {
        for (std::uint32_t i = 0; i < bits; ++i, ++bit)
            if (value >> i & 1u) block[bit / 8] |= std::uint8_t(1u << (bit % 8));
    };
    put(1u << 6, 7);
    for (std::uint32_t c : {r7, g7, b7, 127u}) {
        put(c, 7);
        put(c, 7);
    }
    put(1, 1);
    put(1, 1);
    assets::TextureImage image;
    image.format = assets::PixelFormat::Bc7Srgb;
    image.width = image.height = size;
    image.mipLevels = 1;
    for (std::uint32_t i = 0; i < (size / 4) * (size / 4); ++i)
        image.data.insert(image.data.end(), block.begin(), block.end());
    Result<std::vector<std::uint8_t>> file = assets::writeKtx2(image);
    CHECK(file, "writeKtx2: %s", file ? "" : file.error().c_str());
    return file ? *file : std::vector<std::uint8_t>{};
}

// An ASTC 4x4 sRGB KTX2 file of one color: void-extent (constant color) blocks, the 8-bit color in the high byte of
// each UNORM16 channel.
std::vector<std::uint8_t> solidAstcKtx2(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint32_t size) {
    std::array<std::uint8_t, 16> block{0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const std::uint8_t channels[] = {r, g, b, 255};
    for (int c = 0; c < 4; ++c) block[std::size_t(8 + 2 * c)] = block[std::size_t(9 + 2 * c)] = channels[c];
    assets::TextureImage image;
    image.format = assets::PixelFormat::Astc4x4Srgb;
    image.width = image.height = size;
    image.mipLevels = 1;
    for (std::uint32_t i = 0; i < (size / 4) * (size / 4); ++i)
        image.data.insert(image.data.end(), block.begin(), block.end());
    Result<std::vector<std::uint8_t>> file = assets::writeKtx2(image);
    CHECK(file, "writeKtx2: %s", file ? "" : file.error().c_str());
    return file ? *file : std::vector<std::uint8_t>{};
}

// A cooked model of one box (half extents 0.5, base at y = 0) with one material.
std::vector<std::uint8_t> boxModel(const assets::ModelMaterial& material) {
    const MeshData box = boxMesh(glm::vec3(0.5f));
    assets::ModelMesh mesh;
    mesh.name = "box";
    for (const Vertex& v : box.vertices)
        mesh.vertices.push_back({v.position + glm::vec3(0.0f, 0.5f, 0.0f), v.normal, v.uv, v.color});
    mesh.indices = box.indices;
    mesh.lods.push_back({0, std::uint32_t(box.indices.size()), 0.0f});
    mesh.boundsMin = glm::vec3(-0.5f, 0.0f, -0.5f);
    mesh.boundsMax = glm::vec3(0.5f, 1.0f, 0.5f);
    assets::ModelData model;
    model.meshes.push_back(std::move(mesh));
    model.materials.push_back(material);
    model.boundsMin = glm::vec3(-0.5f, 0.0f, -0.5f);
    model.boundsMax = glm::vec3(0.5f, 1.0f, 0.5f);
    Result<std::vector<std::uint8_t>> file = assets::writeModel(model);
    CHECK(file, "writeModel: %s", file ? "" : file.error().c_str());
    return file ? *file : std::vector<std::uint8_t>{};
}

RenderSettings plainSettings() {
    RenderSettings s;
    s.antiAliasing = AntiAliasing::None;
    s.bloom = false;
    s.shadowCascades = 0;
    s.vignette = 0.0f;
    s.dither = false;
    return s;
}

// Looking straight down from 9 m, +x to the right of the image.
View topView() {
    return {
        .view = glm::lookAt(glm::vec3(0, 9.0f, 0), glm::vec3(0), glm::vec3(0, 0, -1)), .fovY = 0.9f, .nearPlane = 0.1f};
}

glm::ivec2 pixelOf(const View& view, glm::vec3 p) {
    const glm::vec4 c = reversedPerspective(view.fovY, 1.0f, view.nearPlane) * view.view * glm::vec4(p, 1.0f);
    const glm::vec2 ndc = glm::vec2(c) / c.w;
    return {int((ndc.x * 0.5f + 0.5f) * float(kSize)), int((0.5f - ndc.y * 0.5f) * float(kSize))};
}

struct Frame {
    std::vector<Rgba8> pixels;

    Rgba8 at(glm::ivec2 p) const { return pixels[std::size_t(p.y) * kSize + std::size_t(p.x)]; }
    // Pixels within `radius` of p for which `test` holds.
    template <class Test>
    int count(glm::ivec2 p, int radius, Test test) const {
        int n = 0;
        for (int y = p.y - radius; y <= p.y + radius; ++y)
            for (int x = p.x - radius; x <= p.x + radius; ++x)
                if (test(at({x, y}))) ++n;
        return n;
    }
};

bool gray(Rgba8 c) { return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}) < 45 && c.g > 40; }
bool green(Rgba8 c) { return c.g > 2 * c.r && c.g > 2 * c.b && c.g > 40; }
bool red(Rgba8 c) { return c.r > 2 * c.g && c.r > 2 * c.b && c.r > 40; }
bool magenta(Rgba8 c) { return c.r > 2 * c.g && c.b > 2 * c.g && c.r > 40; }
bool blue(Rgba8 c) { return c.b > 2 * c.r && c.b > 2 * c.g && c.b > 40; }

struct Fixture {
    gpu::Device& device;
    Renderer renderer;
    Scene scene;
    gpu::Texture target;
    View view = topView();

    Frame render() {
        gpu::CommandBuffer commands = device.acquireCommandBuffer();
        renderer.render(scene, view, commands, target);
        CHECK(commands.submit(), "submit");
        return {test::readRgba8(device, target)};
    }
};

// Updates until nothing is loading (at most 5 s).
bool settle(assets::AssetManager& manager, SceneAssets& sceneAssets) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end) {
        manager.update();
        sceneAssets.update();
        if (manager.idle()) return true;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return false;
}

} // namespace

QUENCH_TEST(scene_assets_texture_states) {
    QUENCH_GPU_DEVICE(device);
    useDir("scene_assets_texture_states");
    writeFile("tex/green.ktx2", solidKtx2(20, 200, 20, 16));
    writeFile("tex/red_bc7.ktx2", solidBc7Ktx2(110, 8, 8, 8));
    writeFile("tex/blue_astc.ktx2", solidAstcKtx2(20, 20, 200, 8));
    {
        Result<Renderer> renderer = Renderer::create(device, plainSettings());
        REQUIRE(renderer, "%s", renderer.error().c_str());
        Result<gpu::Texture> target = device.createTexture({.format = gpu::TextureFormat::Rgba8Unorm,
                                                            .usage = gpu::TextureUsage::ColorTarget,
                                                            .width = kSize,
                                                            .height = kSize,
                                                            .name = "scene assets target"});
        REQUIRE(target, "%s", target.error().c_str());
        Fixture f{device, std::move(*renderer), Scene(device), std::move(*target)};
        f.scene.addLight({.type = LightType::Directional, .color = glm::vec3(3.0f), .direction = {0.0f, -1.0f, -0.2f}});

        JobSystem jobs(JobSystemDesc{.workerThreads = 1});
        assets::AssetManager manager(jobs);
        SceneAssets sceneAssets(f.scene, manager);
        const char* paths[] = {"tex/green.ktx2", "tex/red_bc7.ktx2", "tex/missing.ktx2", "tex/blue_astc.ktx2"};
        std::vector<assets::AssetRef<TextureAsset>> textures;
        Result<MeshId> plane = f.scene.addMesh(planeMesh(glm::vec2(1.0f)).desc("plane"));
        REQUIRE(plane, "%s", plane.error().c_str());
        std::array<glm::ivec2, 4> centers{};
        for (int i = 0; i < 4; ++i) {
            textures.push_back(sceneAssets.loadTexture(paths[i]));
            REQUIRE(textures.back()->texture(), "%s: no texture handle while loading", paths[i]);
            MaterialDesc material{.baseColor = glm::vec4(0.6f, 0.6f, 0.6f, 1.0f), .roughness = 1.0f};
            material.textures[0] = textures.back()->texture();
            const glm::vec3 center = i < 3 ? glm::vec3(float(i - 1) * 2.5f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 2.5f);
            f.scene.addInstance({.mesh = *plane,
                                 .material = f.scene.addMaterial(material),
                                 .transform = glm::translate(glm::mat4(1.0f), center)});
            centers[std::size_t(i)] = pixelOf(f.view, center);
        }

        // While loading every texture samples white.
        Frame frame = f.render();
        for (int i = 0; i < 4; ++i)
            CHECK(textures[std::size_t(i)].state() == assets::AssetState::Loading &&
                      gray(frame.at(centers[std::size_t(i)])),
                  "%s while loading: %s, %d %d %d", paths[i], stateName(textures[std::size_t(i)].state()),
                  frame.at(centers[std::size_t(i)]).r, frame.at(centers[std::size_t(i)]).g,
                  frame.at(centers[std::size_t(i)]).b);

        REQUIRE(settle(manager, sceneAssets), "the loads did not finish");
        frame = f.render();
        const Rgba8 g = frame.at(centers[0]), r = frame.at(centers[1]);
        CHECK(textures[0].ready() && green(g), "green texture: %s, %d %d %d", stateName(textures[0].state()), g.r, g.g,
              g.b);
        CHECK(textures[1].ready() && red(r), "BC7 texture: %s, %d %d %d (BC7 %s)", stateName(textures[1].state()), r.r,
              r.g, r.b, sceneAssets.bcTextures() ? "sampled" : "decoded");
        CHECK(textures[1]->format() ==
                      (sceneAssets.bcTextures() ? assets::PixelFormat::Bc7Srgb : assets::PixelFormat::Rgba8Srgb) &&
                  textures[1]->decodedOnCpu() == !sceneAssets.bcTextures(),
              "BC7 texture format %s", assets::pixelFormatInfo(textures[1]->format()).name);
        CHECK(textures[0]->width() == 16 && textures[0]->mipLevels() == 5, "green texture %ux%u, %u levels",
              textures[0]->width(), textures[0]->height(), textures[0]->mipLevels());
        const int checker = frame.count(centers[2], 3, magenta);
        CHECK(textures[2].state() == assets::AssetState::Failed && checker >= 25,
              "missing texture: %s, %d of 49 pixels magenta", stateName(textures[2].state()), checker);
        // ASTC: sampled where the device can, else an error and the placeholder (no decoder at run time).
        const Rgba8 a = frame.at(centers[3]);
        if (sceneAssets.astcTextures())
            CHECK(textures[3].ready() && blue(a) && textures[3]->format() == assets::PixelFormat::Astc4x4Srgb &&
                      !textures[3]->decodedOnCpu(),
                  "ASTC texture: %s, %s, %d %d %d", stateName(textures[3].state()),
                  textures[3].ready() ? assets::pixelFormatInfo(textures[3]->format()).name : "-", a.r, a.g, a.b);
        else
            CHECK(textures[3].state() == assets::AssetState::Failed && frame.count(centers[3], 3, magenta) >= 25,
                  "ASTC texture without ASTC sampling: %s", stateName(textures[3].state()));
        std::printf("ASTC %s: %d %d %d\n", sceneAssets.astcTextures() ? "sampled" : "not supported", a.r, a.g, a.b);
        textures.clear();
    }
    fs::setBackend(nullptr);
}

// Basis Universal files: transcoded on the loading job to what the device samples, and again on a reload (hot reload
// and the asset overlay replace a file the same way).
QUENCH_TEST(scene_assets_basis_textures) {
    QUENCH_GPU_DEVICE(device);
    useDir("scene_assets_basis_textures");
    writeFile("tex/green.ktx2", test::basisGreenEtc1s());
    writeFile("tex/blue.ktx2", test::basisBlueUastc());
    {
        Result<Renderer> renderer = Renderer::create(device, plainSettings());
        REQUIRE(renderer, "%s", renderer.error().c_str());
        Result<gpu::Texture> target = device.createTexture({.format = gpu::TextureFormat::Rgba8Unorm,
                                                            .usage = gpu::TextureUsage::ColorTarget,
                                                            .width = kSize,
                                                            .height = kSize,
                                                            .name = "scene assets target"});
        REQUIRE(target, "%s", target.error().c_str());
        Fixture f{device, std::move(*renderer), Scene(device), std::move(*target)};
        f.scene.addLight({.type = LightType::Directional, .color = glm::vec3(3.0f), .direction = {0.0f, -1.0f, -0.2f}});
        JobSystem jobs(JobSystemDesc{.workerThreads = 1});
        assets::AssetManager manager(jobs);
        SceneAssets sceneAssets(f.scene, manager);
        Result<MeshId> plane = f.scene.addMesh(planeMesh(glm::vec2(1.0f)).desc("plane"));
        REQUIRE(plane, "%s", plane.error().c_str());
        const char* paths[] = {"tex/green.ktx2", "tex/blue.ktx2"};
        const std::span<const std::uint8_t> files[] = {test::basisGreenEtc1s(), test::basisBlueUastc()};
        std::vector<assets::AssetRef<TextureAsset>> textures;
        std::array<glm::ivec2, 2> centers{};
        for (int i = 0; i < 2; ++i) {
            textures.push_back(sceneAssets.loadTexture(paths[i]));
            MaterialDesc material{.baseColor = glm::vec4(0.6f, 0.6f, 0.6f, 1.0f), .roughness = 1.0f};
            material.textures[0] = textures.back()->texture();
            const glm::vec3 center(float(i) * 2.5f - 1.25f, 0.0f, 0.0f);
            f.scene.addInstance({.mesh = *plane,
                                 .material = f.scene.addMaterial(material),
                                 .transform = glm::translate(glm::mat4(1.0f), center)});
            centers[std::size_t(i)] = pixelOf(f.view, center);
        }
        REQUIRE(settle(manager, sceneAssets), "the loads did not finish");
        Frame frame = f.render();
        const assets::BasisCodec codecs[] = {assets::BasisCodec::Etc1s, assets::BasisCodec::Uastc};
        for (std::size_t i = 0; i < 2; ++i) {
            const TextureAsset& texture = *textures[i];
            const Result<assets::BasisInfo> info = assets::readBasisInfo(files[i]);
            REQUIRE(info, "%s: %s", paths[i], info ? "" : info.error().c_str());
            const assets::PixelFormat expected = assets::basisTarget(*info, sceneAssets.textureSupport());
            const Rgba8 c = frame.at(centers[i]);
            CHECK(textures[i].ready() && (i == 0 ? green(c) : blue(c)), "%s: %s, %d %d %d", paths[i],
                  stateName(textures[i].state()), c.r, c.g, c.b);
            CHECK(texture.basisCodec() == codecs[i] && texture.format() == expected &&
                      texture.decodedOnCpu() == (assets::pixelFormatInfo(expected).blockSize == 1) &&
                      texture.width() == 16 && texture.mipLevels() == 5,
                  "%s: %s, %ux%u, %u levels, not %s", paths[i], assets::pixelFormatInfo(texture.format()).name,
                  texture.width(), texture.height(), texture.mipLevels(), assets::pixelFormatInfo(expected).name);
            std::printf("%s: Basis Universal %s transcoded to %s in %.3f ms\n", paths[i],
                        texture.basisCodec() ? assets::basisCodecName(*texture.basisCodec()) : "-",
                        assets::pixelFormatInfo(texture.format()).name, texture.decodeMilliseconds());
        }

        // The green file replaced by the blue UASTC one: transcoded again, for that file's codec.
        writeFile("tex/green.ktx2", test::basisBlueUastc());
        manager.reload(textures[0]->id());
        REQUIRE(settle(manager, sceneAssets), "the reload did not finish");
        frame = f.render();
        const Rgba8 c = frame.at(centers[0]);
        CHECK(textures[0]->version() >= 2 && blue(c) && textures[0]->basisCodec() == assets::BasisCodec::Uastc,
              "after the reload: version %u, %d %d %d, %s", textures[0]->version(), c.r, c.g, c.b,
              textures[0]->basisCodec() ? assets::basisCodecName(*textures[0]->basisCodec()) : "-");
        textures.clear();
    }
    fs::setBackend(nullptr);
}

QUENCH_TEST(scene_assets_model_states) {
    QUENCH_GPU_DEVICE(device);
    useDir("scene_assets_model_states");
    writeFile(
        "models/box.qmodel",
        boxModel(
            {.name = "green", .baseColor = glm::vec4(0.05f, 0.7f, 0.05f, 1.0f), .roughness = 1.0f, .metallic = 0.0f}));
    assets::ModelMaterial textured{.name = "textured", .roughness = 1.0f, .metallic = 0.0f};
    textured.baseColorTexture = "tex/red.ktx2";
    writeFile("models/textured.qmodel", boxModel(textured));
    writeFile("tex/red.ktx2", solidKtx2(200, 20, 20, 4));
    const std::uint8_t garbage[] = {'n', 'o', 't', ' ', 'a', ' ', 'm', 'o', 'd', 'e', 'l'};
    writeFile("models/broken.qmodel", garbage);
    {
        Result<Renderer> renderer = Renderer::create(device, plainSettings());
        REQUIRE(renderer, "%s", renderer.error().c_str());
        Result<gpu::Texture> target = device.createTexture({.format = gpu::TextureFormat::Rgba8Unorm,
                                                            .usage = gpu::TextureUsage::ColorTarget,
                                                            .width = kSize,
                                                            .height = kSize,
                                                            .name = "scene assets target"});
        REQUIRE(target, "%s", target.error().c_str());
        Fixture f{device, std::move(*renderer), Scene(device), std::move(*target)};
        f.scene.addLight({.type = LightType::Directional, .color = glm::vec3(3.0f), .direction = {0.0f, -1.0f, -0.2f}});
        const std::uint32_t baseMeshes = f.scene.meshCount();

        JobSystem jobs(JobSystemDesc{.workerThreads = 1});
        assets::AssetManager manager(jobs);
        auto sceneAssets = std::make_unique<SceneAssets>(f.scene, manager);
        const char* paths[] = {"models/box.qmodel", "models/textured.qmodel", "models/broken.qmodel"};
        std::array<ModelInstanceId, 3> placed{};
        std::array<glm::ivec2, 3> tops{};
        for (int i = 0; i < 3; ++i) {
            const glm::vec3 base(float(i - 1) * 2.0f, 0.0f, 0.0f);
            placed[std::size_t(i)] =
                sceneAssets->addModel(sceneAssets->loadModel(paths[i]), glm::translate(glm::mat4(1.0f), base));
            tops[std::size_t(i)] = pixelOf(f.view, base + glm::vec3(0.0f, 1.0f, 0.0f));
        }
        CHECK(sceneAssets->modelInstanceCount() == 3, "%u placed models", sceneAssets->modelInstanceCount());

        // While loading: gray boxes.
        Frame frame = f.render();
        for (int i = 0; i < 3; ++i) {
            const Rgba8 c = frame.at(tops[std::size_t(i)]);
            CHECK(gray(c), "%s while loading: %d %d %d", paths[i], c.r, c.g, c.b);
        }

        REQUIRE(settle(manager, *sceneAssets), "the loads did not finish");
        frame = f.render();
        const assets::AssetRef<ModelAsset>& box = sceneAssets->model(placed[0]);
        const assets::AssetRef<ModelAsset>& texturedBox = sceneAssets->model(placed[1]);
        const assets::AssetRef<ModelAsset>& broken = sceneAssets->model(placed[2]);
        const Rgba8 a = frame.at(tops[0]), b = frame.at(tops[1]), c = frame.at(tops[2]);
        CHECK(box.ready() && box->parts().size() == 1 && box->triangles() == 12 && green(a),
              "box: %s, %zu parts, %u triangles, %d %d %d", stateName(box.state()), box->parts().size(),
              box->triangles(), a.r, a.g, a.b);
        CHECK(texturedBox.ready() && texturedBox->textures().size() == 1 && texturedBox->textures()[0].ready() &&
                  red(b),
              "textured box: %s, %zu textures, %d %d %d", stateName(texturedBox.state()),
              texturedBox->textures().size(), b.r, b.g, b.b);
        CHECK(broken.state() == assets::AssetState::Failed && magenta(c) && !broken->error().empty(),
              "broken model: %s (%s), %d %d %d", stateName(broken.state()), broken->error().c_str(), c.r, c.g, c.b);

        // Removing the models and the SceneAssets gives back every mesh it made (the placeholder boxes too).
        for (ModelInstanceId id : placed) sceneAssets->removeModel(id);
        CHECK(sceneAssets->modelInstanceCount() == 0, "%u placed models left", sceneAssets->modelInstanceCount());
        sceneAssets.reset();
        CHECK(f.scene.meshCount() == baseMeshes, "%u meshes left of %u", f.scene.meshCount(), baseMeshes);
    }
    fs::setBackend(nullptr);
}

namespace {

// A cooked model of two boxes (half extents 0.5, base at y = 0), green at x -2 and red at x 2. The green one's blend
// shape "slide" (+1.5 m in x) is at weight 1 by default; the red one has "hop" (-2 m in z) and its own "slide" (+2 m
// in z), both at 0.
std::vector<std::uint8_t> morphingBoxes() {
    const MeshData box = boxMesh(glm::vec3(0.5f));
    assets::ModelData model;
    for (int part = 0; part < 2; ++part) {
        assets::ModelMesh mesh;
        mesh.name = part == 0 ? "green box" : "red box";
        mesh.material = std::uint32_t(part);
        const glm::vec3 at(part == 0 ? -2.0f : 2.0f, 0.5f, 0.0f);
        for (const Vertex& v : box.vertices) mesh.vertices.push_back({v.position + at, v.normal, v.uv, v.color});
        mesh.indices = box.indices;
        mesh.lods.push_back({0, std::uint32_t(box.indices.size()), 0.0f});
        mesh.boundsMin = at - glm::vec3(0.5f);
        mesh.boundsMax = at + glm::vec3(0.5f);
        const auto target = [&](const char* name, float weight, glm::vec3 offset) {
            assets::ModelMorphTarget t{.name = name, .weight = weight};
            t.offsets.assign(mesh.vertices.size(), assets::ModelMorphVertex{.position = offset});
            mesh.morphTargets.push_back(std::move(t));
        };
        if (part == 0)
            target("slide", 1.0f, {1.5f, 0.0f, 0.0f});
        else {
            target("hop", 0.0f, {0.0f, 0.0f, -2.0f});
            target("slide", 0.0f, {0.0f, 0.0f, 2.0f});
        }
        model.meshes.push_back(std::move(mesh));
    }
    model.materials.push_back(
        {.name = "green", .baseColor = glm::vec4(0.05f, 0.7f, 0.05f, 1.0f), .roughness = 1.0f, .metallic = 0.0f});
    model.materials.push_back(
        {.name = "red", .baseColor = glm::vec4(0.7f, 0.05f, 0.05f, 1.0f), .roughness = 1.0f, .metallic = 0.0f});
    model.boundsMin = glm::vec3(-2.5f, 0.0f, -0.5f);
    model.boundsMax = glm::vec3(2.5f, 1.0f, 0.5f);
    Result<std::vector<std::uint8_t>> file = assets::writeModel(model);
    CHECK(file, "writeModel: %s", file ? "" : file.error().c_str());
    return file ? *file : std::vector<std::uint8_t>{};
}

} // namespace

// Blend shapes of a model: by name across its parts, the file's defaults until set, kept across a reload.
QUENCH_TEST(scene_assets_blend_shapes) {
    QUENCH_GPU_DEVICE(device);
    useDir("scene_assets_blend_shapes");
    writeFile("models/boxes.qmodel", morphingBoxes());
    {
        Result<Renderer> renderer = Renderer::create(device, plainSettings());
        REQUIRE(renderer, "%s", renderer.error().c_str());
        Result<gpu::Texture> target = device.createTexture({.format = gpu::TextureFormat::Rgba8Unorm,
                                                            .usage = gpu::TextureUsage::ColorTarget,
                                                            .width = kSize,
                                                            .height = kSize,
                                                            .name = "scene assets target"});
        REQUIRE(target, "%s", target.error().c_str());
        Fixture f{device, std::move(*renderer), Scene(device), std::move(*target)};
        f.scene.addLight({.type = LightType::Directional, .color = glm::vec3(3.0f), .direction = {0.0f, -1.0f, -0.2f}});
        JobSystem jobs(JobSystemDesc{.workerThreads = 1});
        assets::AssetManager manager(jobs);
        SceneAssets sceneAssets(f.scene, manager);
        const assets::AssetRef<ModelAsset> model = sceneAssets.loadModel("models/boxes.qmodel");
        const ModelInstanceId placed = sceneAssets.addModel(model, glm::mat4(1.0f));
        sceneAssets.setMorphWeights(placed, std::array{0.0f}); // while loading: ignored
        REQUIRE(settle(manager, sceneAssets), "the load did not finish");
        REQUIRE(model.ready() && model->parts().size() == 2, "%s, %zu parts", stateName(model.state()),
                model->parts().size());
        CHECK(model->morphTargets().size() == 2 && model->morphTargets()[0] == "slide" &&
                  model->morphTargets()[1] == "hop" && model->morphDefaults()[0] == 1.0f &&
                  model->morphDefaults()[1] == 0.0f,
              "%zu blend shapes by name, the first part's defaults", model->morphTargets().size());
        CHECK(model->parts()[1].morphs == (std::vector<std::uint32_t>{1, 0}), "the red box's shapes by name");

        const auto top = [&](float x, float z) { return pixelOf(f.view, glm::vec3(x, 1.0f, z)); };
        const auto check = [&](const char* what, glm::vec2 greenAt, glm::vec2 redAt) {
            const Frame frame = f.render();
            const int g = frame.count(top(greenAt.x, greenAt.y), 1, green);
            const int r = frame.count(top(redAt.x, redAt.y), 1, red);
            CHECK(g == 9 && r == 9, "%s: green at %.1f %.1f (%d of 9 pixels), red at %.1f %.1f (%d of 9)", what,
                  double(greenAt.x), double(greenAt.y), g, double(redAt.x), double(redAt.y), r);
        };
        check("the file's defaults", {-0.5f, 0.0f}, {2.0f, 0.0f});
        sceneAssets.setMorphWeights(placed, std::array{0.0f, 1.0f});
        check("slide 0, hop 1", {-2.0f, 0.0f}, {2.0f, -2.0f});
        sceneAssets.setMorphWeights(placed, std::array{1.0f});
        check("slide 1 in both parts, hop 0", {-0.5f, 0.0f}, {2.0f, 2.0f});

        manager.reload(model->id());
        REQUIRE(settle(manager, sceneAssets), "the reload did not finish");
        CHECK(model->version() >= 2, "reloaded: version %u", model->version());
        check("after a reload, the weights kept", {-0.5f, 0.0f}, {2.0f, 2.0f});
    }
    fs::setBackend(nullptr);
}
