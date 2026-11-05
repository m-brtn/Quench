// The standard frame on the GPU: small offscreen renders of procedural scenes, checked by their pixels.

#include "GpuTest.h"

#include <quench/render/Daylight.h>
#include <quench/render/MeshBuilder.h>
#include <quench/render/Projection.h>
#include <quench/render/Renderer.h>
#include <quench/render/ToneMap.h>

#include <render_test_shaders.h>

#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace quench;
using namespace quench::render;
using test::Rgba8;

namespace {

constexpr std::uint32_t kSize = 64;

Result<gpu::Texture> makeTarget(gpu::Device& device, gpu::TextureFormat format = gpu::TextureFormat::Rgba8Unorm) {
    return device.createTexture({.format = format,
                                 .usage = gpu::TextureUsage::ColorTarget,
                                 .width = kSize,
                                 .height = kSize,
                                 .name = "render test target"});
}

// Settings without anything that varies between frames or blurs across pixels.
RenderSettings plainSettings() {
    RenderSettings s;
    s.antiAliasing = AntiAliasing::None;
    s.bloom = false;
    s.shadowCascades = 0;
    s.vignette = 0.0f;
    s.dither = false;
    return s;
}

std::vector<Rgba8> renderOnce(gpu::Device& device, Renderer& renderer, Scene& scene, const View& view,
                              const gpu::Texture& target) {
    gpu::CommandBuffer commands = device.acquireCommandBuffer();
    renderer.render(scene, view, commands, target);
    CHECK(commands.submit(), "submit");
    return test::readRgba8(device, target);
}

View lookAt(glm::vec3 eye, glm::vec3 at, glm::vec3 up = glm::vec3(0, 1, 0)) {
    return {.view = glm::lookAt(eye, at, up), .fovY = 0.9f, .nearPlane = 0.1f};
}

// The pixel a world point lands on (the renderer's projection, square target).
glm::ivec2 pixelOf(const View& view, glm::vec3 p) {
    const glm::vec4 c = reversedPerspective(view.fovY, 1.0f, view.nearPlane) * view.view * glm::vec4(p, 1.0f);
    const glm::vec2 ndc = glm::vec2(c) / c.w;
    return {int((ndc.x * 0.5f + 0.5f) * float(kSize)), int((0.5f - ndc.y * 0.5f) * float(kSize))};
}

Rgba8 at(const std::vector<Rgba8>& pixels, glm::ivec2 p) { return pixels[std::size_t(p.y) * kSize + std::size_t(p.x)]; }
int sum(Rgba8 c) { return c.r + c.g + c.b; }

MeshId addMesh(Scene& scene, const MeshData& data) {
    Result<MeshId> mesh = scene.addMesh(data.desc());
    CHECK(mesh, "mesh: %s", mesh.error().c_str());
    return mesh ? *mesh : MeshId{};
}

LightId addSun(Scene& scene, glm::vec3 direction) {
    return scene.addLight({.type = LightType::Directional, .color = glm::vec3(5.0f), .direction = direction});
}

} // namespace

QUENCH_TEST(renderer_draws_surfaces_over_the_sky) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, plainSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    const MeshId box = addMesh(scene, boxMesh(glm::vec3(0.5f)));
    const MaterialId red = scene.addMaterial({.baseColor = glm::vec4(0.8f, 0.05f, 0.05f, 1.0f)});
    scene.addInstance({.mesh = box, .material = red});
    addSun(scene, glm::vec3(-0.3f, -1.0f, -0.5f));
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0, 1.5f, 3.0f), glm::vec3(0));
    const std::vector<Rgba8> pixels = renderOnce(device, *renderer, scene, view, *target);
    REQUIRE(pixels.size() == kSize * kSize, "%zu pixels", pixels.size());
    const Rgba8 center = at(pixels, pixelOf(view, glm::vec3(0, 0.5f, 0)));
    const Rgba8 sky = at(pixels, {kSize / 2, 1});
    CHECK(center.r > 2 * center.b && center.r > 60, "the box is red: %d %d %d", center.r, center.g, center.b);
    CHECK(sky.b > sky.r && sky.b > 100, "the sky is blue: %d %d %d", sky.r, sky.g, sky.b);
    const RenderStats& stats = renderer->stats();
    CHECK(stats.instances == 1 && stats.draws == 1 && stats.renderWidth == kSize, "stats: %u instances, %u draws",
          stats.instances, stats.draws);
}

QUENCH_TEST(renderer_shadow_darkens_the_ground) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.shadowCascades = 2;
    settings.shadowMapSize = 512;
    settings.shadowDistance = 30.0f;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (renderer->effects().shadowCascades == 0) QUENCH_SKIP("no shadow maps on this device");
    Scene scene(device);
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    const MeshId box = addMesh(scene, boxMesh(glm::vec3(0.5f)));
    const MaterialId grey = scene.addMaterial({.baseColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f), .roughness = 0.9f});
    scene.addInstance({.mesh = ground, .material = grey});
    scene.addInstance({.mesh = box, .material = grey, .transform = glm::translate(glm::mat4(1.0f), {0, 1.5f, 0})});
    addSun(scene, glm::normalize(glm::vec3(1.0f, -1.0f, 0.0f))); // the box's shadow falls 1.5 m toward +x
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0, 9.0f, 0), glm::vec3(0), glm::vec3(0, 0, -1));
    std::vector<Rgba8> pixels = renderOnce(device, *renderer, scene, view, *target);
    const glm::ivec2 shadowPixel = pixelOf(view, glm::vec3(1.5f, 0, 0)), litPixel = pixelOf(view, glm::vec3(-3, 0, 0));
    const int shadowed = sum(at(pixels, shadowPixel)), lit = sum(at(pixels, litPixel));
    CHECK(shadowed * 10 < lit * 7, "the shadow is darker: %d vs %d (pixels %d,%d and %d,%d)", shadowed, lit,
          shadowPixel.x, shadowPixel.y, litPixel.x, litPixel.y);
    CHECK(renderer->stats().shadowInstances >= 2 && renderer->stats().shadowDraws >= 2, "shadow draws %u",
          renderer->stats().shadowDraws);

    settings.shadowCascades = 0;
    renderer->setSettings(settings);
    pixels = renderOnce(device, *renderer, scene, view, *target);
    const int noShadow = sum(at(pixels, shadowPixel));
    CHECK(std::abs(noShadow - sum(at(pixels, litPixel))) < 12, "without shadows both are lit: %d vs %d", noShadow,
          sum(at(pixels, litPixel)));
}

QUENCH_TEST(renderer_bloom_spreads_bright_light) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.sky = false;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    Environment dark;
    dark.skyAmbient = dark.groundAmbient = glm::vec3(0.0f);
    dark.skyZenith = dark.skyHorizon = dark.skyGround = glm::vec3(0.0f);
    scene.setEnvironment(dark);
    const MeshId sphere = addMesh(scene, sphereMesh(0.25f, 12, 24));
    const MaterialId glow = scene.addMaterial({.baseColor = glm::vec4(0, 0, 0, 1), .emissive = glm::vec3(40.0f)});
    scene.addInstance({.mesh = sphere, .material = glow});
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0, 0, 4.0f), glm::vec3(0));
    const glm::ivec2 near{kSize / 2 + 9, kSize / 2};
    const int off = sum(at(renderOnce(device, *renderer, scene, view, *target), near));
    settings.bloom = true;
    settings.bloomStrength = 0.2f;
    renderer->setSettings(settings);
    const std::vector<Rgba8> pixels = renderOnce(device, *renderer, scene, view, *target);
    const int on = sum(at(pixels, near));
    CHECK(off <= 3, "black beside the sphere without bloom: %d", off);
    CHECK(on > off + 20, "bloom lights the pixels beside the sphere: %d vs %d", on, off);
    CHECK(sum(at(pixels, {kSize / 2, kSize / 2})) > 600, "the sphere is bright");
}

QUENCH_TEST(renderer_runs_game_passes_in_stage_order) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.shadowCascades = 2;
    settings.shadowMapSize = 256;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    const int cascades = renderer->effects().shadowCascades;
    std::string order;
    const auto record = [&order](const char* tag) {
        return [&order, tag](PassContext& c) {
            order += tag;
            if (c.cascade >= 0) order += std::to_string(c.cascade);
            order += ' ';
            CHECK(c.pass || std::string(tag) == "post", "%s: an open pass", tag);
        };
    };
    renderer->addPass(PassStage::Overlay, "overlay", record("overlay"));
    renderer->addPass(PassStage::Opaque, "opaque", record("opaque"));
    const PassId removed = renderer->addPass(PassStage::Opaque, "removed", record("removed"));
    renderer->addPass(PassStage::PostHdr, "post", record("post"));
    renderer->addPass(PassStage::Transparent, "transparent", record("transparent"));
    renderer->addPass(PassStage::Shadow, "shadow", record("shadow"));
    renderer->removePass(removed);
    Scene scene(device);
    addSun(scene, glm::vec3(0.2f, -1.0f, 0.1f));
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    renderOnce(device, *renderer, scene, lookAt(glm::vec3(0, 1, 3), glm::vec3(0)), *target);
    std::string expected;
    for (int c = 0; c < cascades; ++c) expected += "shadow" + std::to_string(c) + " ";
    expected += "opaque transparent post overlay ";
    CHECK(order == expected, "order '%s', expected '%s'", order.c_str(), expected.c_str());
}

QUENCH_TEST(renderer_custom_surface_shader) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.shadowCascades = 1;
    settings.shadowMapSize = 256;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Result<SurfaceId> surface =
        renderer->addSurface({.vertexShader = shaders::render_test.find("surface_vs"),
                              .fragmentShader = shaders::render_test.find("surface_fs"),
                              .shadowVertexShader = shaders::render_test.find("surface_shadow_vs"),
                              .name = "flat"});
    REQUIRE(surface, "%s", surface.error().c_str());
    CHECK(!renderer->addSurface({.vertexShader = shaders::render_test.find("surface_fs")}), "a fragment shader as the "
                                                                                            "vertex shader is refused");
    Scene scene(device);
    const MeshId box = addMesh(scene, boxMesh(glm::vec3(0.5f)));
    MaterialDesc material{.surface = *surface};
    material.params[0] = glm::vec4(0.0f, 4.0f, 0.0f, 0.0f);
    const MaterialId green = scene.addMaterial(material);
    // The vertex hook lifts the box by custom.y = 1: it is drawn around y = 1, not at the origin.
    scene.addInstance({.mesh = box, .material = green, .custom = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)});
    addSun(scene, glm::vec3(-0.2f, -1.0f, -0.3f));
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0, 1.0f, 4.0f), glm::vec3(0, 1.0f, 0));
    const std::vector<Rgba8> pixels = renderOnce(device, *renderer, scene, view, *target);
    const Rgba8 c = at(pixels, pixelOf(view, glm::vec3(0, 1.0f, 0.5f)));
    CHECK(c.g > 200 && c.r < 60 && c.b < 60, "the surface shader's emissive green: %d %d %d", c.r, c.g, c.b);
    const Rgba8 below = at(pixels, pixelOf(view, glm::vec3(0, 0.2f, 0.5f)));
    CHECK(below.g < 200 || below.b > 60, "nothing at the unlifted place: %d %d %d", below.r, below.g, below.b);
}

QUENCH_TEST(renderer_srgb_and_unorm_targets_match) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, plainSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    const MeshId sphere = addMesh(scene, sphereMesh(0.8f, 16, 32));
    scene.addInstance({.mesh = sphere, .material = scene.addMaterial({.roughness = 0.4f})});
    addSun(scene, glm::vec3(-0.4f, -1.0f, -0.6f));
    const View view = lookAt(glm::vec3(0, 0.5f, 3.0f), glm::vec3(0));
    std::vector<std::byte> images[2];
    int i = 0;
    for (const gpu::TextureFormat format : {gpu::TextureFormat::Rgba8Unorm, gpu::TextureFormat::Rgba8UnormSrgb}) {
        Result<gpu::Texture> target = makeTarget(device, format);
        REQUIRE(target, "%s", target.error().c_str());
        gpu::CommandBuffer commands = device.acquireCommandBuffer();
        renderer->render(scene, view, commands, *target);
        REQUIRE(commands.submit(), "submit");
        Result<std::vector<std::byte>> bytes = device.download({.texture = *target});
        REQUIRE(bytes, "%s", bytes.error().c_str());
        images[i++] = std::move(*bytes);
    }
    REQUIRE(images[0].size() == images[1].size(), "sizes");
    int worst = 0;
    for (std::size_t k = 0; k < images[0].size(); ++k)
        worst = std::max(worst, std::abs(int(images[0][k]) - int(images[1][k])));
    CHECK(worst <= 4, "UNORM and sRGB targets hold the same display values (largest difference %d)", worst);
}

namespace {

float halfToFloat(std::uint16_t h) {
    const int exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    const float value =
        exponent == 0 ? std::ldexp(float(mantissa), -24) : std::ldexp(float(mantissa | 0x400), exponent - 25);
    return (h & 0x8000) ? -value : value;
}

// The rgb values of an Rgba16Float target.
std::vector<glm::vec3> readRgb16f(gpu::Device& device, const gpu::Texture& target) {
    Result<std::vector<std::byte>> bytes = device.download({.texture = target});
    CHECK(bytes && bytes->size() == std::size_t(kSize) * kSize * 8, "download");
    std::vector<glm::vec3> pixels;
    if (!bytes || bytes->size() != std::size_t(kSize) * kSize * 8) return pixels;
    for (std::size_t i = 0; i < std::size_t(kSize) * kSize; ++i) {
        std::uint16_t h[3];
        std::memcpy(h, bytes->data() + i * 8, sizeof(h));
        pixels.emplace_back(halfToFloat(h[0]), halfToFloat(h[1]), halfToFloat(h[2]));
    }
    return pixels;
}

} // namespace

QUENCH_TEST(renderer_hdr_output_extends_highlights) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, plainSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    const MeshId sphere = addMesh(scene, sphereMesh(0.6f, 16, 32));
    scene.addInstance({.mesh = sphere,
                       .material = scene.addMaterial({.roughness = 0.4f}),
                       .transform = glm::translate(glm::mat4(1.0f), {-0.7f, 0, 0})});
    const MaterialId glow =
        scene.addMaterial({.baseColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), .emissive = glm::vec3(40.0f, 24.0f, 8.0f)});
    scene.addInstance({.mesh = sphere, .material = glow, .transform = glm::translate(glm::mat4(1.0f), {0.7f, 0, 0})});
    addSun(scene, glm::vec3(-0.4f, -1.0f, -0.6f));
    const View view = lookAt(glm::vec3(0, 0.3f, 3.0f), glm::vec3(0));
    const glm::ivec2 glowPixel = pixelOf(view, glm::vec3(0.7f, 0, 0.6f));

    Result<gpu::Texture> sdrTarget = makeTarget(device);
    Result<gpu::Texture> hdrTarget = makeTarget(device, gpu::TextureFormat::Rgba16Float);
    REQUIRE(sdrTarget && hdrTarget, "targets");
    const std::vector<Rgba8> sdr = renderOnce(device, *renderer, scene, view, *sdrTarget);
    const auto renderHdr = [&](float headroom) {
        gpu::CommandBuffer commands = device.acquireCommandBuffer();
        renderer->render(scene, view, commands, *hdrTarget,
                         {.composition = gpu::SwapchainComposition::HdrExtendedLinear, .headroom = headroom});
        CHECK(commands.submit(), "submit");
        return readRgb16f(device, *hdrTarget);
    };
    const std::vector<glm::vec3> flat = renderHdr(1.0f);
    const std::vector<glm::vec3> hdr = renderHdr(4.0f);
    REQUIRE(sdr.size() == flat.size() && flat.size() == hdr.size(), "pixel counts %zu %zu %zu", sdr.size(), flat.size(),
            hdr.size());

    // Headroom 1 is the SDR image in linear values; below the knee HDR adds nothing.
    int worstFlat = 0;
    float worstBelow = 0.0f, peak = 0.0f;
    for (std::size_t i = 0; i < sdr.size(); ++i) {
        const glm::vec3 encoded = glm::pow(glm::clamp(flat[i], 0.0f, 1.0f), glm::vec3(1.0f / 2.2f)) * 255.0f;
        worstFlat = std::max({worstFlat, std::abs(int(std::lround(encoded.r)) - sdr[i].r),
                              std::abs(int(std::lround(encoded.g)) - sdr[i].g),
                              std::abs(int(std::lround(encoded.b)) - sdr[i].b)});
        if (std::max({flat[i].r, flat[i].g, flat[i].b}) < kHdrKnee * 0.95f)
            worstBelow = std::max(worstBelow, glm::length(hdr[i] - flat[i]));
        peak = std::max({peak, hdr[i].r, hdr[i].g, hdr[i].b});
    }
    CHECK(worstFlat <= 2, "headroom 1 matches the SDR target (largest difference %d levels)", worstFlat);
    CHECK(worstBelow < 2e-3f, "below the knee HDR equals SDR (largest difference %f)", double(worstBelow));
    const glm::vec3 g = hdr[std::size_t(glowPixel.y) * kSize + std::size_t(glowPixel.x)];
    CHECK(peak <= 4.0f + 1e-2f && g.r > 2.0f && g.r > g.b, "the emissive sphere above paper white: %f %f %f (peak %f)",
          double(g.r), double(g.g), double(g.b), double(peak));
}

QUENCH_TEST(renderer_tone_curves_match_the_cpu_curves) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.sky = false; // the background is the clear color, tone-mapped like the scene
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    Result<gpu::Texture> sdrTarget = makeTarget(device);
    Result<gpu::Texture> hdrTarget = makeTarget(device, gpu::TextureFormat::Rgba16Float);
    REQUIRE(sdrTarget && hdrTarget, "targets");
    const View view = lookAt(glm::vec3(0, 0, 3), glm::vec3(0));
    const std::size_t centre = std::size_t(kSize / 2) * kSize + kSize / 2;
    constexpr float kHeadroom = 4.0f;
    for (const ToneCurve curve : {ToneCurve::Aces, ToneCurve::Agx}) {
        for (const float value : {0.05f, 0.3f, 1.0f, 3.0f, 12.0f, 60.0f}) {
            settings.toneCurve = curve;
            settings.clearColor = glm::vec3(value, value * 0.6f, value * 0.3f);
            renderer->setSettings(settings);
            const Rgba8 sdr = at(renderOnce(device, *renderer, scene, view, *sdrTarget), {kSize / 2, kSize / 2});
            const glm::vec3 want = glm::pow(toneMapSdr(curve, settings.clearColor), glm::vec3(1.0f / 2.2f)) * 255.0f;
            const int worst =
                std::max({std::abs(sdr.r - int(std::lround(want.r))), std::abs(sdr.g - int(std::lround(want.g))),
                          std::abs(sdr.b - int(std::lround(want.b)))});
            CHECK(worst <= 1, "curve %d, scene %f: SDR %d %d %d, the CPU curve %.1f %.1f %.1f", int(curve),
                  double(value), sdr.r, sdr.g, sdr.b, double(want.r), double(want.g), double(want.b));

            gpu::CommandBuffer commands = device.acquireCommandBuffer();
            renderer->render(scene, view, commands, *hdrTarget,
                             {.composition = gpu::SwapchainComposition::HdrExtendedLinear, .headroom = kHeadroom});
            REQUIRE(commands.submit(), "submit");
            const std::vector<glm::vec3> hdr = readRgb16f(device, *hdrTarget);
            REQUIRE(hdr.size() == std::size_t(kSize) * kSize, "%zu pixels", hdr.size());
            const glm::vec3 cpu = toneMapHdr(curve, settings.clearColor, kHeadroom);
            const glm::vec3 d = glm::abs(hdr[centre] - cpu);
            CHECK(std::max({d.r, d.g, d.b}) <= 0.004f + 0.002f * std::max({cpu.r, cpu.g, cpu.b}),
                  "curve %d, scene %f: HDR %f %f %f, the CPU curve %f %f %f", int(curve), double(value),
                  double(hdr[centre].r), double(hdr[centre].g), double(hdr[centre].b), double(cpu.r), double(cpu.g),
                  double(cpu.b));
        }
    }
}

QUENCH_TEST(renderer_sun_disc_outshines_its_glow) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, plainSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    // A low sun, where the scattering sky's glow at the sun is brighter than the palette's disc radiance.
    const glm::vec3 towardSun = skyDirection(9.0f, 0.0f);
    const Daylight day = daylightFor(towardSun);
    scene.setEnvironment(day.environment);
    scene.addLight({.type = LightType::Directional, .color = day.sunColor, .direction = -towardSun});
    const View view = {
        .view = glm::lookAt(glm::vec3(0.0f), towardSun, glm::vec3(0, 1, 0)), .fovY = 0.2f, .nearPlane = 0.1f};
    const std::size_t centre = std::size_t(kSize / 2) * kSize + kSize / 2;
    const float discPixels =
        std::tan(day.environment.sunAngularRadius) / std::tan(view.fovY * 0.5f) * float(kSize / 2) + 2.0f;
    const auto outsideDisc = [discPixels](std::size_t i) {
        const float dx = float(int(i % kSize) - int(kSize / 2)), dy = float(int(i / kSize) - int(kSize / 2));
        return dx * dx + dy * dy > discPixels * discPixels;
    };

    Result<gpu::Texture> sdrTarget = makeTarget(device);
    Result<gpu::Texture> hdrTarget = makeTarget(device, gpu::TextureFormat::Rgba16Float);
    REQUIRE(sdrTarget && hdrTarget, "targets");
    const std::vector<Rgba8> sdr = renderOnce(device, *renderer, scene, view, *sdrTarget);
    REQUIRE(sdr.size() == std::size_t(kSize) * kSize, "%zu pixels", sdr.size());
    int brightestSky = 0;
    for (std::size_t i = 0; i < sdr.size(); ++i)
        if (outsideDisc(i)) brightestSky = std::max(brightestSky, sum(sdr[i]));
    CHECK(sum(sdr[centre]) >= brightestSky && sum(sdr[centre]) >= 3 * 250,
          "SDR: the disc is near white and not darker than its glow: %d %d %d, brightest sky %d", sdr[centre].r,
          sdr[centre].g, sdr[centre].b, brightestSky);

    gpu::CommandBuffer commands = device.acquireCommandBuffer();
    renderer->render(scene, view, commands, *hdrTarget,
                     {.composition = gpu::SwapchainComposition::HdrExtendedLinear, .headroom = 8.0f});
    REQUIRE(commands.submit(), "submit");
    const std::vector<glm::vec3> hdr = readRgb16f(device, *hdrTarget);
    REQUIRE(hdr.size() == sdr.size(), "%zu pixels", hdr.size());
    float glow = 0.0f;
    for (std::size_t i = 0; i < hdr.size(); ++i)
        if (outsideDisc(i)) glow = std::max({glow, hdr[i].r, hdr[i].g, hdr[i].b});
    const glm::vec3 disc = hdr[centre];
    CHECK(std::min({disc.r, disc.g, disc.b}) > 1.25f * glow,
          "HDR (headroom 8): the disc %f %f %f well above the brightest glow %f", double(disc.r), double(disc.g),
          double(disc.b), double(glow));
}

QUENCH_TEST(renderer_taa_motion_vectors_and_render_scale) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.antiAliasing = AntiAliasing::Taa;
    settings.bloom = true;
    settings.renderScale = 0.5f;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    CHECK(renderer->effects().antiAliasing == AntiAliasing::Taa, "TAA available on %s", device.caps().driver.c_str());
    Scene scene(device);
    const MeshId box = addMesh(scene, boxMesh(glm::vec3(0.4f)));
    const MaterialId white = scene.addMaterial({});
    const InstanceId moving = scene.addInstance({.mesh = box, .material = white});
    addSun(scene, glm::vec3(-0.3f, -1.0f, -0.5f));
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0, 1, 4), glm::vec3(0));
    std::vector<Rgba8> pixels;
    for (int frame = 0; frame < 4; ++frame) {
        scene.setTransform(moving, glm::translate(glm::mat4(1.0f), glm::vec3(0.1f * float(frame), 0, 0)));
        pixels = renderOnce(device, *renderer, scene, view, *target);
        if (frame > 0)
            CHECK(renderer->stats().movedInstances == 1, "frame %d: %u moved", frame, renderer->stats().movedInstances);
    }
    CHECK(renderer->stats().renderWidth == kSize / 2, "render width %u", renderer->stats().renderWidth);
    scene.setTransform(moving, glm::translate(glm::mat4(1.0f), glm::vec3(0.3f, 0, 0)), true);
    pixels = renderOnce(device, *renderer, scene, view, *target);
    CHECK(renderer->stats().movedInstances == 0, "a teleport has no motion");
    const Rgba8 c = at(pixels, pixelOf(view, glm::vec3(0.3f, 0.4f, 0.0f)));
    CHECK(sum(c) > 150, "the box is drawn: %d %d %d", c.r, c.g, c.b);
}
