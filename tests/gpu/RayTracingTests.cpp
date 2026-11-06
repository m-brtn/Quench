#include "GpuTest.h"

#include "core/Platform.h"
#include "render_shaders.h"

#include <quench/gpu/CommandBuffer.h>

#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

using namespace quench;
using namespace quench::gpu;
using test::Rgba8;

namespace {

constexpr std::uint32_t kSize = 32;

// A unit quad in the xy plane at z, centered on the origin: two triangles.
std::array<float, 12> quad(float z) { return {-0.5f, -0.5f, z, 0.5f, -0.5f, z, 0.5f, 0.5f, z, -0.5f, 0.5f, z}; }
constexpr std::uint32_t kQuadIndices[] = {0, 1, 2, 0, 2, 3};

std::array<float, 12> translation(float x, float y, float z) {
    return {1.0f, 0.0f, 0.0f, x, 0.0f, 1.0f, 0.0f, y, 0.0f, 0.0f, 1.0f, z};
}

// The ray_fs pixel at target position x, y: hit, instance id, distance (m).
struct Hit {
    bool hit;
    int id;
    float distance;
};
Hit hitAt(const std::vector<Rgba8>& pixels, std::uint32_t x, std::uint32_t y) {
    const Rgba8 p = pixels[y * kSize + x];
    return {p.r > 127, p.g, float(p.b) / 255.0f * 20.0f};
}

struct RayScene {
    Buffer vertices;
    Buffer indices;
    Texture target;
    GraphicsPipeline pipeline;
};

// The mesh in buffers, the target and a pipeline of fullscreen_vs and the fragment shader; empty on failure.
std::optional<RayScene> rayScene(Device& device, std::span<const float> vertices,
                                 std::span<const std::uint32_t> indices, const ShaderBlob* fragment) {
    Result<Buffer> vb =
        device.createBuffer({.usage = BufferUsage::Vertex, .size = std::uint32_t(vertices.size_bytes())});
    Result<Buffer> ib = device.createBuffer({.usage = BufferUsage::Index, .size = std::uint32_t(indices.size_bytes())});
    Result<Texture> target =
        device.createTexture({.usage = TextureUsage::ColorTarget, .width = kSize, .height = kSize});
    const ColorTargetDesc color{.format = TextureFormat::Rgba8Unorm};
    Result<GraphicsPipeline> pipeline = device.createPipeline({.vertexShader = shaders::render.find("fullscreen_vs"),
                                                               .fragmentShader = fragment,
                                                               .colorTargets = {&color, 1}});
    if (!vb || !ib || !target || !pipeline || !device.upload(*vb, 0, std::as_bytes(vertices)) ||
        !device.upload(*ib, 0, std::as_bytes(indices)))
        return std::nullopt;
    return RayScene{std::move(*vb), std::move(*ib), std::move(*target), std::move(*pipeline)};
}

// Traces ray_fs against the top level and reads the target back.
std::vector<Rgba8> trace(Device& device, RayScene& s, const AccelerationStructure& top) {
    CommandBuffer commands = device.acquireCommandBuffer();
    RenderPass pass = commands.beginRenderPass(ColorTarget{.texture = s.target});
    pass.bindPipeline(s.pipeline);
    pass.bindAccelerationStructure(ShaderStage::Fragment, 0, top);
    pass.draw(3);
    pass.end();
    if (Result<void> r = commands.submit(); !r) return {};
    return test::readRgba8(device, s.target);
}

} // namespace

QUENCH_TEST(ray_tracing_caps) {
    QUENCH_GPU_DEVICE(device);
    const Caps& caps = device.caps();
    std::printf("  rayTracing %d, spatialUpscaling %d, temporalUpscaling %d\n", caps.rayTracing, caps.spatialUpscaling,
                caps.temporalUpscaling);
    if (caps.backend == Backend::SdlGpu)
        CHECK(!caps.rayTracing && !caps.spatialUpscaling && !caps.temporalUpscaling, "SDL GPU with ray tracing");
    // The Mac's GPU (Apple silicon) has both; the iOS Simulator has neither (no MetalFX in its SDK).
    if (caps.backend == Backend::Metal && QUENCH_PLATFORM_MACOS)
        CHECK(caps.rayTracing && caps.spatialUpscaling && caps.temporalUpscaling, "no ray tracing or MetalFX");
    if (!caps.rayTracing) {
        Result<AccelerationStructure> as = device.createAccelerationStructure({.vertexCount = 4, .triangleCount = 2});
        CHECK(!as, "an acceleration structure without Caps::rayTracing");
    }
    if (!caps.temporalUpscaling) {
        Result<Upscaler> u =
            device.createUpscaler({.inputWidth = 16, .inputHeight = 16, .outputWidth = 32, .outputHeight = 32});
        CHECK(!u, "an upscaler without Caps::temporalUpscaling");
    }
}

// Two instances of one bottom level at different depths; the top level is built again with one of them moved.
QUENCH_TEST(ray_query_instances) {
    QUENCH_GPU_DEVICE(device);
    if (!device.caps().rayTracing) QUENCH_SKIP("no ray tracing (gpu::Caps::rayTracing)");
    const ShaderBlob* rayFs = shaders::render.find("ray_fs");
    if (!rayFs) QUENCH_SKIP("no ray query shaders in this build");
    const std::array<float, 12> vertices = quad(0.0f);
    Result<Buffer> vb = device.createBuffer({.usage = BufferUsage::Vertex, .size = sizeof(vertices)});
    Result<Buffer> ib = device.createBuffer({.usage = BufferUsage::Index, .size = sizeof(kQuadIndices)});
    Result<Texture> target =
        device.createTexture({.usage = TextureUsage::ColorTarget, .width = kSize, .height = kSize});
    const ColorTargetDesc color{.format = TextureFormat::Rgba8Unorm};
    Result<GraphicsPipeline> pipeline = device.createPipeline(
        {.vertexShader = shaders::render.find("fullscreen_vs"), .fragmentShader = rayFs, .colorTargets = {&color, 1}});
    Result<AccelerationStructure> bottom = device.createAccelerationStructure({.vertexCount = 4, .triangleCount = 2});
    Result<AccelerationStructure> top =
        device.createAccelerationStructure({.level = AccelerationStructureLevel::Top, .instanceCapacity = 4});
    REQUIRE(vb && ib && target && pipeline && bottom && top, "creation failed");
    REQUIRE(device.upload(*vb, 0, std::as_bytes(std::span(vertices))) &&
                device.upload(*ib, 0, std::as_bytes(std::span(kQuadIndices))),
            "upload failed");
    RayScene s{std::move(*vb), std::move(*ib), std::move(*target), std::move(*pipeline)};
    const RayGeometry geometry{.vertices = s.vertices, .vertexCount = 4, .indices = s.indices, .triangleCount = 2};

    const auto build = [&](float rightDepth) {
        const RayInstance instances[] = {
            {.bottomLevel = &*bottom, .transform = translation(-0.5f, 0.0f, 0.0f), .id = 7},
            {.bottomLevel = &*bottom, .transform = translation(0.5f, 0.0f, -rightDepth), .id = 9},
        };
        CommandBuffer commands = device.acquireCommandBuffer();
        AccelerationStructurePass pass = commands.beginAccelerationStructurePass();
        if (rightDepth == 2.0f) pass.build(*bottom, geometry);
        pass.build(*top, instances);
        pass.end();
        return bool(commands.submit());
    };
    REQUIRE(build(2.0f), "build failed");
    std::vector<Rgba8> pixels = trace(device, s, *top);
    REQUIRE(pixels.size() == kSize * kSize, "readback failed");
    const Hit left = hitAt(pixels, 8, 16), right = hitAt(pixels, 24, 16), above = hitAt(pixels, 16, 2);
    std::printf("  left %d id %d at %.2f m, right %d id %d at %.2f m, above %d\n", left.hit, left.id, left.distance,
                right.hit, right.id, right.distance, above.hit);
    CHECK(left.hit && left.id == 7 && std::abs(left.distance - 10.0f) < 0.1f, "left: %d, id %d, %.2f m", left.hit,
          left.id, left.distance);
    CHECK(right.hit && right.id == 9 && std::abs(right.distance - 12.0f) < 0.1f, "right: %d, id %d, %.2f m", right.hit,
          right.id, right.distance);
    CHECK(!above.hit, "a hit above the quads");

    // The top level of the next frame: the right instance moved 2 m further away.
    REQUIRE(build(4.0f), "rebuild failed");
    pixels = trace(device, s, *top);
    REQUIRE(pixels.size() == kSize * kSize, "readback failed");
    const Hit moved = hitAt(pixels, 24, 16);
    CHECK(moved.hit && moved.id == 9 && std::abs(moved.distance - 14.0f) < 0.1f, "moved: %d, id %d, %.2f m", moved.hit,
          moved.id, moved.distance);
    CHECK(hitAt(pixels, 8, 16).id == 7, "left instance lost");
}

// The quad is counter-clockwise seen from +z, where the rays come from (ray_face_fs): front faces of a plain
// instance and of a mirrored one whose normal faces the rays; back faces of a turned and of a mirrored instance
// whose normal faces away.
QUENCH_TEST(ray_query_front_faces) {
    QUENCH_GPU_DEVICE(device);
    if (!device.caps().rayTracing) QUENCH_SKIP("no ray tracing (gpu::Caps::rayTracing)");
    const ShaderBlob* rayFaceFs = shaders::render.find("ray_face_fs");
    if (!rayFaceFs) QUENCH_SKIP("no ray query shaders in this build");
    const std::array<float, 12> vertices = quad(0.0f);
    std::optional<RayScene> s = rayScene(device, vertices, kQuadIndices, rayFaceFs);
    Result<AccelerationStructure> bottom = device.createAccelerationStructure({.vertexCount = 4, .triangleCount = 2});
    Result<AccelerationStructure> top =
        device.createAccelerationStructure({.level = AccelerationStructureLevel::Top, .instanceCapacity = 4});
    REQUIRE(s && bottom && top, "creation failed");

    // 0.4 m wide, side by side at x -0.75, -0.25, 0.25 and 0.75.
    const RayInstance instances[] = {
        {.bottomLevel = &*bottom, .transform = {0.4f, 0, 0, -0.75f, 0, 0.4f, 0, 0, 0, 0, 0.4f, 0}, .id = 1},
        {.bottomLevel = &*bottom, .transform = {-0.4f, 0, 0, -0.25f, 0, 0.4f, 0, 0, 0, 0, -0.4f, 0}, .id = 2},
        {.bottomLevel = &*bottom, .transform = {-0.4f, 0, 0, 0.25f, 0, 0.4f, 0, 0, 0, 0, 0.4f, 0}, .id = 3},
        {.bottomLevel = &*bottom, .transform = {0.4f, 0, 0, 0.75f, 0, 0.4f, 0, 0, 0, 0, -0.4f, 0}, .id = 4},
    };
    {
        CommandBuffer commands = device.acquireCommandBuffer();
        AccelerationStructurePass pass = commands.beginAccelerationStructurePass();
        pass.build(*bottom, {.vertices = s->vertices, .vertexCount = 4, .indices = s->indices, .triangleCount = 2});
        pass.build(*top, instances);
        pass.end();
        REQUIRE(commands.submit(), "build failed");
    }
    const std::vector<Rgba8> pixels = trace(device, *s, *top);
    REQUIRE(pixels.size() == kSize * kSize, "readback failed");
    struct Expected {
        std::uint32_t x;
        int id;
        bool front;
        const char* what;
    };
    for (const Expected& e : {Expected{4, 1, true, "plain"}, Expected{12, 2, false, "turned"},
                              Expected{20, 3, true, "mirrored in x"}, Expected{28, 4, false, "mirrored in z"}}) {
        const Rgba8 p = pixels[16 * kSize + e.x];
        const bool hit = p.r > 127, front = p.g > 127;
        std::printf("  %s: hit %d, id %d, front %d\n", e.what, hit, p.b, front);
        CHECK(hit && p.b == e.id && front == e.front, "%s: hit %d, id %d, front %d (expected %d)", e.what, hit, p.b,
              front, e.front);
    }
}

// A bottom level refitted to moved vertices (skinned meshes): rays see the new positions.
QUENCH_TEST(ray_query_refit) {
    QUENCH_GPU_DEVICE(device);
    if (!device.caps().rayTracing) QUENCH_SKIP("no ray tracing (gpu::Caps::rayTracing)");
    const ShaderBlob* rayFs = shaders::render.find("ray_fs");
    if (!rayFs) QUENCH_SKIP("no ray query shaders in this build");
    // Two vertex sets in one buffer: the quad at z 0, then at z -3 and twice as wide.
    std::array<float, 24> vertices{};
    const std::array<float, 12> near = quad(0.0f), far = quad(-3.0f);
    std::memcpy(vertices.data(), near.data(), sizeof(near));
    std::memcpy(vertices.data() + 12, far.data(), sizeof(far));
    for (int v = 0; v < 4; ++v) vertices[std::size_t(12 + v * 3)] *= 2.0f;
    Result<Buffer> vb = device.createBuffer({.usage = BufferUsage::Vertex, .size = sizeof(vertices)});
    Result<Buffer> ib = device.createBuffer({.usage = BufferUsage::Index, .size = sizeof(kQuadIndices)});
    Result<Texture> target =
        device.createTexture({.usage = TextureUsage::ColorTarget, .width = kSize, .height = kSize});
    const ColorTargetDesc color{.format = TextureFormat::Rgba8Unorm};
    Result<GraphicsPipeline> pipeline = device.createPipeline(
        {.vertexShader = shaders::render.find("fullscreen_vs"), .fragmentShader = rayFs, .colorTargets = {&color, 1}});
    Result<AccelerationStructure> bottom =
        device.createAccelerationStructure({.vertexCount = 4, .triangleCount = 2, .allowRefit = true});
    Result<AccelerationStructure> top =
        device.createAccelerationStructure({.level = AccelerationStructureLevel::Top, .instanceCapacity = 1});
    REQUIRE(vb && ib && target && pipeline && bottom && top, "creation failed");
    REQUIRE(device.upload(*vb, 0, std::as_bytes(std::span(vertices))) &&
                device.upload(*ib, 0, std::as_bytes(std::span(kQuadIndices))),
            "upload failed");
    RayScene s{std::move(*vb), std::move(*ib), std::move(*target), std::move(*pipeline)};
    const RayInstance instance{.bottomLevel = &*bottom, .id = 3};

    const auto update = [&](bool refit) {
        RayGeometry geometry{.vertices = s.vertices, .vertexCount = 4, .indices = s.indices, .triangleCount = 2};
        CommandBuffer commands = device.acquireCommandBuffer();
        AccelerationStructurePass pass = commands.beginAccelerationStructurePass();
        if (refit) {
            geometry.vertexOffset = 12 * sizeof(float);
            pass.refit(*bottom, geometry);
        } else {
            pass.build(*bottom, geometry);
        }
        pass.build(*top, {&instance, 1});
        pass.end();
        return bool(commands.submit());
    };
    REQUIRE(update(false), "build failed");
    std::vector<Rgba8> pixels = trace(device, s, *top);
    REQUIRE(pixels.size() == kSize * kSize, "readback failed");
    const Hit before = hitAt(pixels, 16, 16), outside = hitAt(pixels, 2, 16);
    CHECK(before.hit && before.id == 3 && std::abs(before.distance - 10.0f) < 0.1f, "built: %d, id %d, %.2f m",
          before.hit, before.id, before.distance);
    CHECK(!outside.hit, "a hit outside the quad");

    REQUIRE(update(true), "refit failed");
    pixels = trace(device, s, *top);
    REQUIRE(pixels.size() == kSize * kSize, "readback failed");
    const Hit after = hitAt(pixels, 16, 16), wider = hitAt(pixels, 2, 16);
    std::printf("  built %.2f m, refitted %.2f m, outside %d -> %d\n", before.distance, after.distance, outside.hit,
                wider.hit);
    CHECK(after.hit && std::abs(after.distance - 13.0f) < 0.1f, "refitted: %d, %.2f m", after.hit, after.distance);
    CHECK(wider.hit && std::abs(wider.distance - 13.0f) < 0.1f, "the wider quad: %d, %.2f m", wider.hit,
          wider.distance);
}

namespace {

float halfToFloat(std::uint16_t h) {
    const int exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    const float sign = (h & 0x8000) ? -1.0f : 1.0f;
    if (exponent == 0) return sign * std::ldexp(float(mantissa), -24);
    if (exponent == 31) return sign * INFINITY;
    return sign * std::ldexp(float(mantissa | 0x400), exponent - 25);
}

// Pixel x, y of an Rgba16Float texture as floats.
std::array<float, 4> readHalf4(Device& device, const Texture& texture, std::uint32_t x, std::uint32_t y) {
    Result<std::vector<std::byte>> bytes = device.download({.texture = texture});
    if (!bytes) return {-1.0f, -1.0f, -1.0f, -1.0f};
    std::array<std::uint16_t, 4> h{};
    std::memcpy(h.data(), bytes->data() + (std::size_t(y) * texture.desc().width + x) * 8, 8);
    return {halfToFloat(h[0]), halfToFloat(h[1]), halfToFloat(h[2]), halfToFloat(h[3])};
}

} // namespace

// MetalFX spatial and temporal upscaling of a flat color: the output keeps it (gpu::Caps::*Upscaling).
QUENCH_TEST(upscale_flat_color) {
    QUENCH_GPU_DEVICE(device);
    const Caps& caps = device.caps();
    if (!caps.spatialUpscaling && !caps.temporalUpscaling) QUENCH_SKIP("no upscaling on this device");
    const TextureUsage sampledTarget = TextureUsage::ColorTarget | TextureUsage::Sampled;
    Result<Texture> color =
        device.createTexture({.format = TextureFormat::Rgba16Float, .usage = sampledTarget, .width = 32, .height = 32});
    Result<Texture> depth = device.createTexture({.format = TextureFormat::Depth32Float,
                                                  .usage = TextureUsage::DepthStencilTarget | TextureUsage::Sampled,
                                                  .width = 32,
                                                  .height = 32});
    Result<Texture> motion =
        device.createTexture({.format = TextureFormat::Rg16Float, .usage = sampledTarget, .width = 32, .height = 32});
    REQUIRE(color && depth && motion, "creation failed");
    {
        CommandBuffer commands = device.acquireCommandBuffer();
        const DepthTarget dt{.texture = *depth, .store = StoreOp::Store, .clearDepth = 0.5f};
        commands.beginRenderPass(ColorTarget{.texture = *color, .clear = {0.5f, 0.25f, 2.0f, 1.0f}}, &dt).end();
        commands.beginRenderPass(ColorTarget{.texture = *motion, .clear = {0.0f, 0.0f, 0.0f, 0.0f}}).end();
        REQUIRE(commands.submit(), "clear failed");
    }
    for (const UpscalerType type : {UpscalerType::Spatial, UpscalerType::Temporal}) {
        const bool temporal = type == UpscalerType::Temporal;
        if (!(temporal ? caps.temporalUpscaling : caps.spatialUpscaling)) continue;
        Result<Upscaler> upscaler = device.createUpscaler(
            {.type = type, .inputWidth = 32, .inputHeight = 32, .outputWidth = 64, .outputHeight = 64});
        Result<Texture> output = device.createTexture({.format = TextureFormat::Rgba16Float,
                                                       .usage = TextureUsage::Sampled | TextureUsage::ShaderWrite,
                                                       .width = 64,
                                                       .height = 64});
        REQUIRE(upscaler && output, "%s upscaler: %s", temporal ? "temporal" : "spatial",
                upscaler ? output.error().c_str() : upscaler.error().c_str());
        // A few frames, as a game runs it (the temporal one blends its history); the last one timed.
        for (int frame = 0; frame < 3; ++frame) {
            const bool timed = frame == 2;
            device.setPassTiming(timed);
            CommandBuffer commands = device.acquireCommandBuffer();
            UpscaleDesc desc{.color = *color, .output = *output};
            if (temporal) {
                desc.depth = *depth;
                desc.motion = *motion;
                desc.reset = frame == 0;
            }
            commands.beginTiming("upscale");
            commands.upscale(*upscaler, desc);
            commands.endTiming();
            Result<Fence> fence = commands.submitWithFence();
            REQUIRE(fence, "submit failed: %s", fence.error().c_str());
            if (!timed || !caps.gpuPassTiming) continue;
            device.wait(*fence);
            const std::vector<GpuTiming> timings = device.gpuTimings(*fence);
            const std::optional<double> ms = device.gpuMilliseconds(*fence);
            REQUIRE(timings.size() == 1 && ms, "%zu ranges", timings.size());
            const GpuTiming& t = timings[0];
            std::printf("  %s timed: %.4f ms busy, %.4f .. %.4f ms, command buffer %.4f ms\n",
                        temporal ? "temporal" : "spatial", t.milliseconds, t.startMilliseconds, t.endMilliseconds, *ms);
            CHECK(t.name == "upscale" && t.milliseconds > 0.0 && t.endMilliseconds <= *ms * 1.05 + 0.01,
                  "'%s': %.4f ms busy, ends at %.4f ms, command buffer %.4f ms", t.name.c_str(), t.milliseconds,
                  t.endMilliseconds, *ms);
        }
        device.setPassTiming(false);
        const std::array<float, 4> c = readHalf4(device, *output, 32, 32);
        std::printf("  %s: %.3f %.3f %.3f\n", temporal ? "temporal" : "spatial", c[0], c[1], c[2]);
        CHECK(std::abs(c[0] - 0.5f) < 0.05f && std::abs(c[1] - 0.25f) < 0.03f && std::abs(c[2] - 2.0f) < 0.2f,
              "%s: %.3f %.3f %.3f", temporal ? "temporal" : "spatial", c[0], c[1], c[2]);
    }
}
