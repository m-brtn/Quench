#include "GpuTest.h"

#include "render_shaders.h"

#include <array>
#include <cstring>
#include <vector>

using namespace quench;
using namespace quench::gpu;
using test::Rgba8;

namespace {

struct ColorVertex {
    float x, y, z;
    float r, g, b, a;
};

constexpr VertexBufferLayout kColorLayout{.slot = 0, .stride = sizeof(ColorVertex)};
constexpr VertexAttribute kColorAttributes[] = {
    {.location = 0, .slot = 0, .format = VertexFormat::Float3, .offset = 0},
    {.location = 1, .slot = 0, .format = VertexFormat::Float4, .offset = 12},
};
constexpr ColorTargetDesc kRgba8{.format = TextureFormat::Rgba8Unorm};

// Two triangles covering the clip-space rectangle [x0, x1] x [y0, y1] (y up).
void addRect(std::vector<ColorVertex>& out, float x0, float y0, float x1, float y1, float z, Color c) {
    const ColorVertex v[4] = {{x0, y0, z, c.r, c.g, c.b, c.a},
                              {x1, y0, z, c.r, c.g, c.b, c.a},
                              {x0, y1, z, c.r, c.g, c.b, c.a},
                              {x1, y1, z, c.r, c.g, c.b, c.a}};
    out.insert(out.end(), {v[0], v[1], v[2], v[2], v[1], v[3]});
}

Result<Texture> colorTarget(Device& device, std::uint32_t width, std::uint32_t height) {
    return device.createTexture({.usage = TextureUsage::ColorTarget, .width = width, .height = height});
}

Result<Buffer> vertexBuffer(Device& device, const std::vector<ColorVertex>& vertices) {
    const std::span<const std::byte> bytes = std::as_bytes(std::span(vertices));
    Result<Buffer> buffer = device.createBuffer({.size = std::uint32_t(bytes.size())});
    if (!buffer) return buffer;
    Result<void> up = device.upload(*buffer, 0, bytes);
    if (!up) return Error{up.error()};
    return buffer;
}

Rgba8 at(const std::vector<Rgba8>& pixels, std::uint32_t width, std::uint32_t x, std::uint32_t y) {
    return pixels[y * width + x];
}

#define CHECK_PIXEL(pixels, width, x, y, expected, tolerance)                                                          \
    do {                                                                                                               \
        const Rgba8 got_ = at(pixels, width, x, y);                                                                    \
        const Rgba8 want_ = expected;                                                                                  \
        CHECK(test::near(got_, want_, tolerance), "(%d, %d): %d %d %d %d, expected %d %d %d %d", int(x), int(y),       \
              got_.r, got_.g, got_.b, got_.a, want_.r, want_.g, want_.b, want_.a);                                     \
    } while (0)

constexpr Rgba8 kRed{255, 0, 0, 255};
constexpr Rgba8 kGreen{0, 255, 0, 255};
constexpr Rgba8 kBlue{0, 0, 255, 255};
constexpr Rgba8 kBlack{0, 0, 0, 255};

} // namespace

QUENCH_TEST(clear) {
    QUENCH_GPU_DEVICE(device);
    Result<Texture> target = colorTarget(device, 4, 4);
    REQUIRE(target, "%s", target.error().c_str());
    CommandBuffer commands = device.acquireCommandBuffer();
    commands.beginRenderPass(ColorTarget{.texture = *target, .clear = {0.0f, 0.0f, 1.0f, 1.0f}}).end();
    REQUIRE(commands.submit(), "submit");
    const std::vector<Rgba8> pixels = test::readRgba8(device, *target);
    REQUIRE(pixels.size() == 16, "%zu pixels", pixels.size());
    for (std::uint32_t i = 0; i < 16; ++i) CHECK_PIXEL(pixels, 4, i % 4, i / 4, kBlue, 0);
}

// Clip-space y points up and row 0 of a download is the top of the image, on every backend.
QUENCH_TEST(orientation) {
    QUENCH_GPU_DEVICE(device);
    Result<Texture> target = colorTarget(device, 8, 8);
    std::vector<ColorVertex> vertices;
    addRect(vertices, -1, 0, 1, 1, 0, {1, 0, 0, 1});  // top half red
    addRect(vertices, -1, -1, 0, 0, 0, {0, 1, 0, 1}); // bottom-left quarter green
    Result<Buffer> vb = vertexBuffer(device, vertices);
    Result<GraphicsPipeline> pipeline = device.createPipeline({.vertexShader = shaders::render.find("color_vs"),
                                                               .fragmentShader = shaders::render.find("color_fs"),
                                                               .vertexBuffers = {&kColorLayout, 1},
                                                               .vertexAttributes = kColorAttributes,
                                                               .colorTargets = {&kRgba8, 1},
                                                               .name = "orientation"});
    REQUIRE(target && vb && pipeline, "creation failed");

    CommandBuffer commands = device.acquireCommandBuffer();
    {
        RenderPass pass = commands.beginRenderPass(ColorTarget{.texture = *target, .clear = {0, 0, 1, 1}});
        pass.bindPipeline(*pipeline);
        pass.bindVertexBuffer(0, {*vb});
        pass.draw(std::uint32_t(vertices.size()));
    }
    REQUIRE(commands.submit(), "submit");
    const std::vector<Rgba8> p = test::readRgba8(device, *target);
    REQUIRE(p.size() == 64, "%zu pixels", p.size());
    CHECK_PIXEL(p, 8, 0, 0, kRed, 0);
    CHECK_PIXEL(p, 8, 7, 3, kRed, 0);
    CHECK_PIXEL(p, 8, 0, 4, kGreen, 0);
    CHECK_PIXEL(p, 8, 3, 7, kGreen, 0);
    CHECK_PIXEL(p, 8, 4, 4, kBlue, 0);
    CHECK_PIXEL(p, 8, 7, 7, kBlue, 0);
}

// Fragment uniforms change between draws; each draw goes to its own viewport.
QUENCH_TEST(uniforms_and_viewports) {
    QUENCH_GPU_DEVICE(device);
    Result<Texture> target = colorTarget(device, 8, 4);
    Result<GraphicsPipeline> pipeline = device.createPipeline({.vertexShader = shaders::render.find("fullscreen_vs"),
                                                               .fragmentShader = shaders::render.find("uniform_fs"),
                                                               .colorTargets = {&kRgba8, 1}});
    REQUIRE(target && pipeline, "creation failed");

    CommandBuffer commands = device.acquireCommandBuffer();
    {
        RenderPass pass = commands.beginRenderPass(ColorTarget{.texture = *target});
        pass.bindPipeline(*pipeline);
        pass.setViewport({.x = 0, .y = 0, .width = 4, .height = 4});
        pass.pushUniforms(ShaderStage::Fragment, 0, Color{0.25f, 0.5f, 0.75f, 1.0f});
        pass.draw(3);
        pass.setViewport({.x = 4, .y = 0, .width = 4, .height = 4});
        pass.pushUniforms(ShaderStage::Fragment, 0, Color{1.0f, 0.0f, 0.0f, 1.0f});
        pass.draw(3);
    }
    REQUIRE(commands.submit(), "submit");
    const std::vector<Rgba8> p = test::readRgba8(device, *target);
    REQUIRE(p.size() == 32, "%zu pixels", p.size());
    CHECK_PIXEL(p, 8, 0, 0, (Rgba8{64, 128, 191, 255}), 1);
    CHECK_PIXEL(p, 8, 3, 3, (Rgba8{64, 128, 191, 255}), 1);
    CHECK_PIXEL(p, 8, 4, 0, kRed, 0);
    CHECK_PIXEL(p, 8, 7, 3, kRed, 0);
}

// Uniforms pushed outside a pass stay for the passes after a blit and mipmap generation (SDL GPU draws both with a
// shader of its own on some drivers, with uniforms of its own in fragment slot 0).
QUENCH_TEST(uniforms_outlive_blits) {
    QUENCH_GPU_DEVICE(device);
    const TextureUsage usage = TextureUsage::Sampled | TextureUsage::ColorTarget;
    Result<Texture> target = colorTarget(device, 4, 4);
    Result<Texture> source = device.createTexture({.usage = usage, .width = 4, .height = 4, .name = "blit source"});
    Result<Texture> copy = device.createTexture({.usage = usage, .width = 8, .height = 8, .name = "blit copy"});
    Result<Texture> mips = device.createTexture(
        {.usage = usage, .width = 8, .height = 8, .mipLevels = fullMipCount(8, 8), .name = "mips"});
    Result<GraphicsPipeline> pipeline = device.createPipeline({.vertexShader = shaders::render.find("fullscreen_vs"),
                                                               .fragmentShader = shaders::render.find("uniform_fs"),
                                                               .colorTargets = {&kRgba8, 1}});
    REQUIRE(target && source && copy && mips && pipeline, "creation failed");

    CommandBuffer commands = device.acquireCommandBuffer();
    commands.pushUniforms(ShaderStage::Fragment, 0, Color{1.0f, 0.0f, 0.0f, 1.0f});
    commands.blit({.source = {.texture = *source}, .destination = {.texture = *copy}});
    commands.generateMipmaps(*mips);
    {
        RenderPass pass = commands.beginRenderPass(ColorTarget{.texture = *target});
        pass.bindPipeline(*pipeline);
        pass.draw(3);
    }
    REQUIRE(commands.submit(), "submit");
    const std::vector<Rgba8> p = test::readRgba8(device, *target);
    REQUIRE(p.size() == 16, "%zu pixels", p.size());
    CHECK_PIXEL(p, 4, 0, 0, kRed, 0);
    CHECK_PIXEL(p, 4, 3, 3, kRed, 0);
}

// A nearest-filtered texture drawn 1:1 comes back unchanged: uploads, sampling and downloads agree on the origin.
QUENCH_TEST(texture_sampling) {
    QUENCH_GPU_DEVICE(device);
    Result<Texture> image = device.createTexture({.width = 4, .height = 4, .name = "image"});
    Result<Texture> target = colorTarget(device, 4, 4);
    Result<Sampler> sampler = device.createSampler({.minFilter = Filter::Nearest,
                                                    .magFilter = Filter::Nearest,
                                                    .mipmapMode = MipmapMode::Nearest,
                                                    .addressU = AddressMode::ClampToEdge,
                                                    .addressV = AddressMode::ClampToEdge});
    Result<GraphicsPipeline> pipeline = device.createPipeline({.vertexShader = shaders::render.find("fullscreen_vs"),
                                                               .fragmentShader = shaders::render.find("texture_fs"),
                                                               .colorTargets = {&kRgba8, 1}});
    REQUIRE(image && target && sampler && pipeline, "creation failed");
    std::vector<Rgba8> texels(16);
    for (std::uint8_t y = 0; y < 4; ++y)
        for (std::uint8_t x = 0; x < 4; ++x)
            texels[y * 4 + x] = {std::uint8_t(x * 60), std::uint8_t(y * 60), std::uint8_t(255 - x * 30), 255};
    REQUIRE(device.upload({.texture = *image}, std::as_bytes(std::span(texels))), "upload");

    CommandBuffer commands = device.acquireCommandBuffer();
    {
        RenderPass pass = commands.beginRenderPass(ColorTarget{.texture = *target});
        pass.bindPipeline(*pipeline);
        pass.bindSampler(ShaderStage::Fragment, 0, {*image, *sampler});
        pass.draw(3);
    }
    REQUIRE(commands.submit(), "submit");
    const std::vector<Rgba8> p = test::readRgba8(device, *target);
    REQUIRE(p.size() == 16, "%zu pixels", p.size());
    for (std::uint32_t i = 0; i < 16; ++i) CHECK_PIXEL(p, 4, i % 4, i / 4, texels[i], 0);
}

// Reversed depth: cleared to 0 (far), GreaterOrEqual keeps the nearer (larger) depth.
QUENCH_TEST(reversed_depth) {
    QUENCH_GPU_DEVICE(device);
    Result<Texture> target = colorTarget(device, 8, 8);
    Result<Texture> depth = device.createTexture(
        {.format = TextureFormat::Depth32Float, .usage = TextureUsage::DepthStencilTarget, .width = 8, .height = 8});
    std::vector<ColorVertex> vertices;
    addRect(vertices, -1, -1, 0, 1, 0.75f, {1, 0, 0, 1}); // near, left half, drawn first
    addRect(vertices, -1, -1, 1, 1, 0.25f, {0, 1, 0, 1}); // far, everywhere
    Result<Buffer> vb = vertexBuffer(device, vertices);
    Result<GraphicsPipeline> pipeline = device.createPipeline({.vertexShader = shaders::render.find("color_vs"),
                                                               .fragmentShader = shaders::render.find("color_fs"),
                                                               .vertexBuffers = {&kColorLayout, 1},
                                                               .vertexAttributes = kColorAttributes,
                                                               .depth = {.test = true, .write = true},
                                                               .colorTargets = {&kRgba8, 1},
                                                               .depthFormat = TextureFormat::Depth32Float});
    REQUIRE(target && depth && vb && pipeline, "creation failed");

    CommandBuffer commands = device.acquireCommandBuffer();
    {
        const DepthTarget depthTarget{.texture = *depth};
        RenderPass pass = commands.beginRenderPass(ColorTarget{.texture = *target}, &depthTarget);
        pass.bindPipeline(*pipeline);
        pass.bindVertexBuffer(0, {*vb});
        pass.draw(std::uint32_t(vertices.size()));
    }
    REQUIRE(commands.submit(), "submit");
    const std::vector<Rgba8> p = test::readRgba8(device, *target);
    REQUIRE(p.size() == 64, "%zu pixels", p.size());
    CHECK_PIXEL(p, 8, 0, 0, kRed, 0);
    CHECK_PIXEL(p, 8, 3, 7, kRed, 0);
    CHECK_PIXEL(p, 8, 4, 0, kGreen, 0);
    CHECK_PIXEL(p, 8, 7, 7, kGreen, 0);
}

// Instances read from a vertex storage buffer, moved by a vertex uniform, drawn as triangle strips.
QUENCH_TEST(storage_buffer_instances) {
    QUENCH_GPU_DEVICE(device);
    struct RectData {
        float bounds[4];
        float color[4];
    };
    // The shader moves them right by half the target (1 in clip space).
    const std::array<RectData, 2> rects{{{{-2, 0, -1, 1}, {1, 0, 0, 1}}, {{-1, -1, 0, 0}, {0, 1, 0, 1}}}};
    const std::span<const std::byte> bytes = std::as_bytes(std::span(rects));
    Result<Buffer> buffer =
        device.createBuffer({.usage = BufferUsage::GraphicsStorageRead, .size = std::uint32_t(bytes.size())});
    Result<Texture> target = colorTarget(device, 8, 8);
    Result<GraphicsPipeline> pipeline = device.createPipeline({.vertexShader = shaders::render.find("rects_vs"),
                                                               .fragmentShader = shaders::render.find("color_fs"),
                                                               .primitive = PrimitiveType::TriangleStrip,
                                                               .colorTargets = {&kRgba8, 1}});
    REQUIRE(buffer && target && pipeline, "creation failed");
    REQUIRE(device.upload(*buffer, 0, bytes), "upload");

    CommandBuffer commands = device.acquireCommandBuffer();
    {
        RenderPass pass = commands.beginRenderPass(ColorTarget{.texture = *target});
        pass.bindPipeline(*pipeline);
        const BufferRef storage = *buffer;
        pass.bindStorageBuffers(ShaderStage::Vertex, 0, {&storage, 1});
        pass.pushUniforms(ShaderStage::Vertex, 0, Color{1, 0, 0, 0});
        pass.draw(4, 2);
    }
    REQUIRE(commands.submit(), "submit");
    const std::vector<Rgba8> p = test::readRgba8(device, *target);
    REQUIRE(p.size() == 64, "%zu pixels", p.size());
    CHECK_PIXEL(p, 8, 0, 0, kRed, 0);
    CHECK_PIXEL(p, 8, 3, 3, kRed, 0);
    CHECK_PIXEL(p, 8, 4, 0, kBlack, 0);
    CHECK_PIXEL(p, 8, 0, 4, kBlack, 0); // where the second instance would be without the uniform
    CHECK_PIXEL(p, 8, 4, 4, kGreen, 0);
    CHECK_PIXEL(p, 8, 7, 7, kGreen, 0);
}

// A compute pass with two read-only inputs, a uniform and a read-write output; the last thread group is partial.
QUENCH_TEST(compute_dispatch) {
    QUENCH_GPU_DEVICE(device);
    constexpr std::uint32_t kCount = 70;
    std::vector<float> a(kCount * 4), b(kCount * 4);
    for (std::uint32_t i = 0; i < kCount * 4; ++i) {
        a[i] = float(i % 7) - 3.0f;
        b[i] = float(i) * 0.5f;
    }
    const std::uint32_t size = kCount * 16;
    Result<Buffer> inA = device.createBuffer({.usage = BufferUsage::ComputeStorageRead, .size = size});
    Result<Buffer> inB = device.createBuffer({.usage = BufferUsage::ComputeStorageRead, .size = size});
    Result<Buffer> out = device.createBuffer({.usage = BufferUsage::ComputeStorageWrite, .size = size + 16});
    Result<ComputePipeline> pipeline = device.createComputePipeline({.shader = shaders::render.find("scale_cs")});
    REQUIRE(inA && inB && out && pipeline, "creation failed: %s", pipeline ? "" : pipeline.error().c_str());
    REQUIRE(device.upload(*inA, 0, std::as_bytes(std::span(a))), "upload a");
    REQUIRE(device.upload(*inB, 0, std::as_bytes(std::span(b))), "upload b");
    const std::array<float, 4> sentinel{-1, -1, -1, -1};
    REQUIRE(device.upload(*out, size, std::as_bytes(std::span(sentinel))), "upload sentinel");

    CommandBuffer commands = device.acquireCommandBuffer();
    {
        const StorageBufferWrite write{.buffer = *out};
        ComputePass pass = commands.beginComputePass({&write, 1});
        REQUIRE(pass, "beginComputePass");
        pass.bindPipeline(*pipeline);
        const std::array<BufferRef, 2> inputs{*inA, *inB};
        pass.bindStorageBuffers(0, inputs);
        pass.pushUniforms(0, Color{2.0f, float(kCount), 0, 0});
        pass.dispatch((kCount + 31) / 32);
    }
    REQUIRE(commands.submit(), "submit");
    Result<std::vector<std::byte>> bytes = device.download(*out, 0, size + 16);
    REQUIRE(bytes && bytes->size() == size + 16, "download");
    std::vector<float> got(kCount * 4 + 4);
    std::memcpy(got.data(), bytes->data(), bytes->size());
    int wrong = 0;
    for (std::uint32_t i = 0; i < kCount * 4; ++i)
        if (got[i] != a[i] * 2.0f + b[i] + float(i / 4)) ++wrong;
    CHECK(wrong == 0, "%d of %u values wrong (first: %g, expected %g)", wrong, kCount * 4, double(got[0]),
          double(a[0] * 2.0f + b[0]));
    CHECK(got[kCount * 4] == -1.0f, "thread %u past the count wrote %g", kCount, double(got[kCount * 4]));
}

// The indirect draw only where Caps::indirectDraw.
QUENCH_TEST(indexed_and_indirect) {
    QUENCH_GPU_DEVICE(device);
    std::vector<ColorVertex> vertices = {
        {-1, 0, 0, 1, 0, 0, 1}, {1, 0, 0, 1, 0, 0, 1}, {-1, 1, 0, 1, 0, 0, 1}, {1, 1, 0, 1, 0, 0, 1}};
    addRect(vertices, -1, -1, 1, 0, 0, {0, 1, 0, 1}); // vertices 4..9, drawn indirectly
    const std::array<std::uint16_t, 6> indices{0, 1, 2, 2, 1, 3};
    const std::array<std::uint32_t, 4> indirect{6, 1, 4, 0}; // vertexCount, instanceCount, firstVertex, firstInstance
    Result<Buffer> vb = vertexBuffer(device, vertices);
    Result<Buffer> ib = device.createBuffer({.usage = BufferUsage::Index, .size = sizeof(indices)});
    Result<Buffer> args = device.createBuffer({.usage = BufferUsage::Indirect, .size = sizeof(indirect)});
    Result<Texture> target = colorTarget(device, 4, 4);
    Result<GraphicsPipeline> pipeline = device.createPipeline({.vertexShader = shaders::render.find("color_vs"),
                                                               .fragmentShader = shaders::render.find("color_fs"),
                                                               .vertexBuffers = {&kColorLayout, 1},
                                                               .vertexAttributes = kColorAttributes,
                                                               .colorTargets = {&kRgba8, 1}});
    REQUIRE(vb && ib && args && target && pipeline, "creation failed");
    REQUIRE(device.upload(*ib, 0, std::as_bytes(std::span(indices))), "index upload");
    REQUIRE(device.upload(*args, 0, std::as_bytes(std::span(indirect))), "indirect upload");

    CommandBuffer commands = device.acquireCommandBuffer();
    {
        RenderPass pass = commands.beginRenderPass(ColorTarget{.texture = *target});
        pass.bindPipeline(*pipeline);
        pass.bindVertexBuffer(0, {*vb});
        pass.bindIndexBuffer({*ib}, IndexFormat::Uint16);
        pass.drawIndexed(6);
        if (device.caps().indirectDraw) pass.drawIndirect(*args, 0, 1);
    }
    REQUIRE(commands.submit(), "submit");
    const std::vector<Rgba8> p = test::readRgba8(device, *target);
    REQUIRE(p.size() == 16, "%zu pixels", p.size());
    CHECK_PIXEL(p, 4, 0, 0, kRed, 0);
    CHECK_PIXEL(p, 4, 3, 1, kRed, 0);
    const Rgba8 bottom = device.caps().indirectDraw ? kGreen : kBlack;
    CHECK_PIXEL(p, 4, 0, 2, bottom, 0);
    CHECK_PIXEL(p, 4, 3, 3, bottom, 0);
}

QUENCH_TEST(alpha_blending) {
    QUENCH_GPU_DEVICE(device);
    Result<Texture> target = colorTarget(device, 4, 4);
    const ColorTargetDesc blended{.format = TextureFormat::Rgba8Unorm, .blend = BlendState::alpha()};
    Result<GraphicsPipeline> pipeline = device.createPipeline({.vertexShader = shaders::render.find("fullscreen_vs"),
                                                               .fragmentShader = shaders::render.find("uniform_fs"),
                                                               .colorTargets = {&blended, 1}});
    REQUIRE(target && pipeline, "creation failed");
    CommandBuffer commands = device.acquireCommandBuffer();
    {
        RenderPass pass = commands.beginRenderPass(ColorTarget{.texture = *target, .clear = {0, 0, 1, 1}});
        pass.bindPipeline(*pipeline);
        pass.pushUniforms(ShaderStage::Fragment, 0, Color{1, 0, 0, 0.5f});
        pass.draw(3);
    }
    REQUIRE(commands.submit(), "submit");
    const std::vector<Rgba8> p = test::readRgba8(device, *target);
    REQUIRE(p.size() == 16, "%zu pixels", p.size());
    CHECK_PIXEL(p, 4, 1, 1, (Rgba8{128, 0, 128, 255}), 1);
}

// 4x MSAA resolved into a single-sample texture: the diagonal edge gets partial coverage.
QUENCH_TEST(msaa_resolve) {
    QUENCH_GPU_DEVICE(device);
    if (!device.supportsSampleCount(TextureFormat::Rgba8Unorm, 4)) QUENCH_SKIP("no 4x MSAA for Rgba8Unorm");
    Result<Texture> msaa = device.createTexture(
        {.usage = TextureUsage::ColorTarget, .width = 16, .height = 16, .sampleCount = 4, .name = "msaa"});
    Result<Texture> resolved = colorTarget(device, 16, 16);
    // Upper-left half: the edge runs through the centres of the pixels (i, 15 - i).
    const std::vector<ColorVertex> vertices = {{-1, -1, 0, 1, 1, 1, 1}, {1, 1, 0, 1, 1, 1, 1}, {-1, 1, 0, 1, 1, 1, 1}};
    Result<Buffer> vb = vertexBuffer(device, vertices);
    Result<GraphicsPipeline> pipeline = device.createPipeline({.vertexShader = shaders::render.find("color_vs"),
                                                               .fragmentShader = shaders::render.find("color_fs"),
                                                               .vertexBuffers = {&kColorLayout, 1},
                                                               .vertexAttributes = kColorAttributes,
                                                               .colorTargets = {&kRgba8, 1},
                                                               .sampleCount = 4});
    REQUIRE(msaa && resolved && vb && pipeline, "creation failed");

    CommandBuffer commands = device.acquireCommandBuffer();
    {
        RenderPass pass = commands.beginRenderPass(ColorTarget{
            .texture = *msaa, .store = StoreOp::Resolve, .clear = {0, 0, 0, 1}, .resolveTexture = *resolved});
        pass.bindPipeline(*pipeline);
        pass.bindVertexBuffer(0, {*vb});
        pass.draw(3);
    }
    REQUIRE(commands.submit(), "submit");
    const std::vector<Rgba8> p = test::readRgba8(device, *resolved);
    REQUIRE(p.size() == 256, "%zu pixels", p.size());
    CHECK_PIXEL(p, 16, 0, 0, (Rgba8{255, 255, 255, 255}), 0);
    CHECK_PIXEL(p, 16, 15, 15, kBlack, 0);
    int partial = 0;
    for (std::uint32_t i = 0; i < 16; ++i) {
        const Rgba8 edge = at(p, 16, i, 15 - i);
        if (edge.r > 0 && edge.r < 255) ++partial;
    }
    CHECK(partial == 16, "%d of 16 edge pixels partially covered", partial);
}
