#include "GpuTest.h"

#include "core/Platform.h"
#include "render_shaders.h"

#include <array>
#include <cstring>
#include <optional>
#include <vector>

using namespace quench;
using namespace quench::gpu;
using test::Rgba8;

namespace {

std::vector<std::byte> pattern(std::size_t size, unsigned seed) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) bytes[i] = std::byte((i * 7 + seed) & 0xff);
    return bytes;
}

std::span<const std::byte> asBytes(const std::vector<Rgba8>& pixels) { return std::as_bytes(std::span(pixels)); }

} // namespace

QUENCH_TEST(format_info) {
    CHECK(formatInfo(TextureFormat::Rgba8Unorm).blockBytes == 4, "Rgba8Unorm");
    CHECK(levelByteSize(TextureFormat::Rgba8Unorm, 5, 3) == 60, "Rgba8Unorm 5 x 3");
    CHECK(levelByteSize(TextureFormat::Rgba16Float, 4, 4, 6) == 768, "Rgba16Float 4 x 4 x 6");
    // Block formats round up to whole 4 x 4 blocks.
    CHECK(levelByteSize(TextureFormat::Bc7RgbaUnorm, 5, 5) == 64, "Bc7 5 x 5");
    CHECK(levelByteSize(TextureFormat::Bc1RgbaUnorm, 1, 1) == 8, "Bc1 1 x 1");
    CHECK(levelByteSize(TextureFormat::Astc4x4Unorm, 8, 4) == 32, "Astc4x4 8 x 4");
    CHECK(formatInfo(TextureFormat::Depth32FloatStencil8).depth &&
              formatInfo(TextureFormat::Depth32FloatStencil8).stencil,
          "Depth32FloatStencil8");
    CHECK(formatInfo(TextureFormat::Rgba8UnormSrgb).srgb && !formatInfo(TextureFormat::Rgba8Unorm).srgb, "sRGB flags");
    CHECK(fullMipCount(1, 1) == 1 && fullMipCount(256, 64) == 9 && fullMipCount(5, 3) == 3, "fullMipCount");
}

QUENCH_TEST(caps) {
    QUENCH_GPU_DEVICE(device);
    const Caps& caps = device.caps();
    std::printf(
        "  driver %s, device '%s', shaders %s, gpuTiming %d, depthClamp %d, depthBiasClamp %d, indirectDraw %d, "
        "BC %d, ASTC %d\n",
        caps.driver.c_str(), caps.deviceName.c_str(), shaderFormatName(caps.shaderFormat), caps.gpuTiming,
        caps.depthClamp, caps.depthBiasClamp, caps.indirectDraw, caps.bcTextures, caps.astcTextures);
    if (caps.driver == "metal") {
        CHECK(caps.shaderFormat == (QUENCH_TEST_METALLIB ? ShaderFormat::Metallib : ShaderFormat::Msl), "shaders %s",
              shaderFormatName(caps.shaderFormat));
        CHECK(caps.gpuTiming, "Metal without GPU timing");
        CHECK(caps.depthBiasClamp, "Metal without depth bias clamp");
        // The Simulator's Metal device has no depth clip mode and no indirect draws.
        CHECK(caps.depthClamp == !QUENCH_PLATFORM_IOS_SIMULATOR, "depthClamp %d", caps.depthClamp);
        CHECK(caps.indirectDraw == !QUENCH_PLATFORM_IOS_SIMULATOR, "indirectDraw %d", caps.indirectDraw);
    } else if (caps.driver == "vulkan") {
        CHECK(caps.shaderFormat == ShaderFormat::Spirv, "shaders %s", shaderFormatName(caps.shaderFormat));
        CHECK(!caps.gpuTiming && !caps.depthBiasClamp, "Vulkan: gpuTiming %d, depthBiasClamp %d", caps.gpuTiming,
              caps.depthBiasClamp);
        CHECK(caps.indirectDraw, "Vulkan without indirect draws");
    }
    CHECK(device.supportsFormat(TextureFormat::Rgba8Unorm, TextureType::Texture2D,
                                TextureUsage::Sampled | TextureUsage::ColorTarget),
          "Rgba8Unorm render target");
    CHECK(device.supportsFormat(TextureFormat::Depth32Float, TextureType::Texture2D,
                                TextureUsage::Sampled | TextureUsage::DepthStencilTarget),
          "Depth32Float depth target");
    CHECK(device.supportsSampleCount(TextureFormat::Rgba8Unorm, 4), "4x MSAA");
    CHECK(!device.supportsSampleCount(TextureFormat::Rgba8Unorm, 3), "3 samples");
}

QUENCH_TEST(live_resources) {
    QUENCH_GPU_DEVICE(device);
    REQUIRE(device.liveResources() == 0, "%lld at start", static_cast<long long>(device.liveResources()));
    Result<Texture> texture = device.createTexture({.width = 4, .height = 4, .name = "live texture"});
    Result<Buffer> buffer = device.createBuffer({.size = 64, .name = "live buffer"});
    Result<Sampler> sampler = device.createSampler({});
    Result<TransferBuffer> transfer = device.createTransferBuffer({.size = 64});
    REQUIRE(texture && buffer && sampler && transfer, "creation failed");
    CHECK(device.liveResources() == 4, "%lld live", static_cast<long long>(device.liveResources()));
    CHECK(texture->desc().width == 4 && texture->desc().name.empty(), "desc kept without the name");

    Texture moved = std::move(texture).value();
    CHECK(device.liveResources() == 4, "%lld live after a move", static_cast<long long>(device.liveResources()));
    moved.reset();
    buffer->reset();
    sampler->reset();
    transfer->reset();
    CHECK(device.liveResources() == 0, "%lld live after release", static_cast<long long>(device.liveResources()));
}

// Assigning a device destroys the previous one the way its destructor does.
QUENCH_TEST(device_move) {
    QUENCH_GPU_DEVICE(device);
    std::optional<Device> other = test::createGpuDevice();
    REQUIRE(other, "second device");
    Result<Buffer> buffer = other->createBuffer({.size = 16});
    REQUIRE(buffer, "%s", buffer.error().c_str());
    buffer->reset();
    device = std::move(*other);
    CHECK(device.liveResources() == 0, "%lld live", static_cast<long long>(device.liveResources()));
    Result<Buffer> again = device.createBuffer({.size = 16});
    CHECK(again, "create on the moved device: %s", again ? "" : again.error().c_str());
}

// An unsupported combination is an error the caller can handle (a fallback), not a crash.
QUENCH_TEST(unsupported_texture_fails) {
    QUENCH_GPU_DEVICE(device);
    Result<Texture> texture = device.createTexture(
        {.format = TextureFormat::Bc7RgbaUnorm, .usage = TextureUsage::ColorTarget, .width = 4, .height = 4});
    CHECK(!texture, "a BC7 render target was created");
    if (!texture) std::printf("  expected error: %s\n", texture.error().c_str());
    CHECK(device.liveResources() == 0, "%lld live", static_cast<long long>(device.liveResources()));
}

QUENCH_TEST(buffer_round_trip) {
    QUENCH_GPU_DEVICE(device);
    Result<Buffer> buffer = device.createBuffer({.usage = BufferUsage::Vertex, .size = 256, .name = "round trip"});
    REQUIRE(buffer, "%s", buffer.error().c_str());
    const std::vector<std::byte> data = pattern(256, 3);
    Result<void> up = device.upload(*buffer, 0, data);
    REQUIRE(up, "upload: %s", up.error().c_str());
    Result<std::vector<std::byte>> all = device.download(*buffer, 0, 256);
    REQUIRE(all, "download: %s", all.error().c_str());
    CHECK(*all == data, "contents differ");

    const std::vector<std::byte> patch = pattern(16, 100);
    up = device.upload(*buffer, 64, patch);
    REQUIRE(up, "upload at 64: %s", up.error().c_str());
    Result<std::vector<std::byte>> part = device.download(*buffer, 60, 24);
    REQUIRE(part && part->size() == 24, "partial download");
    CHECK(std::memcmp(part->data(), data.data() + 60, 4) == 0, "bytes before the patch changed");
    CHECK(std::memcmp(part->data() + 4, patch.data(), 16) == 0, "patch missing");
    CHECK(std::memcmp(part->data() + 20, data.data() + 80, 4) == 0, "bytes after the patch changed");
}

QUENCH_TEST(texture_round_trip) {
    QUENCH_GPU_DEVICE(device);
    // An odd width: rows are tightly packed in both directions.
    Result<Texture> texture = device.createTexture({.width = 5, .height = 3, .name = "round trip"});
    REQUIRE(texture, "%s", texture.error().c_str());
    std::vector<Rgba8> pixels(15);
    for (std::uint8_t i = 0; i < 15; ++i) pixels[i] = {i, std::uint8_t(i * 16), std::uint8_t(255 - i), 255};
    Result<void> up = device.upload({.texture = *texture}, asBytes(pixels));
    REQUIRE(up, "upload: %s", up.error().c_str());

    // A sub-region: 2 x 1 texels at (1, 2).
    const std::array<Rgba8, 2> patch{{{1, 2, 3, 4}, {5, 6, 7, 8}}};
    up = device.upload({.texture = *texture, .x = 1, .y = 2, .width = 2, .height = 1}, std::as_bytes(std::span(patch)));
    REQUIRE(up, "region upload: %s", up.error().c_str());
    pixels[2 * 5 + 1] = patch[0];
    pixels[2 * 5 + 2] = patch[1];

    Result<std::vector<std::byte>> back = device.download({.texture = *texture});
    REQUIRE(back && back->size() == 60, "download");
    CHECK(std::memcmp(back->data(), pixels.data(), 60) == 0, "contents differ");

    Result<std::vector<std::byte>> region =
        device.download({.texture = *texture, .x = 1, .y = 2, .width = 2, .height = 1});
    REQUIRE(region && region->size() == 8, "region download");
    CHECK(std::memcmp(region->data(), patch.data(), 8) == 0, "region differs");
}

QUENCH_TEST(texture_array_layers) {
    QUENCH_GPU_DEVICE(device);
    Result<Texture> texture =
        device.createTexture({.type = TextureType::Texture2DArray, .width = 2, .height = 2, .layers = 3});
    REQUIRE(texture, "%s", texture.error().c_str());
    for (std::uint32_t layer = 0; layer < 3; ++layer) {
        const std::vector<Rgba8> pixels(4, Rgba8{std::uint8_t(10 * layer), 0, 0, 255});
        Result<void> up = device.upload({.texture = *texture, .layer = layer}, asBytes(pixels));
        REQUIRE(up, "upload layer %u: %s", layer, up.error().c_str());
    }
    for (std::uint32_t layer = 0; layer < 3; ++layer) {
        Result<std::vector<std::byte>> back = device.download({.texture = *texture, .layer = layer});
        REQUIRE(back && back->size() == 16, "download layer %u", layer);
        CHECK(std::to_integer<int>((*back)[0]) == int(10 * layer), "layer %u holds %d", layer,
              std::to_integer<int>((*back)[0]));
    }
}

// Transfer buffers and copy passes by hand: buffer to buffer, texture to texture, then both read back.
QUENCH_TEST(copy_pass) {
    QUENCH_GPU_DEVICE(device);
    Result<TransferBuffer> staging = device.createTransferBuffer({.size = 64 + 16 * 4});
    Result<Buffer> a = device.createBuffer({.size = 64});
    Result<Buffer> b = device.createBuffer({.size = 64});
    Result<Texture> src = device.createTexture({.width = 4, .height = 4});
    Result<Texture> dst = device.createTexture({.width = 4, .height = 4});
    REQUIRE(staging && a && b && src && dst, "creation failed");

    const std::vector<std::byte> bytes = pattern(64, 9);
    std::vector<Rgba8> texels(16);
    for (std::uint8_t i = 0; i < 16; ++i) texels[i] = {i, i, i, 255};
    std::span<std::byte> mapped = staging->map();
    REQUIRE(mapped.size() == 128, "mapped %zu bytes", mapped.size());
    std::memcpy(mapped.data(), bytes.data(), 64);
    std::memcpy(mapped.data() + 64, texels.data(), 64);
    staging->unmap();

    CommandBuffer commands = device.acquireCommandBuffer();
    REQUIRE(commands, "no command buffer");
    {
        CopyPass copy = commands.beginCopyPass();
        copy.upload(*staging, 0, *a, 0, 64);
        copy.upload(*staging, 64, TextureRegion{.texture = *src});
        copy.copy(*a, 16, *b, 0, 32);
        // The top-right 2 x 2 texels of src to the bottom-left of dst.
        copy.copy(TextureRegion{.texture = *src, .x = 2, .y = 0, .width = 2, .height = 2},
                  TextureRegion{.texture = *dst, .x = 0, .y = 2});
    }
    Result<void> submitted = commands.submit();
    REQUIRE(submitted, "submit: %s", submitted.error().c_str());

    Result<std::vector<std::byte>> bb = device.download(*b, 0, 32);
    REQUIRE(bb, "download b");
    CHECK(std::memcmp(bb->data(), bytes.data() + 16, 32) == 0, "buffer copy differs");
    Result<std::vector<std::byte>> region = device.download({.texture = *dst, .x = 0, .y = 2, .width = 2, .height = 2});
    REQUIRE(region && region->size() == 16, "download dst");
    const Rgba8* got = reinterpret_cast<const Rgba8*>(region->data());
    CHECK(got[0].r == 2 && got[1].r == 3 && got[2].r == 6 && got[3].r == 7, "texture copy: %d %d %d %d", got[0].r,
          got[1].r, got[2].r, got[3].r);
}

QUENCH_TEST(generate_mipmaps) {
    QUENCH_GPU_DEVICE(device);
    Result<Texture> texture = device.createTexture({.usage = TextureUsage::Sampled | TextureUsage::ColorTarget,
                                                    .width = 4,
                                                    .height = 4,
                                                    .mipLevels = fullMipCount(4, 4),
                                                    .name = "mips"});
    REQUIRE(texture, "%s", texture.error().c_str());
    // Left half white, right half black.
    std::vector<Rgba8> pixels(16);
    for (int i = 0; i < 16; ++i) pixels[i] = (i % 4) < 2 ? Rgba8{255, 255, 255, 255} : Rgba8{0, 0, 0, 255};
    Result<void> up = device.upload({.texture = *texture}, asBytes(pixels));
    REQUIRE(up, "upload: %s", up.error().c_str());

    CommandBuffer commands = device.acquireCommandBuffer();
    commands.generateMipmaps(*texture);
    REQUIRE(commands.submit(), "submit");

    Result<std::vector<std::byte>> level1 = device.download({.texture = *texture, .mipLevel = 1});
    Result<std::vector<std::byte>> level2 = device.download({.texture = *texture, .mipLevel = 2});
    REQUIRE(level1 && level1->size() == 16 && level2 && level2->size() == 4, "downloads");
    const Rgba8* l1 = reinterpret_cast<const Rgba8*>(level1->data());
    const Rgba8 l2 = *reinterpret_cast<const Rgba8*>(level2->data());
    CHECK(l1[0].r == 255 && l1[2].r == 255 && l1[1].r == 0 && l1[3].r == 0, "level 1: %d %d / %d %d", l1[0].r, l1[1].r,
          l1[2].r, l1[3].r);
    CHECK(l2.r >= 126 && l2.r <= 129 && l2.a == 255, "level 2: %d alpha %d", l2.r, l2.a);
}

QUENCH_TEST(blit_nearest) {
    QUENCH_GPU_DEVICE(device);
    const TextureUsage usage = TextureUsage::Sampled | TextureUsage::ColorTarget;
    Result<Texture> small = device.createTexture({.usage = usage, .width = 2, .height = 2});
    Result<Texture> large = device.createTexture({.usage = usage, .width = 4, .height = 4});
    REQUIRE(small && large, "creation failed");
    const std::vector<Rgba8> pixels{{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}};
    REQUIRE(device.upload({.texture = *small}, asBytes(pixels)), "upload");

    CommandBuffer commands = device.acquireCommandBuffer();
    commands.blit({.source = {.texture = *small}, .destination = {.texture = *large}, .filter = Filter::Nearest});
    REQUIRE(commands.submit(), "submit");

    const std::vector<Rgba8> out = test::readRgba8(device, *large);
    REQUIRE(out.size() == 16, "%zu pixels", out.size());
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const Rgba8 expected = pixels[(y / 2) * 2 + x / 2];
            CHECK(test::near(out[y * 4 + x], expected, 0), "(%d, %d): %d %d %d", x, y, out[y * 4 + x].r,
                  out[y * 4 + x].g, out[y * 4 + x].b);
        }
}

QUENCH_TEST(gpu_timing) {
    QUENCH_GPU_DEVICE(device);
    Result<Texture> target = device.createTexture({.usage = TextureUsage::ColorTarget, .width = 512, .height = 512});
    const ColorTargetDesc color{.format = TextureFormat::Rgba8Unorm};
    Result<GraphicsPipeline> pipeline = device.createPipeline({.vertexShader = shaders::render.find("fullscreen_vs"),
                                                               .fragmentShader = shaders::render.find("uniform_fs"),
                                                               .colorTargets = {&color, 1}});
    REQUIRE(target && pipeline, "creation failed");

    CommandBuffer commands = device.acquireCommandBuffer();
    {
        RenderPass pass = commands.beginRenderPass(ColorTarget{.texture = *target});
        pass.bindPipeline(*pipeline);
        pass.pushUniforms(ShaderStage::Fragment, 0, Color{1, 0, 0, 1});
        for (int i = 0; i < 16; ++i) pass.draw(3);
    }
    Result<Fence> fence = commands.submitWithFence();
    REQUIRE(fence, "submit: %s", fence.error().c_str());
    device.wait(*fence);
    CHECK(device.isSignaled(*fence), "fence not signalled after wait");
    const std::optional<double> ms = device.gpuMilliseconds(*fence);
    if (device.caps().gpuTiming) {
        CHECK(ms && *ms > 0.0 && *ms < 1000.0, "GPU time %.4f ms", ms.value_or(-1.0));
        std::printf("  GPU time %.4f ms\n", ms.value_or(-1.0));
    } else {
        CHECK(!ms, "GPU time without Caps::gpuTiming");
    }
}
