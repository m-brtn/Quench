// 3D textures filled from the layers of a 2D array, and cubes as color targets (the clouds' noise and the sky cube):
// every slice and face drawn, then sampled back through a 2D target.

#include "GpuTest.h"

#include "volume_shaders.h"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace quench;
using namespace quench::gpu;
using test::Rgba8;

namespace {

constexpr TextureUsage kTargetSampled = TextureUsage::ColorTarget | TextureUsage::Sampled;

Result<GraphicsPipeline> fullscreenPipeline(Device& device, const char* fragment, TextureFormat format) {
    const ColorTargetDesc target{.format = format};
    return device.createPipeline({.vertexShader = shaders::volume.find("fullscreen_vs"),
                                  .fragmentShader = shaders::volume.find(fragment),
                                  .colorTargets = {&target, 1}});
}

Rgba8 toRgba8(const glm::vec3& c) {
    const auto q = [](float v) { return std::uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return {q(c.x), q(c.y), q(c.z), 255};
}

// The direction cube_sample_fs samples for a face and row (volume.hlsl's table).
glm::vec3 testDirection(int face, int row) {
    static const glm::vec3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const glm::vec3 firsts[6] = {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}};
    static const glm::vec3 seconds[6] = {{0, 0, 1}, {0, 1, 0}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {1, 0, 0}};
    glm::vec3 d = axes[face];
    if (row == 1) d += firsts[face] * 0.5f;
    if (row == 2) d -= seconds[face] * 0.5f;
    return d;
}

} // namespace

// Each layer of an Rgba8Unorm 2D array drawn with its own color and copied into a slice of a 3D texture, then each
// slice sampled at its center. (Rendering into the slices themselves needs 2D views of a 3D texture, which MoltenVK's
// portability subset does not have.)
QUENCH_TEST(texture_3d_from_layers) {
    QUENCH_GPU_DEVICE(device);
    constexpr std::uint32_t kSize = 8, kDepth = 4;
    REQUIRE(device.supportsFormat(TextureFormat::Rgba8Unorm, TextureType::Texture3D, TextureUsage::Sampled),
            "no Rgba8Unorm 3D texture");
    Result<Texture> layers = device.createTexture({.type = TextureType::Texture2DArray,
                                                   .usage = TextureUsage::ColorTarget,
                                                   .width = kSize,
                                                   .height = kSize,
                                                   .layers = kDepth,
                                                   .name = "layers"});
    Result<Texture> volume = device.createTexture({.type = TextureType::Texture3D,
                                                   .usage = TextureUsage::Sampled,
                                                   .width = kSize,
                                                   .height = kSize,
                                                   .layers = kDepth,
                                                   .name = "volume"});
    Result<Texture> target =
        device.createTexture({.usage = TextureUsage::ColorTarget, .width = kSize, .height = kSize});
    Result<Sampler> sampler = device.createSampler({.minFilter = Filter::Nearest,
                                                    .magFilter = Filter::Nearest,
                                                    .mipmapMode = MipmapMode::Nearest,
                                                    .addressU = AddressMode::ClampToEdge,
                                                    .addressV = AddressMode::ClampToEdge,
                                                    .addressW = AddressMode::ClampToEdge});
    Result<GraphicsPipeline> flat = fullscreenPipeline(device, "flat_fs", TextureFormat::Rgba8Unorm);
    Result<GraphicsPipeline> read = fullscreenPipeline(device, "volume_fs", TextureFormat::Rgba8Unorm);
    REQUIRE(layers && volume && target && sampler && flat && read, "creation failed");

    const auto sliceColor = [](std::uint32_t z) {
        return Rgba8{std::uint8_t(40 + z * 60), std::uint8_t(220 - z * 50), std::uint8_t(z * 30), 255};
    };
    {
        CommandBuffer commands = device.acquireCommandBuffer();
        for (std::uint32_t z = 0; z < kDepth; ++z) {
            RenderPass pass = commands.beginRenderPass(ColorTarget{.texture = *layers, .layer = z});
            REQUIRE(pass, "layer %u", z);
            const Rgba8 c = sliceColor(z);
            const float color[4] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f};
            pass.bindPipeline(*flat);
            pass.pushUniforms(ShaderStage::Fragment, 0, color);
            pass.draw(3);
        }
        {
            CopyPass copy = commands.beginCopyPass();
            REQUIRE(copy, "copy pass");
            for (std::uint32_t z = 0; z < kDepth; ++z)
                copy.copy({.texture = *layers, .layer = z}, {.texture = *volume, .z = z});
        }
        REQUIRE(commands.submit(), "submit");
    }
    for (std::uint32_t z = 0; z < kDepth; ++z) {
        CommandBuffer commands = device.acquireCommandBuffer();
        {
            RenderPass pass = commands.beginRenderPass(ColorTarget{.texture = *target});
            const float depth[4] = {(float(z) + 0.5f) / float(kDepth), 0.0f, 0.0f, 0.0f};
            pass.bindPipeline(*read);
            pass.bindSampler(ShaderStage::Fragment, 0, {*volume, *sampler});
            pass.pushUniforms(ShaderStage::Fragment, 0, depth);
            pass.draw(3);
        }
        REQUIRE(commands.submit(), "submit");
        const std::vector<Rgba8> p = test::readRgba8(device, *target);
        REQUIRE(p.size() == kSize * kSize, "%zu pixels", p.size());
        const Rgba8 want = sliceColor(z);
        for (std::uint32_t i = 0; i < p.size(); ++i)
            CHECK(test::near(p[i], want, 1), "slice %u pixel %u: %d %d %d, expected %d %d %d", z, i, p[i].r, p[i].g,
                  p[i].b, want.r, want.g, want.b);
    }
}

// Each face of an Rgba16Float cube drawn with quenchCubeDirection (quench/Cube.hlsli) as its color, then sampled
// along directions off each face's center (both tangents): the sample is the direction when the face table matches
// the hardware's. Then its mips: the last level of a face is the mean of its texels.
QUENCH_TEST(cube_faces) {
    QUENCH_GPU_DEVICE(device);
    constexpr std::uint32_t kSize = 16;
    REQUIRE(device.supportsFormat(TextureFormat::Rgba16Float, TextureType::Cube, kTargetSampled),
            "no renderable Rgba16Float cube");
    const std::uint32_t mips = fullMipCount(kSize, kSize);
    Result<Texture> cube = device.createTexture({.type = TextureType::Cube,
                                                 .format = TextureFormat::Rgba16Float,
                                                 .usage = kTargetSampled,
                                                 .width = kSize,
                                                 .height = kSize,
                                                 .layers = 6,
                                                 .mipLevels = mips,
                                                 .name = "cube"});
    Result<Texture> target = device.createTexture({.usage = TextureUsage::ColorTarget, .width = 6, .height = 3});
    Result<Sampler> sampler = device.createSampler({.addressU = AddressMode::ClampToEdge,
                                                    .addressV = AddressMode::ClampToEdge,
                                                    .addressW = AddressMode::ClampToEdge});
    Result<GraphicsPipeline> face = fullscreenPipeline(device, "cube_face_fs", TextureFormat::Rgba16Float);
    Result<GraphicsPipeline> read = fullscreenPipeline(device, "cube_sample_fs", TextureFormat::Rgba8Unorm);
    REQUIRE(cube && target && sampler && face && read, "creation failed");

    const auto sample = [&](float level) {
        CommandBuffer commands = device.acquireCommandBuffer();
        {
            RenderPass pass = commands.beginRenderPass(ColorTarget{.texture = *target});
            const float uniforms[4] = {level, 0.0f, 0.0f, 0.0f};
            pass.bindPipeline(*read);
            pass.bindSampler(ShaderStage::Fragment, 0, {*cube, *sampler});
            pass.pushUniforms(ShaderStage::Fragment, 0, uniforms);
            pass.draw(3);
        }
        if (!commands.submit()) return std::vector<Rgba8>{};
        return test::readRgba8(device, *target);
    };
    {
        CommandBuffer commands = device.acquireCommandBuffer();
        for (std::uint32_t f = 0; f < 6; ++f) {
            RenderPass pass = commands.beginRenderPass(ColorTarget{.texture = *cube, .layer = f});
            REQUIRE(pass, "face %u", f);
            const float uniforms[4] = {float(f), 0.0f, 0.0f, 0.0f};
            pass.bindPipeline(*face);
            pass.pushUniforms(ShaderStage::Fragment, 0, uniforms);
            pass.draw(3);
        }
        REQUIRE(commands.submit(), "submit");
    }
    std::vector<Rgba8> p = sample(0.0f);
    REQUIRE(p.size() == 18, "%zu pixels", p.size());
    for (int row = 0; row < 3; ++row)
        for (int f = 0; f < 6; ++f) {
            const Rgba8 got = p[std::size_t(row * 6 + f)];
            const Rgba8 want = toRgba8(glm::normalize(testDirection(f, row)) * 0.5f + 0.5f);
            CHECK(test::near(got, want, 6), "face %d row %d: %d %d %d, expected %d %d %d", f, row, got.r, got.g, got.b,
                  want.r, want.g, want.b);
        }

    {
        CommandBuffer commands = device.acquireCommandBuffer();
        commands.generateMipmaps(*cube);
        REQUIRE(commands.submit(), "submit");
    }
    // The mean over a face of its texels' directions: the axis times the mean of 1 / |(1, u, v)|.
    double mean = 0.0;
    for (std::uint32_t y = 0; y < kSize; ++y)
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const double u = (x + 0.5) / kSize * 2.0 - 1.0, v = (y + 0.5) / kSize * 2.0 - 1.0;
            mean += 1.0 / std::sqrt(1.0 + u * u + v * v);
        }
    mean /= double(kSize * kSize);
    p = sample(float(mips - 1));
    REQUIRE(p.size() == 18, "%zu pixels", p.size());
    for (int f = 0; f < 6; ++f) {
        const Rgba8 got = p[std::size_t(f)];
        const Rgba8 want = toRgba8(testDirection(f, 0) * float(mean) * 0.5f + 0.5f);
        CHECK(test::near(got, want, 3), "face %d last mip: %d %d %d, expected %d %d %d", f, got.r, got.g, got.b, want.r,
              want.g, want.b);
    }
}
