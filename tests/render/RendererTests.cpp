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
#include <array>
#include <cmath>
#include <cstdio>
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

// The shadow scenes below: a 20 m ground seen from 9 m above, a sun from 45 degrees toward +x, so a caster 2 m up
// shadows the ground 2 m toward +x, and a 2 m square panel 2 m up (its texture u runs along +x).
RenderSettings shadowSettings() {
    RenderSettings s = plainSettings();
    s.shadowCascades = 2;
    s.shadowMapSize = 512;
    s.shadowDistance = 30.0f;
    return s;
}

const View kTopView = lookAt(glm::vec3(0, 9.0f, 0), glm::vec3(0), glm::vec3(0, 0, -1));
const glm::vec3 kSunDown = glm::normalize(glm::vec3(1.0f, -1.0f, 0.0f));
const glm::mat4 kPanelPlace = glm::translate(glm::mat4(1.0f), {0, 2.0f, 0});

// Only the sun lights the scene: no ambient, sky or sun disc, so a pixel's scene value is the sun's light on it.
Environment sunOnly() {
    Environment e;
    e.skyAmbient = e.groundAmbient = glm::vec3(0.0f);
    e.skyZenith = e.skyHorizon = e.skyGround = e.sunGlow = e.sunDisc = glm::vec3(0.0f);
    e.atmosphere = 0.0f;
    return e;
}

// The scene value (exposure 1) per channel of an 8-bit UNORM pixel, through the inverse of the default curve (ACES,
// per channel) and the target's gamma 2.2.
glm::vec3 sceneValue(Rgba8 c) {
    const float channels[3] = {float(c.r), float(c.g), float(c.b)};
    glm::vec3 out(0.0f);
    for (int i = 0; i < 3; ++i) {
        const float display = std::pow(channels[i] / 255.0f, 2.2f);
        float lo = 0.0f, hi = 64.0f;
        for (int k = 0; k < 40; ++k) {
            const float mid = 0.5f * (lo + hi);
            (toneMapSdr(ToneCurve::Aces, glm::vec3(mid)).r < display ? lo : hi) = mid;
        }
        out[i] = 0.5f * (lo + hi);
    }
    return out;
}

// A 16 x 16 RGBA8 texture: transparent where u < 0.5, opaque white elsewhere.
std::vector<std::byte> halfCutPixels() {
    std::vector<std::byte> pixels(16 * 16 * 4, std::byte{255});
    for (std::size_t y = 0; y < 16; ++y)
        for (std::size_t x = 0; x < 8; ++x) pixels[(y * 16 + x) * 4 + 3] = std::byte{0};
    return pixels;
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

QUENCH_TEST(renderer_cutout_shadows_let_light_through_the_holes) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, shadowSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (renderer->effects().shadowCascades == 0) QUENCH_SKIP("no shadow maps on this device");
    Result<SurfaceId> custom =
        renderer->addSurface({.vertexShader = shaders::render_test.find("surface_vs"),
                              .fragmentShader = shaders::render_test.find("surface_fs"),
                              .shadowFragmentShader = shaders::render_test.find("surface_shadow_fs"),
                              .name = "flat"});
    Result<SurfaceId> customWithout = renderer->addSurface({.vertexShader = shaders::render_test.find("surface_vs"),
                                                            .fragmentShader = shaders::render_test.find("surface_fs"),
                                                            .name = "flat without shadow entries"});
    REQUIRE(custom && customWithout, "custom surfaces");
    Scene scene(device);
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    const MeshId panel = addMesh(scene, planeMesh(glm::vec2(1.0f)));
    const std::vector<std::byte> cut = halfCutPixels();
    Result<TextureId> texture = scene.addTexture({.width = 16, .height = 16, .pixels = cut, .mipmaps = false});
    REQUIRE(texture, "%s", texture.error().c_str());
    scene.addInstance(
        {.mesh = ground, .material = scene.addMaterial({.baseColor = glm::vec4(0.5f), .roughness = 0.9f})});
    MaterialDesc fence{.alphaCutoff = 0.5f};
    fence.textures[0] = *texture;
    const MaterialId fenceMaterial = scene.addMaterial(fence);
    const InstanceId caster = scene.addInstance({.mesh = panel, .material = fenceMaterial, .transform = kPanelPlace});
    addSun(scene, kSunDown);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    // The holes (u < 0.5, x < 0 on the panel) shadow the ground at x 1..2, the solid half at x 2..3.
    const glm::ivec2 hole = pixelOf(kTopView, {1.5f, 0, 0.3f}), solid = pixelOf(kTopView, {2.5f, 0, 0.3f}),
                     lit = pixelOf(kTopView, {-3.0f, 0, 0.3f});
    const auto measure = [&](const char* what, bool holes) {
        const std::vector<Rgba8> pixels = renderOnce(device, *renderer, scene, kTopView, *target);
        REQUIRE(pixels.size() == kSize * kSize, "%zu pixels", pixels.size());
        const int h = sum(at(pixels, hole)), s = sum(at(pixels, solid)), l = sum(at(pixels, lit));
        CHECK(s * 10 < l * 7, "%s: the solid part shadows: %d vs lit %d", what, s, l);
        if (holes)
            CHECK(std::abs(h - l) * 20 < l, "%s: light passes through the holes: %d vs lit %d", what, h, l);
        else
            CHECK(h * 10 < l * 7, "%s: no holes, the shadow is solid: %d vs lit %d", what, h, l);
    };
    measure("alpha cutoff 0.5", true);
    CHECK(renderer->stats().shadowDraws >= 2, "shadow draws %u", renderer->stats().shadowDraws);
    fence.alphaCutoff = 0.0f;
    scene.setMaterial(fenceMaterial, fence);
    measure("no cutoff", false);

    // A surface's own opacity (the test surface: 0 where u < 0.5) cuts its shadow through its surface_shadow_fs;
    // without it the standard surface's opacity (1 here) is used.
    MaterialDesc own{.surface = *custom, .alphaCutoff = 0.5f};
    own.params[1] = glm::vec4(1.0f);
    scene.setInstanceMaterial(caster, scene.addMaterial(own));
    measure("surface opacity", true);
    own.surface = *customWithout;
    scene.setInstanceMaterial(caster, scene.addMaterial(own));
    measure("surface without shadow entries", false);
}

QUENCH_TEST(renderer_translucent_shadows_follow_opacity) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = shadowSettings();
    settings.sky = false;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (renderer->effects().shadowCascades == 0) QUENCH_SKIP("no shadow maps on this device");
    const bool mapped = renderer->effects().translucentShadows == TranslucentShadows::Map;
    Scene scene(device);
    scene.setEnvironment(sunOnly());
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    const MeshId panel = addMesh(scene, planeMesh(glm::vec2(1.0f)));
    const MeshId box = addMesh(scene, boxMesh(glm::vec3(0.4f)));
    const MaterialId grey = scene.addMaterial({.baseColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f), .roughness = 0.9f});
    scene.addInstance({.mesh = ground, .material = grey});
    // An opaque box away from the panel, and one above it (nearer the sun): neither is tinted by the panel.
    scene.addInstance(
        {.mesh = box, .material = grey, .transform = glm::translate(glm::mat4(1.0f), {-2.0f, 0.4f, -2.5f})});
    scene.addInstance(
        {.mesh = box, .material = grey, .transform = glm::translate(glm::mat4(1.0f), {0.0f, 2.8f, -0.6f})});
    MaterialDesc glass{.baseColor = glm::vec4(0.5f, 0.5f, 0.5f, 0.5f), .blend = BlendMode::AlphaBlend};
    const MaterialId glassMaterial = scene.addMaterial(glass);
    const InstanceId caster = scene.addInstance({.mesh = panel, .material = glassMaterial, .transform = kPanelPlace});
    addSun(scene, kSunDown);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    // The panel shadows the ground at x 1..3; the camera sees that part of the ground beside the panel.
    const glm::ivec2 behind = pixelOf(kTopView, {2.0f, 0, 0.4f}), lit = pixelOf(kTopView, {-2.5f, 0, 2.0f}),
                     boxShadow = pixelOf(kTopView, {-1.35f, 0, -2.5f}),
                     boxTop = pixelOf(kTopView, {-0.3f, 3.2f, -0.6f});
    for (const glm::ivec2 p : {behind, lit, boxShadow, boxTop})
        REQUIRE(p.x >= 0 && p.y >= 0 && p.x < int(kSize) && p.y < int(kSize), "pixel %d,%d in the target", p.x, p.y);
    scene.setVisible(caster, false);
    const std::vector<Rgba8> without = renderOnce(device, *renderer, scene, kTopView, *target);
    REQUIRE(without.size() == kSize * kSize, "%zu pixels", without.size());
    CHECK(renderer->stats().transmittanceDraws == 0, "no translucent casters, no transmittance draws");
    const glm::vec3 open = sceneValue(at(without, behind));
    REQUIRE(open.r > 0.2f, "the ground is lit: %f", double(open.r));
    scene.setVisible(caster, true);

    // The light through the caster: its scene value over the value without it, per channel.
    const auto transmitted = [&](const char* what, std::vector<Rgba8>* pixels = nullptr) {
        std::vector<Rgba8> image = renderOnce(device, *renderer, scene, kTopView, *target);
        CHECK(image.size() == kSize * kSize, "%s: %zu pixels", what, image.size());
        if (image.size() != kSize * kSize) return glm::vec3(-1.0f);
        const glm::vec3 t = sceneValue(at(image, behind)) / open;
        std::printf("  %s: transmitted %.3f %.3f %.3f\n", what, double(t.r), double(t.g), double(t.b));
        if (pixels) *pixels = std::move(image);
        return t;
    };
    std::vector<Rgba8> with;
    glm::vec3 t = transmitted("grey, opacity 0.5", &with);
    CHECK(std::abs(t.r - 0.5f) < 0.06f && std::abs(t.g - 0.5f) < 0.06f && std::abs(t.b - 0.5f) < 0.06f,
          "opacity 0.5 passes half the light: %.3f %.3f %.3f", double(t.r), double(t.g), double(t.b));
    if (mapped)
        CHECK(renderer->stats().transmittanceInstances >= 1 && renderer->stats().transmittanceDraws >= 1,
              "transmittance draws %u", renderer->stats().transmittanceDraws);
    // Opaque casters and receivers in front of the panel are as without it.
    for (const glm::ivec2 p : {lit, boxShadow, boxTop})
        CHECK(std::abs(sum(at(with, p)) - sum(at(without, p))) <= 3, "pixel %d,%d unchanged: %d vs %d", p.x, p.y,
              sum(at(with, p)), sum(at(without, p)));
    CHECK(sum(at(with, boxShadow)) * 10 < sum(at(with, lit)) * 3, "the opaque box shadows: %d vs %d",
          sum(at(with, boxShadow)), sum(at(with, lit)));

    for (const float opacity : {0.25f, 0.75f}) {
        glass.baseColor.a = opacity;
        scene.setMaterial(glassMaterial, glass);
        t = transmitted("grey");
        CHECK(std::abs(t.g - (1.0f - opacity)) < 0.06f, "opacity %.2f passes %.3f", double(opacity), double(t.g));
    }

    // Tinted: a red caster lets more red through. (1 - a) * lerp(1, hue, a) = (0.5, 0.28, 0.28) for this one.
    glass.baseColor = glm::vec4(0.9f, 0.1f, 0.1f, 0.5f);
    scene.setMaterial(glassMaterial, glass);
    t = transmitted("red");
    if (mapped)
        CHECK(std::abs(t.r - 0.5f) < 0.06f && std::abs(t.g - 0.28f) < 0.06f && std::abs(t.b - 0.28f) < 0.06f,
              "red, opacity 0.5: %.3f %.3f %.3f", double(t.r), double(t.g), double(t.b));

    glass.shadows = ShadowMode::Solid;
    scene.setMaterial(glassMaterial, glass);
    t = transmitted("solid");
    CHECK(t.r < 0.05f && t.g < 0.05f, "ShadowMode::Solid blocks the light: %.3f %.3f", double(t.r), double(t.g));
    glass.shadows = ShadowMode::None;
    scene.setMaterial(glassMaterial, glass);
    t = transmitted("none");
    CHECK(std::abs(t.r - 1.0f) < 0.03f && std::abs(t.g - 1.0f) < 0.03f, "ShadowMode::None: %.3f %.3f", double(t.r),
          double(t.g));

    // The fallback: the opacity dithered into the depth map, grey, smoothed by the filter.
    glass.shadows = ShadowMode::Auto;
    glass.baseColor = glm::vec4(0.5f, 0.5f, 0.5f, 0.5f);
    scene.setMaterial(glassMaterial, glass);
    settings.translucentShadows = TranslucentShadows::Dithered;
    renderer->setSettings(settings);
    CHECK(renderer->effects().translucentShadows == TranslucentShadows::Dithered, "dithered as asked");
    t = transmitted("dithered");
    CHECK(std::abs(t.g - 0.5f) < 0.2f, "dithered, opacity 0.5 passes about half: %.3f", double(t.g));
    CHECK(renderer->stats().transmittanceDraws == 0 && renderer->stats().shadowDraws >= 2,
          "dithered into the depth: %u transmittance, %u shadow draws", renderer->stats().transmittanceDraws,
          renderer->stats().shadowDraws);
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

namespace {

// Two stacked boxes of 0.5 x 1 x 0.5 m: the lower one (y 0..1) moved by joint 0, the upper one (y 1..2) by joint 1;
// the middle row of the upper box is shared half and half.
struct SkinnedBars {
    MeshData mesh;
    std::vector<SkinVertex> skin;
    MeshDesc desc() const { return {mesh.vertices, mesh.indices, "bars", skin, 2}; }
};

SkinnedBars skinnedBars() {
    SkinnedBars out;
    for (int part = 0; part < 2; ++part) {
        const MeshData box = boxMesh(glm::vec3(0.25f, 0.5f, 0.25f));
        const auto base = std::uint32_t(out.mesh.vertices.size());
        for (Vertex v : box.vertices) {
            v.position.y += 0.5f + float(part);
            out.mesh.vertices.push_back(v);
            SkinVertex s;
            s.joints = {std::uint16_t(part), 0, 0, 0};
            out.skin.push_back(s);
        }
        for (std::uint32_t i : box.indices) out.mesh.indices.push_back(base + i);
    }
    return out;
}

} // namespace

QUENCH_TEST(renderer_skins_on_the_gpu) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, plainSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    const SkinnedBars bars = skinnedBars();
    Result<MeshId> mesh = scene.addMesh(bars.desc());
    REQUIRE(mesh, "%s", mesh.error().c_str());
    CHECK(scene.jointCount(*mesh) == 2, "%u joints", scene.jointCount(*mesh));
    const MaterialId red = scene.addMaterial({.baseColor = glm::vec4(0.8f, 0.05f, 0.05f, 1.0f)});
    const InstanceId instance = scene.addInstance({.mesh = *mesh, .material = red});
    addSun(scene, glm::vec3(-0.3f, -1.0f, -0.5f));
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0.75f, 1.0f, 6.0f), glm::vec3(0.75f, 1.0f, 0.0f));
    const glm::vec3 upper(0.0f, 1.6f, 0.25f), moved(1.5f, 0.6f, 0.25f);
    const auto isRed = [](Rgba8 c) { return c.r > 2 * c.b && c.r > 60; };

    // Without a palette: the bind pose.
    std::vector<Rgba8> pixels = renderOnce(device, *renderer, scene, view, *target);
    CHECK(isRed(at(pixels, pixelOf(view, upper))), "bind pose: the upper bar is drawn");
    CHECK(!isRed(at(pixels, pixelOf(view, moved))), "bind pose: nothing to the right");
    CHECK(renderer->stats().skinnedInstances == 1 && renderer->stats().skinnedVertices == bars.mesh.vertices.size(),
          "stats: %u skinned instances, %u vertices", renderer->stats().skinnedInstances,
          renderer->stats().skinnedVertices);

    // Joint 1 moves the upper bar 1.5 m right and 1 m down, next to the lower one.
    const std::array<glm::mat4, 2> palette{glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), {1.5f, -1.0f, 0.0f})};
    scene.setJointPalette(instance, palette);
    pixels = renderOnce(device, *renderer, scene, view, *target);
    CHECK(!isRed(at(pixels, pixelOf(view, upper))), "posed: the upper bar left its bind place");
    CHECK(isRed(at(pixels, pixelOf(view, moved))), "posed: the upper bar is drawn at its new place");
    CHECK(isRed(at(pixels, pixelOf(view, glm::vec3(0.0f, 0.5f, 0.25f)))), "posed: the lower bar stays");

    // Culling uses the posed bounds: the bind pose is out of view, the pose brings it in.
    scene.setTransform(instance, glm::translate(glm::mat4(1.0f), {-20.0f, 0.0f, 0.0f}), true);
    const std::array<glm::mat4, 2> back{glm::translate(glm::mat4(1.0f), {20.0f, 0.0f, 0.0f}),
                                        glm::translate(glm::mat4(1.0f), {21.5f, -1.0f, 0.0f})};
    scene.setJointPalette(instance, back, true);
    pixels = renderOnce(device, *renderer, scene, view, *target);
    CHECK(renderer->stats().culled == 0, "%u culled", renderer->stats().culled);
    CHECK(isRed(at(pixels, pixelOf(view, moved))), "posed back into view from 20 m away");
}

QUENCH_TEST(renderer_skinned_motion_vectors) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.antiAliasing = AntiAliasing::Taa;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (renderer->effects().antiAliasing != AntiAliasing::Taa) QUENCH_SKIP("no TAA on this device");
    Scene scene(device);
    const SkinnedBars bars = skinnedBars();
    Result<MeshId> mesh = scene.addMesh(bars.desc());
    REQUIRE(mesh, "%s", mesh.error().c_str());
    const InstanceId instance = scene.addInstance({.mesh = *mesh, .material = scene.addMaterial({})});
    addSun(scene, glm::vec3(-0.3f, -1.0f, -0.5f));
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0.75f, 1.0f, 6.0f), glm::vec3(0.75f, 1.0f, 0.0f));
    for (int frame = 0; frame < 4; ++frame) {
        const std::array<glm::mat4, 2> palette{
            glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(0.1f * float(frame), 0.0f, 0.0f))};
        scene.setJointPalette(instance, palette);
        renderOnce(device, *renderer, scene, view, *target);
        CHECK(renderer->stats().movedInstances == 1, "frame %d: %u in the motion vectors", frame,
              renderer->stats().movedInstances);
    }
    renderOnce(device, *renderer, scene, view, *target);
    CHECK(renderer->stats().movedInstances == 0, "an unchanged pose has no motion");
    const std::array<glm::mat4, 2> jump{glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0, 0))};
    scene.setJointPalette(instance, jump, true);
    renderOnce(device, *renderer, scene, view, *target);
    CHECK(renderer->stats().movedInstances == 0, "a teleported pose has no motion");
}

// Cut-out and translucent casters draw the skinned vertices into the cascades too: a skinned panel posed 2.5 m
// toward -z shadows the ground there, and its bind place stays lit.
QUENCH_TEST(renderer_skinned_cutout_and_translucent_shadows) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = shadowSettings();
    settings.sky = false;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (renderer->effects().shadowCascades == 0) QUENCH_SKIP("no shadow maps on this device");
    Scene scene(device);
    scene.setEnvironment(sunOnly());
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    scene.addInstance(
        {.mesh = ground, .material = scene.addMaterial({.baseColor = glm::vec4(0.5f), .roughness = 0.9f})});
    const MeshData plane = planeMesh(glm::vec2(1.0f));
    const std::vector<SkinVertex> skin(plane.vertices.size()); // every vertex on joint 0
    Result<MeshId> panel = scene.addMesh({plane.vertices, plane.indices, "skinned panel", skin, 1});
    REQUIRE(panel, "%s", panel.error().c_str());
    const std::vector<std::byte> cut = halfCutPixels();
    Result<TextureId> texture = scene.addTexture({.width = 16, .height = 16, .pixels = cut, .mipmaps = false});
    REQUIRE(texture, "%s", texture.error().c_str());
    MaterialDesc fence{.alphaCutoff = 0.5f};
    fence.textures[0] = *texture;
    const MaterialId fenceMaterial = scene.addMaterial(fence);
    const MaterialId glassMaterial =
        scene.addMaterial({.baseColor = glm::vec4(0.5f, 0.5f, 0.5f, 0.5f), .blend = BlendMode::AlphaBlend});
    const InstanceId caster = scene.addInstance({.mesh = *panel, .material = fenceMaterial, .transform = kPanelPlace});
    const std::array<glm::mat4, 1> posed{glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, -2.5f})};
    scene.setJointPalette(caster, posed, true);
    addSun(scene, kSunDown);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    // The posed panel (z -3.5..-1.5) shadows the ground at x 1..3 there, its holes (x < 0 on the panel) at x 1..2;
    // the bind pose would shadow z -1..1.
    const glm::ivec2 hole = pixelOf(kTopView, {1.5f, 0, -2.5f}), solid = pixelOf(kTopView, {2.5f, 0, -2.5f}),
                     bindPlace = pixelOf(kTopView, {2.5f, 0, 0.3f}), lit = pixelOf(kTopView, {-3.0f, 0, -2.5f});

    std::vector<Rgba8> pixels = renderOnce(device, *renderer, scene, kTopView, *target);
    REQUIRE(pixels.size() == kSize * kSize, "%zu pixels", pixels.size());
    CHECK(renderer->stats().skinnedInstances == 1, "%u skinned instances", renderer->stats().skinnedInstances);
    int l = sum(at(pixels, lit));
    CHECK(sum(at(pixels, solid)) * 10 < l * 7, "cut-out: the posed solid half shadows: %d vs lit %d",
          sum(at(pixels, solid)), l);
    CHECK(std::abs(sum(at(pixels, hole)) - l) * 20 < l, "cut-out: light passes the posed holes: %d vs lit %d",
          sum(at(pixels, hole)), l);
    CHECK(std::abs(sum(at(pixels, bindPlace)) - l) * 20 < l, "cut-out: no shadow at the bind place: %d vs lit %d",
          sum(at(pixels, bindPlace)), l);

    scene.setInstanceMaterial(caster, glassMaterial);
    pixels = renderOnce(device, *renderer, scene, kTopView, *target);
    REQUIRE(pixels.size() == kSize * kSize, "%zu pixels", pixels.size());
    const glm::vec3 open = sceneValue(at(pixels, lit)), behind = sceneValue(at(pixels, solid)),
                    bind = sceneValue(at(pixels, bindPlace));
    std::printf("  translucent: posed %.3f, bind place %.3f of lit\n", double(behind.g / open.g),
                double(bind.g / open.g));
    CHECK(std::abs(behind.g / open.g - 0.5f) < 0.1f, "translucent: half the light behind the posed panel: %.3f",
          double(behind.g / open.g));
    CHECK(std::abs(bind.g / open.g - 1.0f) < 0.05f, "translucent: the bind place is lit: %.3f",
          double(bind.g / open.g));
    if (renderer->effects().translucentShadows == TranslucentShadows::Map)
        CHECK(renderer->stats().transmittanceDraws >= 1, "transmittance draws %u",
              renderer->stats().transmittanceDraws);
}

QUENCH_TEST(renderer_rejects_bad_skins) {
    QUENCH_GPU_DEVICE(device);
    Scene scene(device);
    SkinnedBars bars = skinnedBars();
    MeshDesc desc = bars.desc();
    desc.skin = std::span(bars.skin).first(bars.skin.size() - 1);
    CHECK(!scene.addMesh(desc), "a skin entry per vertex");
    desc = bars.desc();
    desc.jointCount = 0;
    CHECK(!scene.addMesh(desc), "joints needed");
    bars.skin[3].joints[1] = 7;
    bars.skin[3].weights = {40000, 25535, 0, 0};
    Result<MeshId> bad = scene.addMesh(bars.desc());
    CHECK(!bad && bad.error().find("joint 7") != std::string::npos, "joint out of range: %s",
          bad ? "accepted" : bad.error().c_str());
    bars.skin[3].weights = {65535, 0, 0, 0}; // a zero weight may name any joint
    CHECK(scene.addMesh(bars.desc()), "an unused joint index is ignored");
    CHECK(scene.meshCount() == 1, "%u meshes", scene.meshCount());
}

// ---------------------------------------------------------------- ray tracing and upscaling (native Metal backend)

namespace {

// Hard ray-traced shadows (one ray, no sun cone): comparable with the shadow maps pixel by pixel.
RenderSettings rayTraced(RenderSettings s) {
    s.sunShadows = SunShadows::RayTraced;
    s.shadowRays = 1;
    s.shadowSoftness = 0.0f;
    return s;
}

// Without gpu::Caps::rayTracing the renderer keeps the shadow maps.
bool rayTracingActive(gpu::Device& device, const Renderer& renderer) {
    const bool named = std::ranges::any_of(renderer.fallbacks(), [](const std::string& line) {
        return line.starts_with("ray-traced shadows: ") && line.ends_with("; shadow maps");
    });
    if (renderer.effects().sunShadows == SunShadows::RayTraced) {
        CHECK(!named, "no fallback named while the rays run");
        return true;
    }
    CHECK(!device.caps().rayTracing, "ray tracing on the device, shadow maps in the renderer");
    CHECK(renderer.effects().shadowCascades > 0, "the fallback has shadow maps");
    CHECK(named, "Renderer::fallbacks names the shadow fallback and its reason");
    return false;
}

// Mean absolute difference per channel (0..255) and the share of pixels with a channel more than 16 apart.
struct ImageDiff {
    double mean = 0.0;
    double over16 = 0.0;
};
ImageDiff compare(const std::vector<Rgba8>& a, const std::vector<Rgba8>& b) {
    ImageDiff d;
    if (a.size() != b.size() || a.empty()) return {255.0, 1.0};
    for (std::size_t i = 0; i < a.size(); ++i) {
        const int dr = std::abs(a[i].r - b[i].r), dg = std::abs(a[i].g - b[i].g), db = std::abs(a[i].b - b[i].b);
        d.mean += double(dr + dg + db) / 3.0;
        if (std::max({dr, dg, db}) > 16) d.over16 += 1.0;
    }
    d.mean /= double(a.size());
    d.over16 /= double(a.size());
    return d;
}

} // namespace

// An opaque box over the ground: the ray-traced shadow lies where the shadow map's does, and follows the box when it
// moves (the top level is built every frame).
QUENCH_TEST(renderer_ray_traced_shadows_match_shadow_maps) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = shadowSettings();
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (renderer->effects().shadowCascades == 0) QUENCH_SKIP("no shadow maps on this device");
    Scene scene(device);
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    const MeshId box = addMesh(scene, boxMesh(glm::vec3(0.5f)));
    const MaterialId grey = scene.addMaterial({.baseColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f), .roughness = 0.9f});
    scene.addInstance({.mesh = ground, .material = grey});
    const InstanceId caster =
        scene.addInstance({.mesh = box, .material = grey, .transform = glm::translate(glm::mat4(1.0f), {0, 1.5f, 0})});
    addSun(scene, kSunDown); // the box's shadow falls 1.5 m toward +x
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const std::vector<Rgba8> maps = renderOnce(device, *renderer, scene, kTopView, *target);

    renderer->setSettings(rayTraced(settings));
    if (!rayTracingActive(device, *renderer)) QUENCH_SKIP("no ray tracing: shadow maps");
    CHECK(renderer->effects().shadowCascades == 0, "no shadow maps with ray-traced shadows");
    const std::vector<Rgba8> rays = renderOnce(device, *renderer, scene, kTopView, *target);
    REQUIRE(rays.size() == kSize * kSize, "%zu pixels", rays.size());
    const RenderStats& stats = renderer->stats();
    CHECK(stats.rayInstances == 2 && stats.rayBuilds == 2 && stats.shadowDraws == 0,
          "%u instances in the top level, %u bottom levels built, %u shadow draws", stats.rayInstances, stats.rayBuilds,
          stats.shadowDraws);
    const glm::ivec2 shadow = pixelOf(kTopView, {1.5f, 0, 0}), lit = pixelOf(kTopView, {-3.0f, 0, 0});
    const ImageDiff d = compare(maps, rays);
    std::printf("  shadow %d (maps %d), lit %d (maps %d); image: mean %.2f, %.2f%% over 16\n", sum(at(rays, shadow)),
                sum(at(maps, shadow)), sum(at(rays, lit)), sum(at(maps, lit)), d.mean, d.over16 * 100.0);
    CHECK(sum(at(rays, shadow)) * 10 < sum(at(rays, lit)) * 7, "the ray-traced shadow is darker: %d vs %d",
          sum(at(rays, shadow)), sum(at(rays, lit)));
    CHECK(std::abs(sum(at(rays, shadow)) - sum(at(maps, shadow))) <= 6 &&
              std::abs(sum(at(rays, lit)) - sum(at(maps, lit))) <= 6,
          "as the shadow maps: shadow %d vs %d, lit %d vs %d", sum(at(rays, shadow)), sum(at(maps, shadow)),
          sum(at(rays, lit)), sum(at(maps, lit)));
    // The shadow maps' edges are filtered over about a texel, the hard ray-traced ones are not.
    CHECK(d.mean < 1.5 && d.over16 < 0.03, "image: mean %.2f, %.2f%% over 16", d.mean, d.over16 * 100.0);

    // The box moves 2 m toward -z: the next frame's top level has it there.
    scene.setTransform(caster, glm::translate(glm::mat4(1.0f), {0, 1.5f, -2.0f}));
    const std::vector<Rgba8> moved = renderOnce(device, *renderer, scene, kTopView, *target);
    REQUIRE(moved.size() == kSize * kSize, "%zu pixels", moved.size());
    const glm::ivec2 newShadow = pixelOf(kTopView, {1.5f, 0, -2.0f});
    CHECK(std::abs(sum(at(moved, shadow)) - sum(at(moved, lit))) * 20 < sum(at(moved, lit)) &&
              sum(at(moved, newShadow)) * 10 < sum(at(moved, lit)) * 7,
          "the shadow moved with the box: old place %d, new place %d, lit %d", sum(at(moved, shadow)),
          sum(at(moved, newShadow)), sum(at(moved, lit)));
    CHECK(renderer->stats().rayBuilds == 0, "no bottom level rebuilt for a move: %u", renderer->stats().rayBuilds);
}

// Cut-out holes and translucent tints in ray-traced shadows: as in the shadow maps.
QUENCH_TEST(renderer_ray_traced_cutout_and_translucent_shadows) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = shadowSettings();
    settings.sky = false;
    Result<Renderer> renderer = Renderer::create(device, rayTraced(settings));
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (renderer->effects().shadowCascades == 0 && renderer->effects().sunShadows != SunShadows::RayTraced)
        QUENCH_SKIP("no shadow maps on this device");
    if (!rayTracingActive(device, *renderer)) QUENCH_SKIP("no ray tracing: shadow maps");
    Scene scene(device);
    scene.setEnvironment(sunOnly());
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    const MeshId panel = addMesh(scene, planeMesh(glm::vec2(1.0f)));
    const std::vector<std::byte> cut = halfCutPixels();
    Result<TextureId> texture = scene.addTexture({.width = 16, .height = 16, .pixels = cut, .mipmaps = false});
    REQUIRE(texture, "%s", texture.error().c_str());
    scene.addInstance(
        {.mesh = ground, .material = scene.addMaterial({.baseColor = glm::vec4(0.5f), .roughness = 0.9f})});
    MaterialDesc fence{.alphaCutoff = 0.5f};
    fence.textures[0] = *texture;
    const InstanceId caster =
        scene.addInstance({.mesh = panel, .material = scene.addMaterial(fence), .transform = kPanelPlace});
    addSun(scene, kSunDown);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const glm::ivec2 hole = pixelOf(kTopView, {1.5f, 0, 0.3f}), solid = pixelOf(kTopView, {2.5f, 0, 0.3f}),
                     lit = pixelOf(kTopView, {-3.0f, 0, 0.3f});

    const auto both = [&](const char* what) {
        std::array<std::vector<Rgba8>, 2> images;
        for (int i = 0; i < 2; ++i) {
            renderer->setSettings(i == 0 ? settings : rayTraced(settings));
            images[std::size_t(i)] = renderOnce(device, *renderer, scene, kTopView, *target);
            images[std::size_t(i)].resize(kSize * kSize);
        }
        // The shadow maps' edges are filtered over about a texel, the hard ray-traced ones are not.
        const ImageDiff d = compare(images[0], images[1]);
        std::printf("  %s: image mean %.2f, %.2f%% over 16\n", what, d.mean, d.over16 * 100.0);
        CHECK(d.mean < 3.0 && d.over16 < 0.04, "%s: image mean %.2f, %.2f%% over 16", what, d.mean, d.over16 * 100.0);
        return images;
    };
    std::array<std::vector<Rgba8>, 2> images = both("cut-out");
    const std::vector<Rgba8>& rays = images[1];
    const int l = sum(at(rays, lit));
    CHECK(sum(at(rays, solid)) * 10 < l * 7, "the solid half shadows: %d vs lit %d", sum(at(rays, solid)), l);
    CHECK(std::abs(sum(at(rays, hole)) - l) * 20 < l, "light passes the holes: %d vs lit %d", sum(at(rays, hole)), l);
    CHECK(renderer->stats().rayInstances == 2, "%u instances in the top level", renderer->stats().rayInstances);

    // Translucent: the transmittance of the shadow maps, (1 - a) * lerp(1, hue, a).
    MaterialDesc glass{.baseColor = glm::vec4(0.5f, 0.5f, 0.5f, 0.5f), .blend = BlendMode::AlphaBlend};
    const MaterialId glassMaterial = scene.addMaterial(glass);
    scene.setInstanceMaterial(caster, glassMaterial);
    scene.setVisible(caster, false);
    renderer->setSettings(rayTraced(settings));
    const std::vector<Rgba8> without = renderOnce(device, *renderer, scene, kTopView, *target);
    scene.setVisible(caster, true);
    const glm::vec3 open = sceneValue(at(without, solid));
    for (const glm::vec4 color : {glm::vec4(0.5f, 0.5f, 0.5f, 0.5f), glm::vec4(0.9f, 0.1f, 0.1f, 0.5f)}) {
        glass.baseColor = color;
        scene.setMaterial(glassMaterial, glass);
        images = both(color.g > 0.2f ? "translucent grey" : "translucent red");
        const glm::vec3 t = sceneValue(at(images[1], solid)) / open;
        const glm::vec3 m = sceneValue(at(images[0], solid)) / open;
        std::printf("  transmitted %.3f %.3f %.3f (maps %.3f %.3f %.3f)\n", double(t.r), double(t.g), double(t.b),
                    double(m.r), double(m.g), double(m.b));
        const glm::vec3 hue = glm::vec3(color) / std::max({color.r, color.g, color.b});
        const glm::vec3 expected = (1.0f - color.a) * glm::mix(glm::vec3(1.0f), hue, color.a);
        CHECK(glm::all(glm::lessThan(glm::abs(t - expected), glm::vec3(0.06f))),
              "transmitted %.3f %.3f %.3f, expected %.3f %.3f %.3f", double(t.r), double(t.g), double(t.b),
              double(expected.r), double(expected.g), double(expected.b));
    }
}

// A skinned panel posed away from its bind place: the ray-traced shadow is where the pose puts it, its bottom level
// refitted every frame.
QUENCH_TEST(renderer_ray_traced_skinned_shadows) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = shadowSettings();
    settings.sky = false;
    Result<Renderer> renderer = Renderer::create(device, rayTraced(settings));
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (!rayTracingActive(device, *renderer)) QUENCH_SKIP("no ray tracing: shadow maps");
    Scene scene(device);
    scene.setEnvironment(sunOnly());
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    scene.addInstance(
        {.mesh = ground, .material = scene.addMaterial({.baseColor = glm::vec4(0.5f), .roughness = 0.9f})});
    const MeshData plane = planeMesh(glm::vec2(1.0f));
    const std::vector<SkinVertex> skin(plane.vertices.size()); // every vertex on joint 0
    Result<MeshId> panel = scene.addMesh({plane.vertices, plane.indices, "skinned panel", skin, 1});
    REQUIRE(panel, "%s", panel.error().c_str());
    const std::vector<std::byte> cut = halfCutPixels();
    Result<TextureId> texture = scene.addTexture({.width = 16, .height = 16, .pixels = cut, .mipmaps = false});
    REQUIRE(texture, "%s", texture.error().c_str());
    MaterialDesc fence{.alphaCutoff = 0.5f};
    fence.textures[0] = *texture;
    const InstanceId caster =
        scene.addInstance({.mesh = *panel, .material = scene.addMaterial(fence), .transform = kPanelPlace});
    addSun(scene, kSunDown);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const glm::ivec2 hole = pixelOf(kTopView, {1.5f, 0, -2.5f}), solid = pixelOf(kTopView, {2.5f, 0, -2.5f}),
                     bindPlace = pixelOf(kTopView, {2.5f, 0, 0.3f}), lit = pixelOf(kTopView, {-3.0f, 0, -2.5f});
    for (int frame = 0; frame < 3; ++frame) {
        // Toward -z by 2.5 m over three frames: the last frame's pose is the checked one.
        const std::array<glm::mat4, 1> posed{
            glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, -2.5f + 0.5f * float(2 - frame)})};
        scene.setJointPalette(caster, posed, frame == 0);
        const std::vector<Rgba8> pixels = renderOnce(device, *renderer, scene, kTopView, *target);
        REQUIRE(pixels.size() == kSize * kSize, "%zu pixels", pixels.size());
        const RenderStats& stats = renderer->stats();
        CHECK(frame == 0 ? stats.rayBuilds == 2 && stats.rayRefits == 0 : stats.rayBuilds == 0 && stats.rayRefits == 1,
              "frame %d: %u built, %u refitted", frame, stats.rayBuilds, stats.rayRefits);
        if (frame < 2) continue;
        const int l = sum(at(pixels, lit));
        std::printf("  posed: solid %d, hole %d, bind place %d, lit %d\n", sum(at(pixels, solid)),
                    sum(at(pixels, hole)), sum(at(pixels, bindPlace)), l);
        CHECK(sum(at(pixels, solid)) * 10 < l * 7, "the posed solid half shadows: %d vs lit %d", sum(at(pixels, solid)),
              l);
        CHECK(std::abs(sum(at(pixels, hole)) - l) * 20 < l, "light passes the posed holes: %d vs lit %d",
              sum(at(pixels, hole)), l);
        CHECK(std::abs(sum(at(pixels, bindPlace)) - l) * 20 < l, "no shadow at the bind place: %d vs lit %d",
              sum(at(pixels, bindPlace)), l);
    }
}

// Ray-traced ambient occlusion: the ground darkens next to a box standing on it, not far from it.
QUENCH_TEST(renderer_ray_traced_ambient_occlusion) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.ambientOcclusion = AmbientOcclusion::RayTraced;
    settings.occlusionRays = 8;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (renderer->effects().ambientOcclusion != AmbientOcclusion::RayTraced) {
        CHECK(!device.caps().rayTracing, "ray tracing on the device, unused by the renderer");
        QUENCH_SKIP("no ray tracing: no ambient occlusion");
    }
    Scene scene(device);
    Environment e = sunOnly();
    e.skyAmbient = e.groundAmbient = glm::vec3(1.0f); // ambient light only
    scene.setEnvironment(e);
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    const MeshId box = addMesh(scene, boxMesh(glm::vec3(0.5f)));
    const MaterialId grey = scene.addMaterial({.baseColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f), .roughness = 1.0f});
    scene.addInstance({.mesh = ground, .material = grey});
    scene.addInstance({.mesh = box, .material = grey, .transform = glm::translate(glm::mat4(1.0f), {0, 0.5f, 0})});
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    // 0.1 m and 2 m from the box's side (the camera sees the ground beside its top), the box's top.
    const glm::ivec2 near = pixelOf(kTopView, {0.6f, 0, 0}), far = pixelOf(kTopView, {2.5f, 0, 0}),
                     top = pixelOf(kTopView, {0.0f, 1.0f, 0.0f});
    const std::vector<Rgba8> on = renderOnce(device, *renderer, scene, kTopView, *target);
    REQUIRE(on.size() == kSize * kSize, "%zu pixels", on.size());
    settings.ambientOcclusion = AmbientOcclusion::None;
    renderer->setSettings(settings);
    const std::vector<Rgba8> off = renderOnce(device, *renderer, scene, kTopView, *target);
    REQUIRE(off.size() == kSize * kSize, "%zu pixels", off.size());
    const float nearRatio = sceneValue(at(on, near)).g / sceneValue(at(off, near)).g;
    const float farRatio = sceneValue(at(on, far)).g / sceneValue(at(off, far)).g;
    const float topRatio = sceneValue(at(on, top)).g / sceneValue(at(off, top)).g;
    std::printf("  ambient light with occlusion: %.3f next to the box, %.3f away, %.3f on its top\n", double(nearRatio),
                double(farRatio), double(topRatio));
    CHECK(nearRatio < 0.9f, "occluded next to the box: %.3f", double(nearRatio));
    CHECK(std::abs(farRatio - 1.0f) < 0.03f && std::abs(topRatio - 1.0f) < 0.03f,
          "open ground and the box's top are not occluded: %.3f, %.3f", double(farRatio), double(topRatio));
}

// MetalFX at half the target's size: the upscaled frame is close to the one rendered at full size.
QUENCH_TEST(renderer_upscaling) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.antiAliasing = AntiAliasing::Taa;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    const MeshId box = addMesh(scene, boxMesh(glm::vec3(0.6f)));
    scene.addInstance({.mesh = ground, .material = scene.addMaterial({.baseColor = glm::vec4(0.3f, 0.5f, 0.3f, 1)})});
    scene.addInstance({.mesh = box,
                       .material = scene.addMaterial({.baseColor = glm::vec4(0.8f, 0.2f, 0.1f, 1.0f)}),
                       .transform = glm::translate(glm::mat4(1.0f), {0, 0.6f, 0})});
    addSun(scene, glm::vec3(-0.3f, -1.0f, -0.5f));
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0, 2.0f, 4.0f), glm::vec3(0, 0.4f, 0));
    const auto settle = [&] {
        std::vector<Rgba8> pixels;
        for (int frame = 0; frame < 8; ++frame) pixels = renderOnce(device, *renderer, scene, view, *target);
        return pixels;
    };
    const std::vector<Rgba8> native = settle();
    for (const Upscaling upscaling : {Upscaling::Spatial, Upscaling::Temporal}) {
        const char* name = upscaling == Upscaling::Spatial ? "spatial" : "temporal";
        RenderSettings s = settings;
        s.renderScale = 0.5f;
        s.upscaling = upscaling;
        renderer->setSettings(s);
        if (renderer->effects().upscaling != upscaling) {
            CHECK(!(upscaling == Upscaling::Spatial ? device.caps().spatialUpscaling : device.caps().temporalUpscaling),
                  "%s upscaling on the device, bilinear in the renderer", name);
            continue;
        }
        const std::vector<Rgba8> scaled = settle();
        CHECK(renderer->stats().renderWidth == kSize / 2, "%s: render width %u", name, renderer->stats().renderWidth);
        const ImageDiff d = compare(native, scaled);
        std::printf("  %s at half size: mean %.2f, %.2f%% over 16\n", name, d.mean, d.over16 * 100.0);
        CHECK(d.mean < 4.0 && d.over16 < 0.12, "%s: mean %.2f, %.2f%% over 16", name, d.mean, d.over16 * 100.0);
    }
    if (renderer->effects().upscaling == Upscaling::Bilinear) QUENCH_SKIP("no upscaling on this device");
}
