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

// A flat ground under a sun 45 degrees up, seen from above: a normal map that tilts the normal toward the sun
// (along the tangent, +u, or the bitangent, -v) brightens it by about 1 / cos 45, a tilt away darkens it; an
// emissive map multiplies the material's emissive.
QUENCH_TEST(renderer_normal_and_emissive_maps) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.sky = false;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    scene.setEnvironment(sunOnly());
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    const auto flatTexture = [&](std::array<std::uint8_t, 3> rgb, bool srgb) {
        std::vector<std::byte> pixels(4 * 4 * 4, std::byte{255});
        for (std::size_t i = 0; i < 16; ++i)
            for (std::size_t c = 0; c < 3; ++c) pixels[i * 4 + c] = std::byte{rgb[c]};
        Result<TextureId> texture = scene.addTexture({.width = 4, .height = 4, .pixels = pixels, .srgb = srgb});
        CHECK(texture, "texture: %s", texture ? "" : texture.error().c_str());
        return texture ? *texture : TextureId{};
    };
    // Tangent-space normals (x along +u, y toward -v) as texels: 0.5 + 0.5 * n.
    const TextureId towardMinusU = flatTexture({37, 128, 218}, false);
    const TextureId towardPlusU = flatTexture({218, 128, 218}, false);
    const TextureId towardMinusV = flatTexture({128, 218, 218}, false);
    MaterialDesc desc{.baseColor = glm::vec4(0.5f), .roughness = 0.9f};
    const MaterialId material = scene.addMaterial(desc);
    scene.addInstance({.mesh = ground, .material = material});
    const LightId sun = addSun(scene, kSunDown);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const glm::ivec2 center = pixelOf(kTopView, glm::vec3(0.0f));
    const auto measure = [&](TextureId normals) {
        desc.textures[2] = normals;
        scene.setMaterial(material, desc);
        return sceneValue(at(renderOnce(device, *renderer, scene, kTopView, *target), center)).g;
    };
    // The plane's u runs along +x and its v along +z; the sun travels toward +x (it comes from -x).
    const float flat = measure({});
    const float toward = measure(towardMinusU), away = measure(towardPlusU);
    CHECK(toward > flat * 1.25f && away < flat * 0.3f, "tangent: flat %f, toward the sun %f, away %f", double(flat),
          double(toward), double(away));
    // A sun from -z: the bitangent (toward -v, -z) faces it.
    scene.setLight(sun, {.type = LightType::Directional,
                         .color = glm::vec3(5.0f),
                         .direction = glm::normalize(glm::vec3(0.0f, -1.0f, 1.0f))});
    const float flatZ = measure({}), towardZ = measure(towardMinusV);
    CHECK(towardZ > flatZ * 1.25f, "bitangent: flat %f, toward the sun %f", double(flatZ), double(towardZ));

    scene.removeLight(sun);
    desc.textures[2] = {};
    desc.emissive = glm::vec3(1.0f);
    desc.textures[3] = flatTexture({255, 0, 0}, true);
    scene.setMaterial(material, desc);
    const Rgba8 glow = at(renderOnce(device, *renderer, scene, kTopView, *target), center);
    CHECK(glow.r > 120 && glow.g < 8 && glow.b < 8, "emissive map: %u %u %u", glow.r, glow.g, glow.b);
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

// A moving cut-out panel takes the motion vector pass without a depth target (velocity_cutout_fs): its solid half is
// drawn, and the ground stays visible through its holes.
QUENCH_TEST(renderer_moving_cutout_motion_vectors) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.antiAliasing = AntiAliasing::Taa;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    const MeshId panel = addMesh(scene, planeMesh(glm::vec2(1.0f)));
    const std::vector<std::byte> cut = halfCutPixels();
    Result<TextureId> texture = scene.addTexture({.width = 16, .height = 16, .pixels = cut, .mipmaps = false});
    REQUIRE(texture, "%s", texture.error().c_str());
    scene.addInstance(
        {.mesh = ground, .material = scene.addMaterial({.baseColor = glm::vec4(0.1f, 0.1f, 0.6f, 1.0f)})});
    MaterialDesc fence{.baseColor = glm::vec4(0.8f, 0.05f, 0.05f, 1.0f), .alphaCutoff = 0.5f};
    fence.textures[0] = *texture;
    const InstanceId moving =
        scene.addInstance({.mesh = panel, .material = scene.addMaterial(fence), .transform = kPanelPlace});
    scene.setEnvironment(sunOnly());
    scene.addLight({.type = LightType::Directional, .color = glm::vec3(1.0f), .direction = glm::vec3(0, -1, 0)});
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    std::vector<Rgba8> pixels;
    float x = 0.0f;
    for (int frame = 0; frame < 6; ++frame) {
        x = 0.1f * float(frame);
        scene.setTransform(moving, glm::translate(kPanelPlace, {x, 0, 0}));
        pixels = renderOnce(device, *renderer, scene, kTopView, *target);
        if (frame > 0)
            CHECK(renderer->stats().movedInstances == 1, "frame %d: %u moved", frame, renderer->stats().movedInstances);
    }
    const Rgba8 solid = at(pixels, pixelOf(kTopView, {x + 0.5f, 2.0f, 0.3f}));
    const Rgba8 hole = at(pixels, pixelOf(kTopView, {x - 0.5f, 2.0f, 0.3f}));
    CHECK(solid.r > 2 * solid.b, "the solid half is drawn: %d %d %d", solid.r, solid.g, solid.b);
    CHECK(hole.b > 2 * hole.r, "the ground through the hole: %d %d %d", hole.r, hole.g, hole.b);
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

// A palette that does not change (a held animation) is not skinned again: the vertices are copied from the previous
// frame's, and the image is the same as with skinning.
QUENCH_TEST(renderer_reuses_unchanged_skins) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, plainSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    const SkinnedBars bars = skinnedBars();
    Result<MeshId> mesh = scene.addMesh(bars.desc());
    REQUIRE(mesh, "%s", mesh.error().c_str());
    const MaterialId red = scene.addMaterial({.baseColor = glm::vec4(0.8f, 0.05f, 0.05f, 1.0f)});
    // A second instance, added first: hidden in one frame, it can move the other's range in the skinned buffer.
    const InstanceId other = scene.addInstance(
        {.mesh = *mesh, .material = red, .transform = glm::translate(glm::mat4(1.0f), {-1.5f, 0.0f, 0.0f})});
    const InstanceId instance = scene.addInstance({.mesh = *mesh, .material = red});
    addSun(scene, glm::vec3(-0.3f, -1.0f, -0.5f));
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0.75f, 1.0f, 6.0f), glm::vec3(0.75f, 1.0f, 0.0f));
    const std::array<glm::mat4, 2> palette{glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), {1.5f, -1.0f, 0.0f})};
    const std::array<glm::mat4, 2> otherPalette{glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), {0.0f, 0.3f, 0.0f})};

    scene.setJointPalette(instance, palette);
    scene.setJointPalette(other, otherPalette);
    const std::vector<Rgba8> skinned = renderOnce(device, *renderer, scene, view, *target);
    CHECK(renderer->stats().skinnedInstances == 2 && renderer->stats().skinnedReused == 0,
          "first frame: %u skinned, %u reused", renderer->stats().skinnedInstances, renderer->stats().skinnedReused);
    for (int frame = 0; frame < 3; ++frame) {
        scene.setJointPalette(instance, palette); // the same matrices: no change
        scene.setVisible(other, frame != 1);
        const std::vector<Rgba8> held = renderOnce(device, *renderer, scene, view, *target);
        const std::uint32_t expected = frame == 0 ? 2u : 1u;
        CHECK(renderer->stats().skinnedReused == expected, "frame %d: %u reused, expected %u", frame,
              renderer->stats().skinnedReused, expected);
        const bool same = std::equal(held.begin(), held.end(), skinned.begin(), skinned.end(),
                                     [](Rgba8 x, Rgba8 y) { return std::memcmp(&x, &y, sizeof(Rgba8)) == 0; });
        if (frame != 1) CHECK(same, "frame %d: the same image as when skinned", frame);
        CHECK(renderer->stats().movedInstances == 0, "frame %d: an unchanged pose has no motion", frame);
    }
    const std::array<glm::mat4, 2> changed{glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), {1.4f, -1.0f, 0.0f})};
    scene.setJointPalette(instance, changed);
    renderOnce(device, *renderer, scene, view, *target);
    CHECK(renderer->stats().skinnedReused == 1, "a changed palette is skinned again (the other one is reused): %u",
          renderer->stats().skinnedReused);
}

namespace {

// A bar of 0.4 x 2 x 0.4 m standing on y = 0, in 20 segments: joint 0 moves it below 0.5 m, joint 1 above 1.5 m, and
// the weights go linearly from one to the other in between.
SkinnedBars twistBar() {
    SkinnedBars out;
    for (int segment = 0; segment < 20; ++segment) {
        const MeshData box = boxMesh(glm::vec3(0.2f, 0.05f, 0.2f));
        const auto base = std::uint32_t(out.mesh.vertices.size());
        for (Vertex v : box.vertices) {
            v.position.y += 0.05f + 0.1f * float(segment);
            out.mesh.vertices.push_back(v);
            const float upper = std::clamp(v.position.y - 0.5f, 0.0f, 1.0f);
            SkinVertex s;
            s.joints = {0, 1, 0, 0};
            s.weights = {std::uint16_t(std::lround((1.0f - upper) * 65535.0f)),
                         std::uint16_t(std::lround(upper * 65535.0f)), 0, 0};
            out.skin.push_back(s);
        }
        for (std::uint32_t i : box.indices) out.mesh.indices.push_back(base + i);
    }
    return out;
}

// The width in pixels of what is red in the image row of height y (the bar seen from the front, at x = 0).
int redWidth(const std::vector<Rgba8>& pixels, const View& view, float y) {
    const int row = pixelOf(view, {0.0f, y, 0.0f}).y;
    int count = 0;
    for (std::uint32_t x = 0; x < kSize; ++x) {
        const Rgba8 c = at(pixels, {int(x), row});
        count += c.r > 2 * c.b && c.r > 60 ? 1 : 0;
    }
    return count;
}

} // namespace

// The upper joint twisted by 180 degrees about the bar: linear blending pulls the middle of the bar onto its axis (the
// candy wrapper), dual quaternions turn it by 90 degrees and keep its width. With joints that only move, the two are
// the same; a uniform scale is kept by both.
QUENCH_TEST(renderer_dual_quaternion_skinning) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, plainSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    const SkinnedBars bar = twistBar();
    Result<MeshId> mesh = scene.addMesh(bar.desc());
    REQUIRE(mesh, "%s", mesh.error().c_str());
    const MaterialId red = scene.addMaterial({.baseColor = glm::vec4(0.8f, 0.05f, 0.05f, 1.0f)});
    const InstanceId instance = scene.addInstance({.mesh = *mesh, .material = red});
    addSun(scene, glm::vec3(-0.3f, -1.0f, -0.5f));
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0.0f, 1.0f, 2.5f), glm::vec3(0.0f, 1.0f, 0.0f)); // y -0.2 to 2.2 in view
    const float full = float(pixelOf(view, {0.2f, 1.0f, 0.2f}).x - pixelOf(view, {-0.2f, 1.0f, 0.2f}).x);

    const auto render = [&](Skinning skinning, std::span<const glm::mat4> palette) {
        scene.setSkinning(instance, skinning);
        scene.setJointPalette(instance, palette, true);
        return renderOnce(device, *renderer, scene, view, *target);
    };
    const std::array<glm::mat4, 2> twist{glm::mat4(1.0f),
                                         glm::rotate(glm::mat4(1.0f), glm::radians(180.0f), {0.0f, 1.0f, 0.0f})};
    const std::vector<Rgba8> linear = render(Skinning::Linear, twist);
    const std::vector<Rgba8> dual = render(Skinning::DualQuaternion, twist);
    CHECK(renderer->stats().skinnedInstances == 1, "%u skinned", renderer->stats().skinnedInstances);
    const int linearMiddle = redWidth(linear, view, 1.0f), dualMiddle = redWidth(dual, view, 1.0f);
    CHECK(float(linearMiddle) < 0.15f * full, "linear: the middle collapses (%d of %.0f pixels)", linearMiddle,
          double(full));
    CHECK(float(dualMiddle) > 0.85f * full, "dual quaternion: the middle keeps its width (%d of %.0f pixels)",
          dualMiddle, double(full));
    for (const float y : {0.25f, 1.75f}) {
        CHECK(float(redWidth(linear, view, y)) > 0.85f * full && float(redWidth(dual, view, y)) > 0.85f * full,
              "y %.2f, one joint: the full width in both (%d, %d of %.0f)", double(y), redWidth(linear, view, y),
              redWidth(dual, view, y), double(full));
    }
    // A quarter of the way up the blend (weights 0.75 and 0.25): linear at half the width, dual quaternion turned by
    // 45 degrees and seen across the square's diagonal.
    const int linearQuarter = redWidth(linear, view, 0.75f), dualQuarter = redWidth(dual, view, 0.75f);
    CHECK(std::abs(float(linearQuarter) - 0.5f * full) < 0.25f * full, "linear at 0.75 m: %d of %.0f", linearQuarter,
          double(full));
    CHECK(std::abs(float(dualQuarter) - 1.414f * full) < 0.25f * full, "dual quaternion at 0.75 m: %d of %.0f",
          dualQuarter, double(full));

    const std::array<glm::mat4, 2> moved{glm::translate(glm::mat4(1.0f), {0.3f, 0.0f, 0.0f}),
                                         glm::translate(glm::mat4(1.0f), {-0.2f, 0.3f, 0.0f})};
    const std::vector<Rgba8> movedLinear = render(Skinning::Linear, moved);
    const std::vector<Rgba8> movedDual = render(Skinning::DualQuaternion, moved);
    int differing = 0;
    for (std::size_t i = 0; i < movedLinear.size(); ++i)
        differing += std::abs(sum(movedLinear[i]) - sum(movedDual[i])) > 16 ? 1 : 0;
    CHECK(differing <= 2, "joints that only move: the same image (%d pixels differ)", differing);

    const glm::mat4 rigid =
        glm::rotate(glm::translate(glm::mat4(1.0f), {0.25f, 0.15f, 0.0f}), 0.6f, {0.0f, 0.0f, 1.0f});
    const std::array<glm::mat4, 2> rigidPair{rigid, rigid};
    const std::vector<Rgba8> rigidLinear = render(Skinning::Linear, rigidPair);
    const std::vector<Rgba8> rigidDual = render(Skinning::DualQuaternion, rigidPair);
    differing = 0;
    for (std::size_t i = 0; i < rigidLinear.size(); ++i)
        differing += std::abs(sum(rigidLinear[i]) - sum(rigidDual[i])) > 16 ? 1 : 0;
    CHECK(differing <= 2, "one rigid motion (turned and moved): the same image (%d pixels differ)", differing);

    const std::array<glm::mat4, 2> scaled{
        glm::mat4(1.0f), glm::scale(glm::translate(glm::mat4(1.0f), {0.0f, -1.0f, 0.0f}), glm::vec3(2.0f))};
    const std::vector<Rgba8> scaledDual = render(Skinning::DualQuaternion, scaled);
    CHECK(float(redWidth(scaledDual, view, 2.1f)) > 1.7f * full, "a uniform scale of 2 is kept: %d of %.0f",
          redWidth(scaledDual, view, 2.1f), double(full));
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

namespace {

// The renderer's blend shapes on the CPU, as Scene::setMorphWeights documents them: the weighted offsets added, the
// normal normalized, the tangent made perpendicular to it and normalized.
std::vector<Vertex> morphOnCpu(std::span<const Vertex> base, std::span<const MorphVertex> morphs,
                               std::span<const float> weights) {
    std::vector<Vertex> out(base.begin(), base.end());
    for (std::size_t v = 0; v < out.size(); ++v) {
        glm::vec3 dp(0.0f), dn(0.0f), dt(0.0f);
        for (std::size_t k = 0; k < weights.size(); ++k) {
            const MorphVertex& m = morphs[k * base.size() + v];
            dp += weights[k] * m.position;
            dn += weights[k] * m.normal;
            dt += weights[k] * m.tangent;
        }
        out[v].position += dp;
        out[v].normal = glm::normalize(out[v].normal + dn);
        glm::vec3 t = glm::vec3(out[v].tangent) + dt;
        t = glm::normalize(t - out[v].normal * glm::dot(out[v].normal, t));
        out[v].tangent = glm::vec4(t, out[v].tangent.w);
    }
    return out;
}

// Two blend shapes of a mesh: "stretch" (its top 0.6 m up and 0.2 m out in x, its normals tilted up, its tangents
// turned toward +z) and "shift" (all of it 3.5 m along +x).
std::vector<MorphVertex> stretchAndShift(std::span<const Vertex> vertices, float top) {
    std::vector<MorphVertex> out(vertices.size() * 2);
    for (std::size_t v = 0; v < vertices.size(); ++v) {
        const glm::vec3 p = vertices[v].position;
        if (p.y > top - 1e-4f) out[v].position = {p.x > 0.0f ? 0.2f : -0.2f, 0.6f, 0.0f};
        out[v].normal = {0.0f, 0.3f, 0.0f};
        out[v].tangent = {0.0f, 0.0f, 0.4f};
        out[vertices.size() + v].position = {3.5f, 0.0f, 0.0f};
    }
    return out;
}

struct PixelDiff {
    int differing = 0; // pixels with a channel more than 2 apart
    int largest = 0;
};
PixelDiff pixelDiff(const std::vector<Rgba8>& a, const std::vector<Rgba8>& b) {
    PixelDiff d;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        const int m = std::max({std::abs(a[i].r - b[i].r), std::abs(a[i].g - b[i].g), std::abs(a[i].b - b[i].b)});
        d.largest = std::max(d.largest, m);
        d.differing += m > 2 ? 1 : 0;
    }
    return d;
}

bool sameImage(const std::vector<Rgba8>& a, const std::vector<Rgba8>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](Rgba8 x, Rgba8 y) { return std::memcmp(&x, &y, sizeof(Rgba8)) == 0; });
}

} // namespace

// Blend shapes on a rigid box, lit through a normal map (so the tangents matter): at weight 0 the image is the one of
// the mesh without blend shapes, bit for bit; at other weights the one of a mesh built from the morphed vertices on
// the CPU. A shape that moves the box into view counts for culling, and a change of weights moves it.
QUENCH_TEST(renderer_blend_shapes) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, plainSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    const MeshData box = boxMesh(glm::vec3(0.5f));
    const std::vector<MorphVertex> morphs = stretchAndShift(box.vertices, 0.5f);
    MeshDesc desc = box.desc();
    desc.morphs = morphs;
    desc.morphTargetCount = 2;
    Result<MeshId> morphMesh = scene.addMesh(desc);
    REQUIRE(morphMesh, "%s", morphMesh.error().c_str());
    CHECK(scene.morphTargetCount(*morphMesh) == 2 && scene.jointCount(*morphMesh) == 0, "%u targets",
          scene.morphTargetCount(*morphMesh));
    const MeshId plainMesh = addMesh(scene, box);
    std::vector<std::byte> pixels(4 * 4 * 4, std::byte{255});
    for (std::size_t i = 0; i < 16; ++i) pixels[i * 4 + 0] = std::byte{60}; // the normal map's normals toward -u
    Result<TextureId> normals = scene.addTexture({.width = 4, .height = 4, .pixels = pixels, .srgb = false});
    REQUIRE(normals, "%s", normals.error().c_str());
    MaterialDesc material{.baseColor = glm::vec4(0.8f, 0.3f, 0.1f, 1.0f), .roughness = 0.5f};
    material.textures[2] = *normals;
    const MaterialId orange = scene.addMaterial(material);
    const glm::mat4 place = glm::rotate(glm::mat4(1.0f), 0.5f, {0.0f, 1.0f, 0.0f});
    const InstanceId morphed = scene.addInstance({.mesh = *morphMesh, .material = orange, .transform = place});
    const InstanceId plain = scene.addInstance({.mesh = plainMesh, .material = orange, .transform = place});
    addSun(scene, glm::normalize(glm::vec3(-0.4f, -1.0f, -0.6f)));
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0.0f, 1.2f, 3.5f), glm::vec3(0.0f, 0.4f, 0.0f));
    InstanceId cpu{};
    const auto show = [&](InstanceId instance) {
        for (const InstanceId i : {morphed, plain, cpu})
            if (i) scene.setVisible(i, i == instance);
        return renderOnce(device, *renderer, scene, view, *target);
    };

    const std::vector<Rgba8> reference = show(plain);
    const std::vector<Rgba8> rest = show(morphed);
    CHECK(sameImage(rest, reference), "no weights: the mesh's own vertices, bit for bit (%d pixels differ)",
          pixelDiff(rest, reference).differing);
    CHECK(renderer->stats().skinnedInstances == 1, "a rigid mesh with blend shapes is deformed: %u",
          renderer->stats().skinnedInstances);

    for (const std::array<float, 2> weights :
         {std::array{1.0f, 0.0f}, std::array{0.6f, -0.15f}, std::array{1.4f, 0.1f}}) {
        scene.setMorphWeights(morphed, weights);
        const std::vector<Rgba8> gpu = show(morphed);
        const std::vector<Vertex> vertices = morphOnCpu(box.vertices, morphs, weights);
        Result<MeshId> built = scene.addMesh({vertices, box.indices, "morphed on the CPU"});
        REQUIRE(built, "%s", built.error().c_str());
        cpu = scene.addInstance({.mesh = *built, .material = orange, .transform = place});
        const std::vector<Rgba8> expected = show(cpu);
        const PixelDiff d = pixelDiff(gpu, expected);
        CHECK(d.differing <= 2 && !sameImage(gpu, reference),
              "weights %.2f %.2f: as the mesh morphed on the CPU (%d pixels more than 2 apart, at most %d)",
              double(weights[0]), double(weights[1]), d.differing, d.largest);
        scene.removeInstance(cpu);
        scene.removeMesh(*built);
        cpu = {};
    }
    scene.setMorphWeights(morphed, std::array{0.0f, 0.0f});
    CHECK(sameImage(show(morphed), reference), "weights back to 0: bit for bit again");
    show(morphed);
    CHECK(renderer->stats().skinnedReused == 1, "unchanged weights: the vertices reused (%u)",
          renderer->stats().skinnedReused);

    // Placed 3.5 m to its left, out of view; the shift brings it back, and the culling bounds follow.
    scene.setTransform(morphed, place * glm::translate(glm::mat4(1.0f), {-3.5f, 0.0f, 0.0f}), true);
    show(morphed);
    CHECK(renderer->stats().culled == 1, "out of view: %u culled", renderer->stats().culled);
    scene.setMorphWeights(morphed, std::array{0.0f, 1.0f});
    const std::vector<Rgba8> shifted = show(morphed);
    CHECK(renderer->stats().culled == 0 && pixelDiff(shifted, reference).differing <= 2,
          "shifted back into view: %u culled, %d pixels differ", renderer->stats().culled,
          pixelDiff(shifted, reference).differing);

    std::vector<MorphVertex> fewer(morphs.begin(), morphs.end() - 1);
    desc.morphs = fewer;
    Result<MeshId> bad = scene.addMesh(desc);
    CHECK(!bad && bad.error().find("morph offsets") != std::string::npos, "an offset per target and vertex: %s",
          bad ? "accepted" : bad.error().c_str());
}

// A skinned mesh: the blend shapes before the joints (in the bind pose, then turned and moved with the bar), and
// weight 0 bit for bit the skinned mesh without blend shapes, in both skinning methods.
QUENCH_TEST(renderer_blend_shapes_before_skinning) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, plainSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    const SkinnedBars bars = skinnedBars();
    const std::vector<MorphVertex> morphs = stretchAndShift(bars.mesh.vertices, 2.0f);
    MeshDesc desc = bars.desc();
    desc.morphs = morphs;
    desc.morphTargetCount = 2;
    Result<MeshId> morphMesh = scene.addMesh(desc);
    REQUIRE(morphMesh, "%s", morphMesh.error().c_str());
    Result<MeshId> plainMesh = scene.addMesh(bars.desc());
    REQUIRE(plainMesh, "%s", plainMesh.error().c_str());
    const MaterialId red = scene.addMaterial({.baseColor = glm::vec4(0.8f, 0.05f, 0.05f, 1.0f)});
    const InstanceId morphed = scene.addInstance({.mesh = *morphMesh, .material = red});
    const InstanceId plain = scene.addInstance({.mesh = *plainMesh, .material = red});
    addSun(scene, glm::vec3(-0.3f, -1.0f, -0.5f));
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0.75f, 1.0f, 6.0f), glm::vec3(0.75f, 1.0f, 0.0f));
    // Joint 1 turns the upper bar by 90 degrees about z and moves it right.
    const std::array<glm::mat4, 2> palette{glm::mat4(1.0f),
                                           glm::rotate(glm::translate(glm::mat4(1.0f), {2.5f, 1.0f, 0.0f}),
                                                       glm::radians(-90.0f), glm::vec3(0.0f, 0.0f, 1.0f)) *
                                               glm::translate(glm::mat4(1.0f), {0.0f, -1.0f, 0.0f})};
    scene.setJointPalette(morphed, palette);
    scene.setJointPalette(plain, palette);
    InstanceId cpu{};
    const auto show = [&](InstanceId instance) {
        for (const InstanceId i : {morphed, plain, cpu})
            if (i) scene.setVisible(i, i == instance);
        return renderOnce(device, *renderer, scene, view, *target);
    };
    for (const Skinning skinning : {Skinning::Linear, Skinning::DualQuaternion}) {
        const char* name = skinning == Skinning::Linear ? "linear" : "dual quaternion";
        scene.setSkinning(morphed, skinning);
        scene.setSkinning(plain, skinning);
        scene.setMorphWeights(morphed, {});
        const std::vector<Rgba8> reference = show(plain);
        CHECK(sameImage(show(morphed), reference), "%s, no weights: the skinned mesh's image, bit for bit", name);
        const std::array weights{1.0f, 0.0f};
        scene.setMorphWeights(morphed, weights);
        const std::vector<Rgba8> gpu = show(morphed);
        const std::vector<Vertex> vertices = morphOnCpu(bars.mesh.vertices, morphs, weights);
        Result<MeshId> built = scene.addMesh({vertices, bars.mesh.indices, "morphed on the CPU", bars.skin, 2});
        REQUIRE(built, "%s", built.error().c_str());
        cpu = scene.addInstance({.mesh = *built, .material = red});
        scene.setSkinning(cpu, skinning);
        scene.setJointPalette(cpu, palette);
        const std::vector<Rgba8> expected = show(cpu);
        const PixelDiff d = pixelDiff(gpu, expected);
        CHECK(d.differing <= 2 && !sameImage(gpu, reference),
              "%s: morphed in the bind pose, then skinned (%d pixels more than 2 apart, at most %d)", name, d.differing,
              d.largest);
        scene.removeInstance(cpu);
        scene.removeMesh(*built);
        cpu = {};
    }
}

// Motion vectors (TAA) follow the weights as they follow a palette: a change has motion, unchanged weights none.
QUENCH_TEST(renderer_blend_shape_motion_vectors) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.antiAliasing = AntiAliasing::Taa;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (renderer->effects().antiAliasing != AntiAliasing::Taa) QUENCH_SKIP("no TAA on this device");
    Scene scene(device);
    const MeshData box = boxMesh(glm::vec3(0.5f));
    const std::vector<MorphVertex> morphs = stretchAndShift(box.vertices, 0.5f);
    MeshDesc desc = box.desc();
    desc.morphs = morphs;
    desc.morphTargetCount = 2;
    Result<MeshId> mesh = scene.addMesh(desc);
    REQUIRE(mesh, "%s", mesh.error().c_str());
    const InstanceId instance = scene.addInstance({.mesh = *mesh, .material = scene.addMaterial({})});
    addSun(scene, glm::vec3(-0.3f, -1.0f, -0.5f));
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0.0f, 1.2f, 3.5f), glm::vec3(0.0f, 0.4f, 0.0f));
    renderOnce(device, *renderer, scene, view, *target);
    CHECK(renderer->stats().movedInstances == 0, "no weights: no motion");
    for (int frame = 0; frame < 3; ++frame) {
        scene.setMorphWeights(instance, std::array{std::array{0.3f, 0.6f, 0.9f}[std::size_t(frame)]});
        renderOnce(device, *renderer, scene, view, *target);
        CHECK(renderer->stats().movedInstances == 1, "frame %d: %u in the motion vectors", frame,
              renderer->stats().movedInstances);
    }
    scene.setMorphWeights(instance, std::array{0.9f, 0.0f});
    renderOnce(device, *renderer, scene, view, *target);
    CHECK(renderer->stats().movedInstances == 0, "unchanged weights (the second given as 0) have no motion");
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

// A rigid panel 2 m up whose blend shape moves it 2.5 m toward -z: the ray-traced shadow follows the weight (the
// panel's bottom level is built from the deformed vertices and refitted after).
QUENCH_TEST(renderer_ray_traced_blend_shape_shadows) {
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
    const std::vector<MorphVertex> away(plane.vertices.size(), MorphVertex{.position = {0.0f, 0.0f, -2.5f}});
    Result<MeshId> panel = scene.addMesh({.vertices = plane.vertices,
                                          .indices = plane.indices,
                                          .name = "panel with a blend shape",
                                          .morphs = away,
                                          .morphTargetCount = 1});
    REQUIRE(panel, "%s", panel.error().c_str());
    const InstanceId caster =
        scene.addInstance({.mesh = *panel, .material = scene.addMaterial({}), .transform = kPanelPlace});
    addSun(scene, kSunDown);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const glm::ivec2 moved = pixelOf(kTopView, {2.0f, 0, -2.5f}), rest = pixelOf(kTopView, {2.0f, 0, 0.3f}),
                     lit = pixelOf(kTopView, {-3.0f, 0, -2.5f});
    for (int frame = 0; frame < 2; ++frame) {
        if (frame == 1) scene.setMorphWeights(caster, std::array{1.0f});
        const std::vector<Rgba8> pixels = renderOnce(device, *renderer, scene, kTopView, *target);
        REQUIRE(pixels.size() == kSize * kSize, "%zu pixels", pixels.size());
        const RenderStats& stats = renderer->stats();
        CHECK(frame == 0 ? stats.rayBuilds == 2 && stats.rayRefits == 0 : stats.rayBuilds == 0 && stats.rayRefits == 1,
              "frame %d: %u built, %u refitted", frame, stats.rayBuilds, stats.rayRefits);
        const int l = sum(at(pixels, lit)), shadowed = sum(at(pixels, frame == 0 ? rest : moved)),
                  free = sum(at(pixels, frame == 0 ? moved : rest));
        std::printf("  weight %d: shadow %d, the other place %d, lit %d\n", frame, shadowed, free, l);
        CHECK(shadowed * 10 < l * 7 && std::abs(free - l) * 20 < l,
              "weight %d: the shadow where the panel is (%d), none where it is not (%d), lit %d", frame, shadowed, free,
              l);
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

// ---------------------------------------------------------------- ray-traced lighting (native Metal backend)

namespace {

// Without gpu::Caps::rayTracing the traced lighting falls back to the sky's, named in Renderer::fallbacks.
bool lightingActive(gpu::Device& device, const Renderer& renderer, const char* fallback) {
    const ActiveEffects& e = renderer.effects();
    if (e.reflections == Reflections::RayTraced || e.indirectLight != IndirectLight::Sky || e.pathTracer ||
        e.localLightShadows > 0)
        return true;
    CHECK(!device.caps().rayTracing, "ray tracing on the device, not used for %s", fallback);
    CHECK(std::ranges::any_of(renderer.fallbacks(), [&](const std::string& l) { return l.starts_with(fallback); }),
          "Renderer::fallbacks names '%s'", fallback);
    return false;
}

std::vector<Rgba8> settle(gpu::Device& device, Renderer& renderer, Scene& scene, const View& view,
                          const gpu::Texture& target, int frames) {
    std::vector<Rgba8> pixels;
    for (int frame = 0; frame < frames; ++frame) pixels = renderOnce(device, renderer, scene, view, target);
    return pixels;
}

} // namespace

// The path tracer's furnace: a white diffuse sphere under a uniform sky of radiance 1 sends back what reaches it (a
// convex shape's bounces see only the sky), so it disappears into the sky. A darker sphere means lost energy, a
// brighter one a sampling weight that adds it.
QUENCH_TEST(renderer_path_tracer_furnace) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.pathTracer = true;
    settings.pathBounces = 4;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (!lightingActive(device, *renderer, "path tracer: ")) QUENCH_SKIP("no ray tracing: no path tracer");
    CHECK(renderer->effects().antiAliasing == AntiAliasing::None, "no TAA with the path tracer");
    Scene scene(device);
    Environment e = sunOnly();
    e.skyZenith = e.skyHorizon = e.skyGround = glm::vec3(1.0f);
    e.fogColor = glm::vec3(1.0f / 0.85f); // skyRadiance's below-horizon blend
    scene.setEnvironment(e);
    const MeshId sphere = addMesh(scene, sphereMesh(1.0f));
    const InstanceId ball = scene.addInstance(
        {.mesh = sphere, .material = scene.addMaterial({.baseColor = glm::vec4(1.0f), .roughness = 1.0f})});
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0, 0, 3.5f), glm::vec3(0));
    const std::vector<Rgba8> pixels = settle(device, *renderer, scene, view, *target, 96);
    REQUIRE(pixels.size() == kSize * kSize, "%zu pixels", pixels.size());
    CHECK(renderer->stats().pathSamples == 96, "%u samples in the mean", renderer->stats().pathSamples);
    const float sky = sceneValue(at(pixels, {2, 2})).g;
    const auto mean = [&](glm::vec3 p) {
        const glm::ivec2 c = pixelOf(view, p);
        float v = 0.0f;
        for (int y = -1; y <= 1; ++y) v += sceneValue(at(pixels, c + glm::ivec2(0, y))).g / 3.0f;
        return v;
    };
    // The middle, half way out (n.v 0.76) and the pixel at the silhouette (n.v 0.37 to 0).
    const float middle = mean({0, 0, 1}), half = mean({0.5f, 0, 0.866f}), rim = mean({0.8f, 0, 0.6f});
    std::printf("  sky %.3f, sphere middle %.3f, half way %.3f, at the silhouette %.3f\n", double(sky), double(middle),
                double(half), double(rim));
    CHECK(std::abs(sky - 1.0f) < 0.03f, "the sky is 1: %.3f", double(sky));
    // The standard surface's BRDF ((1 - F) Lambert + single-scattering GGX, as in the raster path) loses a few
    // percent; toward grazing views Fresnel takes more from the diffuse term than the GGX lobe gives back.
    CHECK(middle > 0.94f && middle < 1.03f && half > 0.94f && half < 1.03f, "the sphere is the sky: %.3f, %.3f",
          double(middle), double(half));
    CHECK(rim > 0.75f && rim < 1.03f, "the silhouette: %.3f", double(rim));

    // A change of the scene starts the mean again.
    scene.setTransform(ball, glm::translate(glm::mat4(1.0f), {0.01f, 0, 0}));
    renderOnce(device, *renderer, scene, view, *target);
    CHECK(renderer->stats().pathSamples == 1, "restarted: %u samples", renderer->stats().pathSamples);

    // So does a new pose of a skinned mesh (behind the camera); the same pose set again does not.
    const MeshData plane = planeMesh(glm::vec2(0.5f));
    const std::vector<SkinVertex> skin(plane.vertices.size());
    Result<MeshId> skinned = scene.addMesh({plane.vertices, plane.indices, "skinned panel", skin, 1});
    REQUIRE(skinned, "%s", skinned.error().c_str());
    const InstanceId panel = scene.addInstance({.mesh = *skinned,
                                                .material = scene.addMaterial({.baseColor = glm::vec4(1.0f)}),
                                                .transform = glm::translate(glm::mat4(1.0f), {0, 0, 6.0f})});
    std::array<glm::mat4, 1> pose{glm::mat4(1.0f)};
    for (int frame = 0; frame < 3; ++frame) {
        scene.setJointPalette(panel, pose, frame == 0);
        renderOnce(device, *renderer, scene, view, *target);
    }
    CHECK(renderer->stats().pathSamples == 3, "the same pose kept: %u samples", renderer->stats().pathSamples);
    pose[0] = glm::translate(glm::mat4(1.0f), {0, 0.1f, 0});
    scene.setJointPalette(panel, pose);
    renderOnce(device, *renderer, scene, view, *target);
    CHECK(renderer->stats().pathSamples == 1, "a new pose restarted: %u samples", renderer->stats().pathSamples);
}

// Ray-traced reflections on a mirror floor: the image of an emissive box (its emissive texture through the hit
// texture arrays) where the sky's reflection was.
QUENCH_TEST(renderer_ray_traced_reflections) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.reflections = Reflections::RayTraced;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (!lightingActive(device, *renderer, "ray-traced reflections: ")) QUENCH_SKIP("no ray tracing: sky reflections");
    Scene scene(device);
    Environment e = sunOnly();
    e.skyZenith = e.skyHorizon = glm::vec3(0.1f, 0.2f, 0.8f);
    scene.setEnvironment(e);
    const MeshId floor = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    const MeshId box = addMesh(scene, boxMesh(glm::vec3(0.5f)));
    scene.addInstance(
        {.mesh = floor,
         .material = scene.addMaterial({.baseColor = glm::vec4(1.0f), .roughness = 0.05f, .metallic = 1.0f})});
    std::vector<std::byte> red(4 * 4 * 4);
    for (std::size_t i = 0; i < red.size(); i += 4) red[i] = red[i + 3] = std::byte{255};
    Result<TextureId> texture = scene.addTexture({.width = 4, .height = 4, .pixels = red, .mipmaps = false});
    REQUIRE(texture, "%s", texture.error().c_str());
    MaterialDesc glow{.baseColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), .emissive = glm::vec3(4.0f)};
    glow.textures[3] = *texture;
    scene.addInstance({.mesh = box,
                       .material = scene.addMaterial(glow),
                       .transform = glm::translate(glm::mat4(1.0f), {0, 1.0f, -2.0f})});
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0, 1.0f, 3.0f), glm::vec3(0, 0, -1.0f));
    // The mirror image of the box's middle, and floor that reflects the sky.
    const glm::ivec2 image = pixelOf(view, {0, -1.0f, -2.0f}), open = pixelOf(view, {2.5f, -1.0f, -2.0f});
    const std::vector<Rgba8> traced = settle(device, *renderer, scene, view, *target, 8);
    REQUIRE(traced.size() == kSize * kSize, "%zu pixels", traced.size());
    CHECK(renderer->stats().hitTextures == 1, "%u textures in the hit arrays", renderer->stats().hitTextures);
    settings.reflections = Reflections::Sky;
    renderer->setSettings(settings);
    const std::vector<Rgba8> sky = settle(device, *renderer, scene, view, *target, 2);
    const Rgba8 t = at(traced, image), s = at(sky, image), o = at(traced, open);
    std::printf("  mirror image %d %d %d (sky reflections %d %d %d), open floor %d %d %d\n", t.r, t.g, t.b, s.r, s.g,
                s.b, o.r, o.g, o.b);
    CHECK(t.r > 150 && t.r > t.b * 2, "the box's red in the mirror: %d %d %d", t.r, t.g, t.b);
    CHECK(s.b > s.r, "without traced reflections the sky's: %d %d %d", s.r, s.g, s.b);
    CHECK(o.b > o.r && std::abs(sum(o) - sum(at(sky, open))) < 24, "the open floor reflects the sky in both: %d vs %d",
          sum(o), sum(at(sky, open)));
}

// Normal maps in ray hits and in the path tracer: two walls under a sun from the upper right, the right one with a
// normal map that turns its normal toward +u and +v (the light), over a mirror floor. The mapped wall's gain over the
// plain one is the raster's on the walls, in their mirror images (traced reflections) and in the path tracer. A wrong
// tangent or bitangent sign in the hits turns the normal away from the light instead.
QUENCH_TEST(renderer_ray_hits_read_normal_maps) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.reflections = Reflections::RayTraced;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (!lightingActive(device, *renderer, "ray-traced reflections: ")) QUENCH_SKIP("no ray tracing: sky reflections");
    Scene scene(device);
    scene.setEnvironment(sunOnly());
    addSun(scene, -glm::normalize(glm::vec3(1.0f, 1.0f, 0.3f)));
    const InstanceId floor = scene.addInstance(
        {.mesh = addMesh(scene, planeMesh(glm::vec2(10.0f))),
         .material = scene.addMaterial({.baseColor = glm::vec4(1.0f), .roughness = 0.05f, .metallic = 1.0f})});
    // (0.5, 0.5, 0.707) in the tangent frame; the plane's +u is +x, its -v (the map's +y) +y once stood up.
    std::vector<std::byte> tilted(4 * 4 * 4);
    for (std::size_t i = 0; i < tilted.size(); i += 4) {
        tilted[i] = tilted[i + 1] = std::byte{191};
        tilted[i + 2] = std::byte{218};
        tilted[i + 3] = std::byte{255};
    }
    Result<TextureId> normalMap =
        scene.addTexture({.width = 4, .height = 4, .pixels = tilted, .srgb = false, .mipmaps = false});
    REQUIRE(normalMap, "%s", normalMap.error().c_str());
    MaterialDesc wall{.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), .roughness = 1.0f};
    const MeshId panel = addMesh(scene, planeMesh(glm::vec2(0.5f)));
    const glm::mat4 standUp = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1, 0, 0));
    scene.addInstance({.mesh = panel,
                       .material = scene.addMaterial(wall),
                       .transform = glm::translate(glm::mat4(1.0f), {-0.7f, 1.2f, -2.0f}) * standUp});
    wall.textures[2] = *normalMap;
    scene.addInstance({.mesh = panel,
                       .material = scene.addMaterial(wall),
                       .transform = glm::translate(glm::mat4(1.0f), {0.7f, 1.2f, -2.0f}) * standUp});
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0, 1.0f, 3.0f), glm::vec3(0, 0.3f, -2.0f));
    const auto mean = [&](const std::vector<Rgba8>& pixels, glm::vec3 p) {
        const glm::ivec2 c = pixelOf(view, p);
        float v = 0.0f;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) v += sceneValue(at(pixels, c + glm::ivec2(x, y))).g / 9.0f;
        return v;
    };
    const auto gain = [&](const std::vector<Rgba8>& pixels, float y) {
        return mean(pixels, {0.7f, y, -2.0f}) / std::max(mean(pixels, {-0.7f, y, -2.0f}), 1e-3f);
    };
    const std::vector<Rgba8> traced = settle(device, *renderer, scene, view, *target, 8);
    REQUIRE(traced.size() == kSize * kSize, "%zu pixels", traced.size());
    CHECK(renderer->stats().hitTextures == 1, "%u textures in the hit arrays", renderer->stats().hitTextures);
    const float walls = gain(traced, 1.2f), mirrored = gain(traced, -1.2f);

    scene.removeInstance(floor);
    settings.pathTracer = true;
    renderer->setSettings(settings);
    const std::vector<Rgba8> path = settle(device, *renderer, scene, view, *target, 16);
    const float pathWalls = gain(path, 1.2f);
    std::printf("  mapped over plain: raster walls %.2f, their mirror images %.2f, path tracer %.2f\n", double(walls),
                double(mirrored), double(pathWalls));
    // n.l 0.84 against 0.21 for the plain wall's.
    CHECK(walls > 3.0f && walls < 5.0f, "the raster's normal map: %.2f", double(walls));
    CHECK(std::abs(mirrored / walls - 1.0f) < 0.15f, "the mirror images: %.2f against %.2f", double(mirrored),
          double(walls));
    CHECK(std::abs(pathWalls / walls - 1.0f) < 0.15f, "the path tracer: %.2f against %.2f", double(pathWalls),
          double(walls));
}

// A point light beside a box: with ray-traced local light shadows the box shadows the ground behind it.
QUENCH_TEST(renderer_ray_traced_local_light_shadows) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.localLightShadows = 1;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (!lightingActive(device, *renderer, "ray-traced local light shadows: "))
        QUENCH_SKIP("no ray tracing: unshadowed local lights");
    Scene scene(device);
    scene.setEnvironment(sunOnly());
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    const MeshId box = addMesh(scene, boxMesh(glm::vec3(0.4f)));
    const MaterialId grey = scene.addMaterial({.baseColor = glm::vec4(0.6f, 0.6f, 0.6f, 1.0f), .roughness = 0.9f});
    scene.addInstance({.mesh = ground, .material = grey});
    scene.addInstance({.mesh = box, .material = grey, .transform = glm::translate(glm::mat4(1.0f), {-0.75f, 1.5f, 0})});
    scene.addLight({.type = LightType::Point,
                    .color = glm::vec3(1.0f),
                    .intensity = 40.0f,
                    .position = {-1.5f, 3.0f, 0.0f},
                    .range = 12.0f});
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    // The light, the box and (0, 0, 0) are on a line; (-3, 0, 0), mirrored under the light, is in the open.
    const glm::ivec2 shadow = pixelOf(kTopView, {0.0f, 0, 0}), lit = pixelOf(kTopView, {-3.0f, 0, 0});
    const std::vector<Rgba8> traced = settle(device, *renderer, scene, kTopView, *target, 4);
    REQUIRE(traced.size() == kSize * kSize, "%zu pixels", traced.size());
    CHECK(renderer->stats().localShadowChanges == 0, "the light kept its channel: %u changes",
          renderer->stats().localShadowChanges);
    settings.localLightShadows = 0;
    renderer->setSettings(settings);
    const std::vector<Rgba8> open = settle(device, *renderer, scene, kTopView, *target, 2);
    settings.localLightShadows = 1;
    renderer->setSettings(settings);
    (void)renderOnce(device, *renderer, scene, kTopView, *target);
    CHECK(renderer->stats().localShadowChanges == 1, "the light took a free channel: %u changes",
          renderer->stats().localShadowChanges);
    std::printf("  behind the box %d (unshadowed %d), in the open %d (unshadowed %d)\n", sum(at(traced, shadow)),
                sum(at(open, shadow)), sum(at(traced, lit)), sum(at(open, lit)));
    CHECK(sum(at(traced, shadow)) * 4 < sum(at(traced, lit)), "shadowed behind the box: %d vs %d",
          sum(at(traced, shadow)), sum(at(traced, lit)));
    CHECK(std::abs(sum(at(open, shadow)) - sum(at(open, lit))) * 10 < sum(at(open, lit)),
          "unshadowed both are lit alike: %d vs %d", sum(at(open, shadow)), sum(at(open, lit)));
    CHECK(std::abs(sum(at(traced, lit)) - sum(at(open, lit))) * 20 < sum(at(open, lit)),
          "the open ground is as lit with shadows: %d vs %d", sum(at(traced, lit)), sum(at(open, lit)));
}

// Local light shadows without ray tracing: a point light low over the ground on the left (its +x, -y and -z maps each
// have a caster) and a tilted spot light on the right (one map, the second light's layers). Where rays run, the same
// shadows are traced, and the checks hold for them too.
QUENCH_TEST(renderer_local_light_shadow_maps) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.localLightShadows = 2;
    settings.localShadowMapSize = 512;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    const bool maps = renderer->effects().localShadowMaps > 0;
    std::printf("  %s\n", maps ? "shadow maps" : "ray traced");
    Scene scene(device);
    scene.setEnvironment(sunOnly());
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(12.0f)));
    const MeshId box = addMesh(scene, boxMesh(glm::vec3(0.15f)));
    const MeshId plate = addMesh(scene, boxMesh(glm::vec3(0.15f, 0.01f, 0.15f)));
    const MaterialId grey = scene.addMaterial({.baseColor = glm::vec4(0.6f, 0.6f, 0.6f, 1.0f), .roughness = 0.9f});
    scene.addInstance({.mesh = ground, .material = grey});
    const auto place = [&](MeshId mesh, glm::vec3 p) {
        scene.addInstance({.mesh = mesh, .material = grey, .transform = glm::translate(glm::mat4(1.0f), p)});
    };
    const glm::vec3 pointAt(-2.5f, 1.0f, 0.0f), spotAt(2.0f, 3.0f, 0.0f);
    const glm::vec3 spotDir = glm::normalize(glm::vec3(0.25f, -1.0f, 0.0f));
    // Each caster halfway between its light and the ground point it shadows.
    const glm::vec3 plusX(-1.0f, 0, 0), minusY(-2.5f, 0, 0.4f), minusZ(-2.5f, 0, -1.5f), spotted(2.75f, 0, 0.8f);
    place(box, 0.5f * (pointAt + plusX));
    place(plate, {-2.5f, 0.75f, 0.0f});
    place(box, 0.5f * (pointAt + minusZ));
    place(box, 0.5f * (spotAt + spotted));
    scene.addLight(
        {.type = LightType::Point, .color = glm::vec3(1.0f), .intensity = 10.0f, .position = pointAt, .range = 3.5f});
    scene.addLight({.type = LightType::Spot,
                    .color = glm::vec3(1.0f),
                    .intensity = 20.0f,
                    .position = spotAt,
                    .direction = spotDir,
                    .range = 5.0f,
                    .innerAngle = 0.35f,
                    .outerAngle = 0.45f});
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());

    // The traced shadows settle over the temporal denoiser's history; the maps are there in the first frame.
    const std::vector<Rgba8> shadowed = settle(device, *renderer, scene, kTopView, *target, maps ? 1 : 8);
    REQUIRE(shadowed.size() == kSize * kSize, "%zu pixels", shadowed.size());
    const RenderStats stats = renderer->stats();
    if (maps) {
        CHECK(stats.localShadowLights == 2 && stats.localShadowMaps == 7, "2 lights, 7 maps: %u, %u",
              stats.localShadowLights, stats.localShadowMaps);
        CHECK(stats.localShadowInstances >= 4 && stats.localShadowDraws > 0, "casters drawn: %u instances, %u draws",
              stats.localShadowInstances, stats.localShadowDraws);
    }
    settings.localLightShadows = 0;
    renderer->setSettings(settings);
    const std::vector<Rgba8> open = settle(device, *renderer, scene, kTopView, *target, 2);
    CHECK(renderer->stats().localShadowMaps == 0, "no maps without local light shadows: %u",
          renderer->stats().localShadowMaps);

    const auto value = [](const std::vector<Rgba8>& pixels, glm::vec3 p) {
        return sum(at(pixels, pixelOf(kTopView, p)));
    };
    struct Spot {
        const char* name;
        glm::vec3 shadow, mirror; // the shadowed point and one lit alike without the caster
    };
    const Spot spots[] = {{"point +x", plusX, {-4.0f, 0, 0}},
                          {"point -y", minusY, minusY},
                          {"point -z", minusZ, {-2.5f, 0, 1.5f}},
                          {"spot", spotted, {2.75f, 0, -0.8f}}};
    for (const Spot& s : spots) {
        const int dark = value(shadowed, s.shadow), unshadowed = value(open, s.shadow);
        const int lit = value(shadowed, s.mirror), litOpen = value(open, s.mirror);
        std::printf("  %s: shadow %d (unshadowed %d)", s.name, dark, unshadowed);
        if (s.mirror != s.shadow) std::printf(", lit %d (unshadowed %d)", lit, litOpen);
        std::printf("\n");
        // The traced shadows of these small casters are blurred by the denoiser at this size: half as dark.
        CHECK(dark * (maps ? 4 : 2) < unshadowed, "%s: shadowed %d vs unshadowed %d", s.name, dark, unshadowed);
        if (s.mirror != s.shadow) {
            CHECK(std::abs(unshadowed - litOpen) * 10 < litOpen, "%s: lit alike without shadows: %d vs %d", s.name,
                  unshadowed, litOpen);
            CHECK(std::abs(lit - litOpen) * 20 < litOpen, "%s: the open ground as lit with shadows: %d vs %d", s.name,
                  lit, litOpen);
        }
    }
    if (!maps) return;
    // The open ground around the lights, near and far from them, on the edge of two of the point light's maps and in
    // the corner of three: no acne, no seams.
    int worst = 0;
    for (const glm::vec3 p : {glm::vec3(-3.5f, 0, -0.5f), glm::vec3(-4.2f, 0, 1.0f), glm::vec3(-1.5f, 0, 1.2f),
                              glm::vec3(-0.6f, 0, -1.0f), glm::vec3(-1.5f, 0, 0.9f), glm::vec3(-3.5f, 0, -1.0f),
                              glm::vec3(2.0f, 0, -0.2f), glm::vec3(3.6f, 0, -0.6f), glm::vec3(3.0f, 0, 1.3f)}) {
        const int a = value(shadowed, p), b = value(open, p);
        worst = std::max(worst, std::abs(a - b));
        CHECK(std::abs(a - b) * 20 <= b + 20, "open ground at (%.1f, %.1f): %d vs %d unshadowed", p.x, p.z, a, b);
    }
    std::printf("  open ground: largest difference %d\n", worst);

    // One light with shadows: the nearest by the distance from the camera to its range, here the spot light.
    settings.localLightShadows = 1;
    renderer->setSettings(settings);
    (void)renderOnce(device, *renderer, scene, kTopView, *target);
    CHECK(renderer->stats().localShadowLights == 1 && renderer->stats().localShadowMaps == 1,
          "the spot light's map only: %u lights, %u maps", renderer->stats().localShadowLights,
          renderer->stats().localShadowMaps);
    // Without a map size the lights are unshadowed, and the fallback says so.
    settings.localLightShadows = 2;
    settings.localShadowMapSize = 0;
    renderer->setSettings(settings);
    const std::vector<Rgba8> off = renderOnce(device, *renderer, scene, kTopView, *target);
    CHECK(renderer->stats().localShadowMaps == 0 && renderer->effects().localShadowMaps == 0, "no maps at size 0");
    CHECK(value(off, plusX) == value(open, plusX), "unshadowed at size 0: %d vs %d", value(off, plusX),
          value(open, plusX));
    bool named = false;
    for (const std::string& f : renderer->fallbacks()) named = named || f.ends_with("; unshadowed");
    CHECK(named, "the fallback names the unshadowed lights");
}

// Indirect light: the sun lights a red wall and the ground; the ground in front of the wall turns red with the probe
// grid and with traced indirect light, not with the sky's (zero) ambient light.
QUENCH_TEST(renderer_indirect_light_bounces) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.indirectLight = IndirectLight::Probes;
    settings.probeSpacing = 1.0f;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (!lightingActive(device, *renderer, "the probe grid: ")) QUENCH_SKIP("no ray tracing: the sky's ambient light");
    Scene scene(device);
    scene.setEnvironment(sunOnly());
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    const MeshId wall = addMesh(scene, boxMesh(glm::vec3(0.1f, 2.0f, 4.0f)));
    scene.addInstance(
        {.mesh = ground, .material = scene.addMaterial({.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1), .roughness = 1})});
    scene.addInstance({.mesh = wall,
                       .material = scene.addMaterial({.baseColor = glm::vec4(0.9f, 0.05f, 0.05f, 1), .roughness = 1}),
                       .transform = glm::translate(glm::mat4(1.0f), {1.5f, 2.0f, 0})});
    // From the upper left: the wall's face toward -x and the ground in front of it are lit.
    addSun(scene, glm::normalize(glm::vec3(1.0f, -1.2f, 0.3f)));
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    // The top view is 9 m up, above the 8 probes of the grid's height: it centers the grid on the ground.
    View view = kTopView;
    view.probeCenter = glm::vec3(0.0f, 2.0f, 0.0f);
    const glm::ivec2 near = pixelOf(view, {1.0f, 0, 0}), far = pixelOf(view, {-3.5f, 0, 0});
    const auto redness = [](Rgba8 c) {
        const glm::vec3 v = sceneValue(c);
        return v.r / std::max(v.g, 1e-3f);
    };
    const std::vector<Rgba8> grid = settle(device, *renderer, scene, view, *target, 6);
    REQUIRE(grid.size() == kSize * kSize, "%zu pixels", grid.size());
    CHECK(renderer->stats().probes == 16 * 8 * 16, "%u probes", renderer->stats().probes);
    settings.indirectLight = IndirectLight::RayTraced;
    renderer->setSettings(settings);
    const std::vector<Rgba8> traced = settle(device, *renderer, scene, view, *target, 16);
    settings.indirectLight = IndirectLight::Sky;
    renderer->setSettings(settings);
    const std::vector<Rgba8> sky = settle(device, *renderer, scene, view, *target, 2);
    std::printf("  red / green next to the wall: probes %.3f, traced %.3f, sky %.3f; far from it %.3f %.3f %.3f\n",
                double(redness(at(grid, near))), double(redness(at(traced, near))), double(redness(at(sky, near))),
                double(redness(at(grid, far))), double(redness(at(traced, far))), double(redness(at(sky, far))));
    CHECK(std::abs(redness(at(sky, near)) - 1.0f) < 0.02f, "no bounce with the sky's light: %.3f",
          double(redness(at(sky, near))));
    CHECK(redness(at(grid, near)) > 1.05f && redness(at(traced, near)) > 1.05f,
          "red bounce light next to the wall: probes %.3f, traced %.3f", double(redness(at(grid, near))),
          double(redness(at(traced, near))));
    CHECK(redness(at(grid, near)) > redness(at(grid, far)) && redness(at(traced, near)) > redness(at(traced, far)),
          "more of it near the wall than far: %.3f > %.3f, %.3f > %.3f", double(redness(at(grid, near))),
          double(redness(at(grid, far))), double(redness(at(traced, near))), double(redness(at(traced, far))));
}

// RenderSettings::probeUpdatePeriod: the grid traces 1 / n of its probes per frame. A white tile on a black ground,
// in a black roof's shadow, is lit by the grid only, from a sunlit white wall beside it (little light bounces back, so
// the grid settles within the hysteresis). It holds its light while nothing changes, and after the sun doubles or
// halves it takes about n times as many frames to get halfway to its new light. Probes that come into a grid that
// scrolls by a cell are left out until their turn, not taken as black; a grid that jumps traces every probe.
QUENCH_TEST(renderer_probe_grid_updates_in_turns) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = shadowSettings();
    settings.indirectLight = IndirectLight::Probes;
    settings.probeSpacing = 1.0f;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (!lightingActive(device, *renderer, "the probe grid: ")) QUENCH_SKIP("no ray tracing: the sky's ambient light");
    Scene scene(device);
    scene.setEnvironment(sunOnly());
    const MaterialId white = scene.addMaterial({.baseColor = glm::vec4(0.9f, 0.9f, 0.9f, 1), .roughness = 1});
    const MaterialId black = scene.addMaterial({.baseColor = glm::vec4(0.0f, 0.0f, 0.0f, 1), .roughness = 1});
    const auto place = [&](glm::vec3 half, glm::vec3 at, MaterialId material) {
        scene.addInstance({.mesh = addMesh(scene, boxMesh(half)),
                           .material = material,
                           .transform = glm::translate(glm::mat4(1.0f), at)});
    };
    scene.addInstance({.mesh = addMesh(scene, planeMesh(glm::vec2(20.0f))), .material = black});
    place({0.5f, 0.01f, 1.5f}, {2.1f, 0.01f, 0}, white); // the tile, x 1.6..2.6
    place({0.1f, 1.5f, 3.0f}, {3.0f, 1.5f, 0}, white);   // the wall, its sunlit face at x 2.9
    place({0.6f, 0.05f, 1.6f}, {1.1f, 1.0f, 0}, black);  // the roof: its shadow covers x 1.5..2.7
    const LightId sun = addSun(scene, kSunDown);
    const auto setSun = [&](float intensity) {
        scene.setLight(sun, {.type = LightType::Directional, .color = glm::vec3(intensity), .direction = kSunDown});
    };
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    View view = kTopView;
    view.probeCenter = glm::vec3(0.0f, 2.0f, 0.0f);
    // The strip of the tile x 2.0..2.4, z -1.2..1.2 (the camera sees it past the roof): several probes' worth, so
    // less of their noise.
    const glm::ivec2 from = pixelOf(view, {2.0f, 0, -1.2f}), to = pixelOf(view, {2.4f, 0, 1.2f});
    const auto light = [&](int frames) {
        const std::vector<Rgba8> pixels = settle(device, *renderer, scene, view, *target, frames);
        float v = 0.0f;
        int count = 0;
        for (int y = std::min(from.y, to.y); y <= std::max(from.y, to.y); ++y)
            for (int x = std::min(from.x, to.x); x <= std::max(from.x, to.x); ++x, ++count)
                v += sceneValue(at(pixels, {x, y})).g;
        return v / float(std::max(count, 1));
    };
    const auto setTurns = [&](int n) {
        settings.probeUpdatePeriod = n;
        renderer->setSettings(settings);
    };
    setSun(5.0f);
    // Without the grid, what the strip has of the sun past the shadow's filtered edge.
    settings.indirectLight = IndirectLight::Sky;
    renderer->setSettings(settings);
    const float direct = light(1);
    settings.indirectLight = IndirectLight::Probes;
    renderer->setSettings(settings);
    const float low = light(150);
    REQUIRE(low > 0.1f && direct < 0.2f * low, "the shadowed strip has bounce light: %.4f (%.4f without the grid)",
            double(low), double(direct));
    for (int n = 2; n <= 8; n *= 2) {
        setTurns(n);
        const float steady = light(2 * n);
        CHECK(renderer->stats().probes == 16u * 8u * 16u / unsigned(n), "%d turns: %u probes traced", n,
              renderer->stats().probes);
        CHECK(std::abs(steady / low - 1.0f) < 0.05f, "%d turns: the light holds: %.4f vs %.4f", n, double(steady),
              double(low));
    }
    // Light follows the sun: it settles at twice `low` with twice the sun, and halfway is 1.5 times `low`. The sun
    // doubles and halves in turn; the grid settles with every probe traced before each change.
    int halfway[4] = {};
    float high = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const int n = 1 << i;
        const bool up = i % 2 == 0;
        setTurns(n);
        setSun(up ? 10.0f : 5.0f);
        int frames = 1;
        for (; frames < 100 * n; ++frames) {
            const float v = light(1);
            if (up ? v >= 1.5f * low : v <= 1.5f * low) break;
        }
        halfway[i] = frames;
        setTurns(1);
        const float settled = light(150);
        if (i == 0) high = settled;
    }
    std::printf("  bounce light %.4f, %.4f with twice the sun; frames to halfway with 1, 2, 4, 8 turns: %d %d %d %d\n",
                double(low), double(high), halfway[0], halfway[1], halfway[2], halfway[3]);
    CHECK(std::abs(high / low - 2.0f) < 0.1f, "twice the sun, twice the bounce light: %.4f, %.4f", double(low),
          double(high));
    for (int i = 1; i < 4; ++i) {
        const float expected = float(halfway[0] << i);
        CHECK(float(halfway[i]) > 0.75f * expected - 1.0f && float(halfway[i]) < 1.25f * expected + float(1 << i),
              "%d turns: %d frames to halfway, about %d times %d", 1 << i, halfway[i], 1 << i, halfway[0]);
    }

    // The grid ends at x 1.5 (its last probes before the strip), then scrolls a cell: the probes at x 2.5 come in,
    // one in 8 of them traced in that frame. Taken as black, the others would darken the strip in the frames whose
    // turn has none of its new probes; left out, the strip keeps its light or gets more from the traced ones nearer
    // the wall. The grid scrolls back and forth so that the scroll comes in every turn.
    view.probeCenter = glm::vec3(-6.0f, 2.0f, 0.0f);
    const float edge = light(150);
    setTurns(8);
    float darkest = edge * 10.0f;
    for (int turn = 0; turn < 8; ++turn) {
        view.probeCenter = glm::vec3(-6.0f, 2.0f, 0.0f);
        light(2);
        view.probeCenter = glm::vec3(-5.0f, 2.0f, 0.0f);
        darkest = std::min(darkest, light(1));
        CHECK(renderer->stats().probes == 256u, "a scroll by a cell: %u probes traced", renderer->stats().probes);
    }
    const float later = light(8);
    std::printf("  a scroll by a cell with 8 turns: %.4f before, at least %.4f in the frame of the scroll, %.4f once "
                "the new probes were traced\n",
                double(edge), double(darkest), double(later));
    CHECK(edge > 0.2f * low && darkest > 0.9f * edge && later > edge,
          "the new probes left out until traced: at least %.4f, %.4f before and %.4f after", double(darkest),
          double(edge), double(later));

    // The grid jumps away (the strip outside it: no bounce light) and back: every probe is traced.
    view.probeCenter = glm::vec3(-14.0f, 2.0f, 0.0f);
    const float away = light(8);
    CHECK(renderer->stats().probes == 256u, "away: %u probes traced", renderer->stats().probes);
    view.probeCenter = glm::vec3(0.0f, 2.0f, 0.0f);
    const float back = light(1);
    CHECK(renderer->stats().probes == 16u * 8u * 16u, "a jump: %u probes traced", renderer->stats().probes);
    std::printf("  a jump away and back with 8 turns: %.4f, %.4f\n", double(away), double(back));
    CHECK(std::abs(away - direct) < 0.02f * low, "away: no bounce light (%.4f, %.4f without the grid)", double(away),
          double(direct));
    CHECK(std::abs(back / low - 1.0f) < 0.3f, "back: lit at once, %.4f vs %.4f", double(back), double(low));
}

// The temporal denoiser: a soft sun shadow traced with one ray per pixel, settled over frames, is closer to the same
// shadow traced with eight rays than the spatial filter's, and steadier from frame to frame.
QUENCH_TEST(renderer_temporal_ray_denoiser) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = shadowSettings();
    settings.sunShadows = SunShadows::RayTraced;
    settings.shadowSoftness = 6.0f;
    settings.shadowRays = 1;
    settings.ambientOcclusion = AmbientOcclusion::RayTraced;
    settings.occlusionRays = 1;
    settings.rayDenoiser = RayDenoiser::Temporal;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (!rayTracingActive(device, *renderer)) QUENCH_SKIP("no ray tracing: shadow maps");
    CHECK(renderer->effects().rayDenoiser == RayDenoiser::Temporal, "the temporal denoiser runs");
    Scene scene(device);
    Environment e = sunOnly();
    e.skyAmbient = e.groundAmbient = glm::vec3(0.3f);
    scene.setEnvironment(e);
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    const MeshId box = addMesh(scene, boxMesh(glm::vec3(0.5f)));
    const MaterialId grey = scene.addMaterial({.baseColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f), .roughness = 0.9f});
    scene.addInstance({.mesh = ground, .material = grey});
    scene.addInstance({.mesh = box, .material = grey, .transform = glm::translate(glm::mat4(1.0f), {0, 1.5f, 0})});
    addSun(scene, kSunDown);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const auto run = [&](RayDenoiser denoiser, int rays, std::vector<Rgba8>* previous) {
        RenderSettings s = settings;
        s.rayDenoiser = denoiser;
        s.shadowRays = s.occlusionRays = rays;
        renderer->setSettings(s);
        std::vector<Rgba8> pixels = settle(device, *renderer, scene, kTopView, *target, 24);
        if (previous) *previous = pixels;
        return settle(device, *renderer, scene, kTopView, *target, 1);
    };
    std::vector<Rgba8> temporalBefore, spatialBefore;
    const std::vector<Rgba8> reference = run(RayDenoiser::Temporal, 8, nullptr);
    const std::vector<Rgba8> temporal = run(RayDenoiser::Temporal, 1, &temporalBefore);
    const std::vector<Rgba8> spatial = run(RayDenoiser::Spatial, 1, &spatialBefore);
    const ImageDiff t = compare(temporal, reference), s = compare(spatial, reference);
    const ImageDiff tf = compare(temporal, temporalBefore), sf = compare(spatial, spatialBefore);
    std::printf("  against 8 rays: temporal mean %.2f (%.2f%% over 16), spatial %.2f (%.2f%%); frame to frame: "
                "temporal %.2f, spatial %.2f\n",
                t.mean, t.over16 * 100.0, s.mean, s.over16 * 100.0, tf.mean, sf.mean);
    CHECK(t.mean < s.mean, "the temporal denoiser is closer to the reference: %.2f vs %.2f", t.mean, s.mean);
    CHECK(tf.mean < sf.mean * 0.75, "and steadier: %.2f vs %.2f", tf.mean, sf.mean);
}

// The acceleration structures over frames: rigid meshes are built once and replaced by compacted copies, the top
// level is refitted while only transforms change, and a refitted top level traces what a fresh build does.
QUENCH_TEST(renderer_ray_tracing_refit_and_compaction) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = shadowSettings();
    settings.sunShadows = SunShadows::RayTraced;
    settings.shadowSoftness = 0.0f;
    settings.rayDenoiser = RayDenoiser::Spatial; // hard shadows: the mask as traced, no history
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (!rayTracingActive(device, *renderer)) QUENCH_SKIP("no ray tracing: shadow maps");
    Scene scene(device);
    scene.setEnvironment(sunOnly());
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    const MeshId box = addMesh(scene, boxMesh(glm::vec3(0.5f)));
    const MaterialId grey = scene.addMaterial({.baseColor = glm::vec4(0.6f, 0.6f, 0.6f, 1.0f), .roughness = 0.9f});
    scene.addInstance({.mesh = ground, .material = grey});
    scene.addInstance({.mesh = box, .material = grey, .transform = glm::translate(glm::mat4(1.0f), {-1.5f, 1.0f, 0})});
    const InstanceId moving = scene.addInstance(
        {.mesh = box, .material = grey, .transform = glm::translate(glm::mat4(1.0f), {1.5f, 1.0f, 0})});
    addSun(scene, kSunDown);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());

    renderOnce(device, *renderer, scene, kTopView, *target);
    const RenderStats first = renderer->stats();
    renderOnce(device, *renderer, scene, kTopView, *target); // the first frame's command buffer has completed
    const RenderStats second = renderer->stats();
    renderOnce(device, *renderer, scene, kTopView, *target);
    const RenderStats third = renderer->stats();
    scene.setTransform(moving, glm::translate(glm::mat4(1.0f), {1.5f, 1.0f, 2.0f}));
    const std::vector<Rgba8> refitted = renderOnce(device, *renderer, scene, kTopView, *target);
    const RenderStats moved = renderer->stats();
    std::printf("  frame 1: %u builds, %llu bytes; frame 2: %u compactions, %llu bytes; frame 3: top refit %d; "
                "moved: top refit %d\n",
                first.rayBuilds, static_cast<unsigned long long>(first.rayMemoryBytes), second.rayCompactions,
                static_cast<unsigned long long>(second.rayMemoryBytes), int(third.rayTopRefit), int(moved.rayTopRefit));
    CHECK(first.rayBuilds == 2 && !first.rayTopRefit, "two bottom levels and the top level built: %u", first.rayBuilds);
    CHECK(second.rayCompactions == 2 && second.rayBuilds == 0 && !second.rayTopRefit,
          "both compacted, the top level built over the copies: %u compactions", second.rayCompactions);
    CHECK(second.rayMemoryBytes < first.rayMemoryBytes, "less memory after compaction: %llu vs %llu",
          static_cast<unsigned long long>(second.rayMemoryBytes),
          static_cast<unsigned long long>(first.rayMemoryBytes));
    CHECK(third.rayTopRefit && moved.rayTopRefit && third.rayCompactions == 0, "refits while only transforms change");

    // A fresh renderer builds its top level over the same (compacted) bottom levels.
    Result<Renderer> fresh = Renderer::create(device, settings);
    REQUIRE(fresh, "%s", fresh.error().c_str());
    const std::vector<Rgba8> built = renderOnce(device, *fresh, scene, kTopView, *target);
    CHECK(!fresh->stats().rayTopRefit, "built");
    const ImageDiff d = compare(refitted, built);
    std::printf("  refitted against built: mean %.3f, %.2f%% over 16\n", d.mean, d.over16 * 100.0);
    CHECK(d.mean < 0.05 && d.over16 == 0.0, "the same shadows: mean %.3f", d.mean);
    // The 1 m box between heights 0.5 and 1.5 shadows x 1.5 to 3.5 under the 45 degree sun (x 2 to 3.5 beside it), z
    // +-0.5 around the box: about 7 pixels across.
    const glm::ivec2 shadow = pixelOf(kTopView, {2.5f, 0, 2.0f}), old = pixelOf(kTopView, {2.5f, 0, 0});
    CHECK(sum(at(refitted, shadow)) * 4 < sum(at(refitted, old)), "the shadow moved with the box: %d vs %d",
          sum(at(refitted, shadow)), sum(at(refitted, old)));

    // The temporal denoiser's history does not trail the moved shadow (its anti-lag), also where the shadow is a few
    // pixels thin and the history clamp alone lets it fade in over the history's length.
    settings.rayDenoiser = RayDenoiser::Temporal;
    renderer->setSettings(settings);
    settle(device, *renderer, scene, kTopView, *target, 8);
    scene.setTransform(moving, glm::translate(glm::mat4(1.0f), {1.5f, 1.0f, 0.0f}));
    const std::vector<Rgba8> after = renderOnce(device, *renderer, scene, kTopView, *target);
    const std::vector<Rgba8> settled = settle(device, *renderer, scene, kTopView, *target, 16);
    std::printf("  temporal denoiser, the frame after a move back: new shadow %d (settled %d), where it was %d "
                "(settled %d)\n",
                sum(at(after, old)), sum(at(settled, old)), sum(at(after, shadow)), sum(at(settled, shadow)));
    CHECK(sum(at(after, old)) < sum(at(settled, old)) + 40, "the new shadow at once: %d, settled %d",
          sum(at(after, old)), sum(at(settled, old)));
    CHECK(sum(at(after, shadow)) + 20 > sum(at(settled, shadow)), "lit at once where it was: %d, settled %d",
          sum(at(after, shadow)), sum(at(settled, shadow)));
}

// The indirect light's furnace: under a uniform sky of radiance 1 whose ambient values are 1 too, the probe grid and
// the per-pixel rays light an open floor and a sphere on it as the sky's ambient light does (the grid's light has
// the scale of Environment::skyAmbient); the sphere a little less, as it also sees the darker floor.
QUENCH_TEST(renderer_indirect_light_furnace) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.indirectLight = IndirectLight::Probes;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (!lightingActive(device, *renderer, "the probe grid: ")) QUENCH_SKIP("no ray tracing: the sky's ambient light");
    Scene scene(device);
    Environment e = sunOnly();
    e.skyZenith = e.skyHorizon = e.skyGround = glm::vec3(1.0f);
    e.fogColor = glm::vec3(1.0f / 0.85f); // skyRadiance's below-horizon blend
    e.skyAmbient = e.groundAmbient = glm::vec3(1.0f);
    scene.setEnvironment(e);
    const MaterialId white = scene.addMaterial({.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), .roughness = 1.0f});
    scene.addInstance({.mesh = addMesh(scene, planeMesh(glm::vec2(20.0f))), .material = white});
    scene.addInstance({.mesh = addMesh(scene, sphereMesh(1.0f)),
                       .material = white,
                       .transform = glm::translate(glm::mat4(1.0f), {0, 1.0f, 0})});
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    View view = lookAt(glm::vec3(0, 3.5f, 6.0f), glm::vec3(0, 0.5f, 0));
    view.probeCenter = glm::vec3(0, 1.0f, 0);
    const auto measure = [&](IndirectLight mode) {
        settings.indirectLight = mode;
        renderer->setSettings(settings);
        const std::vector<Rgba8> pixels = settle(device, *renderer, scene, view, *target, 48);
        const auto mean = [&](glm::vec3 p) {
            const glm::ivec2 c = pixelOf(view, p);
            float v = 0.0f;
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x) v += sceneValue(at(pixels, c + glm::ivec2(x, y))).g / 9.0f;
            return v;
        };
        return glm::vec2(mean({2.6f, 0, 1.0f}), mean({0, 1.5f, 0.866f}));
    };
    const glm::vec2 sky = measure(IndirectLight::Sky), probes = measure(IndirectLight::Probes),
                    traced = measure(IndirectLight::RayTraced);
    std::printf("  floor / sphere: sky %.3f %.3f, probes %.3f %.3f, traced %.3f %.3f\n", double(sky.x), double(sky.y),
                double(probes.x), double(probes.y), double(traced.x), double(traced.y));
    for (const glm::vec2 v : {probes, traced}) {
        CHECK(std::abs(v.x / sky.x - 1.0f) < 0.06f, "the open floor as with the sky's light: %.3f vs %.3f", double(v.x),
              double(sky.x));
        CHECK(v.y / sky.y > 0.85f && v.y / sky.y < 1.03f, "the sphere about as with the sky's light: %.3f vs %.3f",
              double(v.y), double(sky.y));
    }
}

// The MetalFX denoising upscaler (gpu::Caps::denoisedUpscaling): the raw one-ray shadows and occlusion at half size,
// denoised and upscaled from the G-buffer, against eight rays at full size; without it the temporal upscaler and
// the engine's denoiser.
QUENCH_TEST(renderer_denoised_upscaling) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = shadowSettings();
    settings.sunShadows = SunShadows::RayTraced;
    settings.shadowSoftness = 6.0f;
    settings.shadowRays = 8;
    settings.ambientOcclusion = AmbientOcclusion::RayTraced;
    settings.occlusionRays = 8;
    settings.antiAliasing = AntiAliasing::Taa;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (!rayTracingActive(device, *renderer)) QUENCH_SKIP("no ray tracing: shadow maps");
    Scene scene(device);
    Environment e = sunOnly();
    e.skyAmbient = e.groundAmbient = glm::vec3(0.3f);
    scene.setEnvironment(e);
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    const MeshId box = addMesh(scene, boxMesh(glm::vec3(0.5f)));
    const MaterialId grey = scene.addMaterial({.baseColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f), .roughness = 0.9f});
    scene.addInstance({.mesh = ground, .material = grey});
    scene.addInstance({.mesh = box, .material = grey, .transform = glm::translate(glm::mat4(1.0f), {0, 1.5f, 0})});
    addSun(scene, kSunDown);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const std::vector<Rgba8> reference = settle(device, *renderer, scene, kTopView, *target, 24);
    REQUIRE(reference.size() == kSize * kSize, "%zu pixels", reference.size());

    settings.shadowRays = settings.occlusionRays = 1;
    settings.renderScale = 0.5f;
    settings.upscaling = Upscaling::TemporalDenoised;
    renderer->setSettings(settings);
    const bool denoised = renderer->effects().upscaling == Upscaling::TemporalDenoised;
    if (!denoised) {
        CHECK(!device.caps().denoisedUpscaling, "the denoising upscaler on the device, not used");
        CHECK(renderer->effects().upscaling ==
                  (device.caps().temporalUpscaling ? Upscaling::Temporal : Upscaling::Bilinear),
              "the temporal upscaler in its place");
        CHECK(std::ranges::any_of(renderer->fallbacks(),
                                  [](const std::string& l) { return l.starts_with("denoising upscaler: "); }),
              "Renderer::fallbacks names the denoising upscaler");
    }
    std::vector<Rgba8> previous = settle(device, *renderer, scene, kTopView, *target, 32);
    const std::vector<Rgba8> upscaled = settle(device, *renderer, scene, kTopView, *target, 1);
    CHECK(renderer->stats().renderWidth == kSize / 2, "render width %u", renderer->stats().renderWidth);
    const ImageDiff d = compare(upscaled, reference), f = compare(upscaled, previous);
    std::printf("  %s at half size, one ray, against eight at full size: mean %.2f (%.2f%% over 16); frame to "
                "frame %.2f\n",
                denoised ? "denoising upscaler" : "temporal upscaler and the engine's denoiser", d.mean,
                d.over16 * 100.0, f.mean);
    CHECK(d.mean < 4.0 && d.over16 < 0.12, "mean %.2f, %.2f%% over 16", d.mean, d.over16 * 100.0);
    CHECK(f.mean < 1.0, "steady: %.2f from frame to frame", f.mean);
    if (!denoised) QUENCH_SKIP("no denoising upscaler on this device");

    settings.upscaling = Upscaling::Temporal;
    renderer->setSettings(settings);
    previous = settle(device, *renderer, scene, kTopView, *target, 32);
    const std::vector<Rgba8> engine = settle(device, *renderer, scene, kTopView, *target, 1);
    const ImageDiff de = compare(engine, reference), fe = compare(engine, previous);
    std::printf("  the temporal upscaler and the engine's denoiser: mean %.2f (%.2f%% over 16); frame to frame %.2f\n",
                de.mean, de.over16 * 100.0, fe.mean);
}

// ---------------------------------------------------------------- area lights and anisotropic GGX

namespace {

// The light an area light sends from a white rough ground point P toward V, integrated over the light's surface on
// the CPU with the standard surface's BRDF ((1 - F) Lambert + GGX, as quenchDirectLight): the value the raster's LTC
// approximates and the path tracer converges to.
struct AreaSample {
    glm::vec3 point;
    glm::vec3 normal;
    float area;
};

std::vector<AreaSample> areaSamples(const LightDesc& l, int n) {
    std::vector<AreaSample> out;
    const glm::vec3 t = glm::normalize(l.tangent);
    if (l.type == LightType::Tube) {
        const glm::vec3 e1 =
            glm::normalize(glm::cross(t, std::abs(t.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
        const glm::vec3 e2 = glm::cross(t, e1);
        const float r = l.size.y, half = 0.5f * l.size.x;
        const float da = 2.0f * glm::pi<float>() * r * 2.0f * half / float(n * n);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                const float phi = 2.0f * glm::pi<float>() * (float(i) + 0.5f) / float(n);
                const float s = ((float(j) + 0.5f) / float(n) * 2.0f - 1.0f) * half;
                const glm::vec3 nrm = std::cos(phi) * e1 + std::sin(phi) * e2;
                out.push_back({l.position + t * s + nrm * r, nrm, da});
            }
        return out;
    }
    const glm::vec3 nrm = glm::normalize(l.direction);
    const glm::vec3 tt = glm::normalize(t - nrm * glm::dot(nrm, t));
    const glm::vec3 b = glm::cross(nrm, tt);
    if (l.type == LightType::Disk) {
        const float r = l.size.x, da = glm::pi<float>() * r * r / float(n * n);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                const float rr = r * std::sqrt((float(i) + 0.5f) / float(n));
                const float phi = 2.0f * glm::pi<float>() * (float(j) + 0.5f) / float(n);
                out.push_back({l.position + rr * (std::cos(phi) * tt + std::sin(phi) * b), nrm, da});
            }
        return out;
    }
    const float da = l.size.x * l.size.y / float(n * n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const float u = ((float(i) + 0.5f) / float(n) - 0.5f) * l.size.x;
            const float v = ((float(j) + 0.5f) / float(n) - 0.5f) * l.size.y;
            out.push_back({l.position + u * tt + v * b, nrm, da});
        }
    return out;
}

float ggxSpecCpu(float NdotH, float NdotV, float NdotL, float rough) {
    const float a = std::max(rough * rough, 0.002f), a2 = a * a;
    const float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    const float D = a2 / (glm::pi<float>() * d * d);
    const float k = (rough + 1.0f) * (rough + 1.0f) / 8.0f;
    const float G = NdotV / (NdotV * (1.0f - k) + k) * NdotL / (NdotL * (1.0f - k) + k);
    return D * G / std::max(4.0f * NdotV * NdotL, 1e-4f);
}

// White ground (albedo 1, F0 0.04) with a roughness, normal +y.
float areaLightOnGround(const LightDesc& l, glm::vec3 p, glm::vec3 eye, float rough) {
    const glm::vec3 N(0, 1, 0), V = glm::normalize(eye - p);
    const float NdotV = std::max(glm::dot(N, V), 1e-3f);
    double sum = 0.0;
    for (const AreaSample& s : areaSamples(l, 160)) {
        const glm::vec3 d = s.point - p;
        const float d2 = glm::dot(d, d);
        const glm::vec3 L = d / std::sqrt(d2);
        float cosLight = -glm::dot(s.normal, L);
        if (l.twoSided && l.type != LightType::Tube) cosLight = std::abs(cosLight);
        const float NdotL = glm::dot(N, L);
        if (cosLight <= 0.0f || NdotL <= 0.0f) continue;
        const glm::vec3 H = glm::normalize(L + V);
        const float F = 0.04f + 0.96f * std::pow(1.0f - std::clamp(glm::dot(V, H), 0.0f, 1.0f), 5.0f);
        const float brdf =
            (1.0f - F) / glm::pi<float>() + F * ggxSpecCpu(std::max(glm::dot(N, H), 0.0f), NdotV, NdotL, rough);
        sum += double(brdf * NdotL * cosLight * s.area / d2);
    }
    return float(sum) * l.intensity;
}

// Three area lights over a white ground, one of each shape, and a fourth upright one facing the camera that lights
// nothing near the measured points (its range ends 0.5 m from it): the shape that shows its radiance (not behind the
// disk, whose back is black from above).
struct AreaScene {
    std::array<LightDesc, 3> lights;
    LightDesc facing;
    std::array<glm::vec3, 3> points{glm::vec3(-2.5f, 0, 0), glm::vec3(0, 0, 0), glm::vec3(2.5f, 0, 0)};
    glm::vec3 eye{0, 3.5f, 5.0f};
};

AreaScene areaScene() {
    AreaScene a;
    a.lights[0] = {.type = LightType::Rect,
                   .color = glm::vec3(1.0f, 0.9f, 0.8f),
                   .intensity = 4.0f,
                   .position = {-2.5f, 1.5f, 0},
                   .range = 20.0f,
                   .size = {1.0f, 0.5f}};
    a.lights[1] = {.type = LightType::Disk,
                   .color = glm::vec3(0.8f, 0.9f, 1.0f),
                   .intensity = 4.0f,
                   .position = {0, 1.5f, 0},
                   .range = 20.0f,
                   .size = {0.4f, 0.0f}};
    a.lights[2] = {.type = LightType::Tube,
                   .color = glm::vec3(1.0f),
                   .intensity = 10.0f,
                   .position = {2.5f, 1.0f, 0},
                   .range = 20.0f,
                   .size = {1.2f, 0.05f}};
    a.facing = {.type = LightType::Rect,
                .color = glm::vec3(1.0f),
                .intensity = 0.5f,
                .position = {-1.2f, 0.5f, -3.0f},
                .direction = glm::normalize(glm::vec3(1.2f, 3.0f, 8.0f)),
                .range = 0.5f,
                .size = {0.8f, 0.6f}};
    return a;
}

// The mean scene value (green) of the 3 x 3 pixels around a world point.
float meanValue(const std::vector<Rgba8>& pixels, const View& view, glm::vec3 p) {
    const glm::ivec2 c = pixelOf(view, p);
    float v = 0.0f;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) v += sceneValue(at(pixels, c + glm::ivec2(x, y))).g / 9.0f;
    return v;
}

// Where the middle of a pixel's view ray meets the ground (y = 0).
glm::vec3 groundOf(const View& view, glm::ivec2 pixel) {
    const glm::mat4 inverse = glm::inverse(reversedPerspective(view.fovY, 1.0f, view.nearPlane) * view.view);
    const glm::vec2 ndc((float(pixel.x) + 0.5f) / float(kSize) * 2.0f - 1.0f,
                        1.0f - (float(pixel.y) + 0.5f) / float(kSize) * 2.0f);
    const glm::vec4 a = inverse * glm::vec4(ndc, 1.0f, 1.0f), b = inverse * glm::vec4(ndc, 0.5f, 1.0f);
    const glm::vec3 near = glm::vec3(a) / a.w, far = glm::vec3(b) / b.w;
    return near + (far - near) * (near.y / (near.y - far.y));
}

// The lights' integrated green over the same 3 x 3 pixels' ground points (meanValue's).
float meanIntegral(const std::array<LightDesc, 3>& lights, const View& view, glm::vec3 p, glm::vec3 eye, float rough) {
    const glm::ivec2 c = pixelOf(view, p);
    float v = 0.0f;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            for (const LightDesc& l : lights)
                v += areaLightOnGround(l, groundOf(view, c + glm::ivec2(x, y)), eye, rough) * l.color.g / 9.0f;
    return v;
}

} // namespace

// Rect, disk and tube lights on a rough white ground: the raster's LTC shading against the BRDF integrated over each
// light's surface on the CPU, on every backend (no rays involved); the shape of a light shows its radiance.
QUENCH_TEST(renderer_area_lights_match_their_integrals) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, plainSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    scene.setEnvironment(sunOnly());
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(5.0f)));
    scene.addInstance(
        {.mesh = ground, .material = scene.addMaterial({.baseColor = glm::vec4(1.0f), .roughness = 1.0f})});
    const AreaScene a = areaScene();
    for (const LightDesc& l : a.lights) scene.addLight(l);
    scene.addLight(a.facing);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(a.eye, glm::vec3(0));
    const std::vector<Rgba8> pixels = renderOnce(device, *renderer, scene, view, *target);
    REQUIRE(pixels.size() == kSize * kSize, "%zu pixels", pixels.size());
    const char* names[3] = {"rect", "disk", "tube"};
    for (std::size_t i = 0; i < a.points.size(); ++i) {
        const float expected = meanIntegral(a.lights, view, a.points[i], a.eye, 1.0f);
        const float measured = meanValue(pixels, view, a.points[i]);
        std::printf("  under the %s: %.3f, integrated %.3f (%.1f%%)\n", names[i], double(measured), double(expected),
                    double((measured / expected - 1.0f) * 100.0f));
        CHECK(std::abs(measured / expected - 1.0f) < 0.08f, "under the %s: %.3f vs %.3f", names[i], double(measured),
              double(expected));
    }
    const float shape = meanValue(pixels, view, a.facing.position);
    std::printf("  the facing rect's surface %.3f (radiance %.3f)\n", double(shape), double(a.facing.intensity));
    CHECK(std::abs(shape - a.facing.intensity) < 0.03f, "the shape shows its radiance: %.3f", double(shape));
    CHECK(renderer->stats().lights == 4, "%u local lights", renderer->stats().lights);
}

// The shapes of area lights are the scene's own: not in its counts, moved with the light, gone with it; a one-sided
// light sends nothing behind it.
QUENCH_TEST(renderer_area_light_shapes_follow_their_lights) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, plainSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    Environment e = sunOnly();
    e.fogColor = glm::vec3(0.0f); // the sky below the horizon, behind the panel
    scene.setEnvironment(e);
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(5.0f)));
    scene.addInstance(
        {.mesh = ground, .material = scene.addMaterial({.baseColor = glm::vec4(1.0f), .roughness = 1.0f})});
    // A rect facing the camera at the middle of the view, 1 m up; it lights the ground in front of it.
    LightDesc panel{.type = LightType::Rect,
                    .intensity = 0.6f,
                    .position = {0, 1.0f, 0},
                    .direction = {0, 0, 1.0f},
                    .range = 6.0f,
                    .size = {1.0f, 1.0f}};
    const LightId light = scene.addLight(panel);
    CHECK(scene.meshCount() == 1 && scene.materialCount() == 1 && scene.instanceCount() == 1 && scene.lightCount() == 1,
          "counts: %u meshes, %u materials, %u instances, %u lights", scene.meshCount(), scene.materialCount(),
          scene.instanceCount(), scene.lightCount());
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0, 1.0f, 4.0f), glm::vec3(0, 1.0f, 0));
    const glm::vec3 front(0, 0, 1.5f);
    std::vector<Rgba8> pixels = renderOnce(device, *renderer, scene, view, *target);
    const float face = meanValue(pixels, view, panel.position), lit = meanValue(pixels, view, front);
    std::printf("  the panel %.3f, the ground in front %.3f\n", double(face), double(lit));
    CHECK(std::abs(face - panel.intensity) < 0.03f, "the panel shows its radiance: %.3f", double(face));
    CHECK(lit > 0.02f, "the ground in front is lit: %.3f", double(lit));

    // Turned away: its back is a black surface (one-sided) and the ground in front gets nothing; the ground behind it
    // is lit.
    panel.direction = {0, 0, -1.0f};
    scene.setLight(light, panel);
    pixels = renderOnce(device, *renderer, scene, view, *target);
    const float back = sceneValue(at(pixels, pixelOf(view, panel.position))).g;
    const float dark = meanValue(pixels, view, front), lit2 = meanValue(pixels, view, {0, 0, -1.2f});
    std::printf("  turned away: the panel's back %.3f, in front %.3f, behind %.3f\n", double(back), double(dark),
                double(lit2));
    CHECK(back < 0.02f && dark < 0.005f, "nothing toward the camera: %.3f, %.3f", double(back), double(dark));
    CHECK(lit2 > 0.01f, "the ground behind it is lit: %.3f", double(lit2));

    // Two-sided: both faces show the radiance and light the ground.
    panel.twoSided = true;
    scene.setLight(light, panel);
    pixels = renderOnce(device, *renderer, scene, view, *target);
    CHECK(std::abs(meanValue(pixels, view, panel.position) - panel.intensity) < 0.03f &&
              meanValue(pixels, view, front) > 0.02f,
          "two-sided: %.3f, %.3f", double(meanValue(pixels, view, panel.position)),
          double(meanValue(pixels, view, front)));

    // A point light now: no shape; removed: nothing left.
    scene.setLight(light, {.type = LightType::Point, .intensity = 0.0f, .position = {0, 1.0f, 0}});
    pixels = renderOnce(device, *renderer, scene, view, *target);
    CHECK(sceneValue(at(pixels, pixelOf(view, panel.position))).g < 0.01f, "no shape for a point light");
    panel.type = LightType::Disk;
    panel.size = {0.5f, 0.0f};
    panel.direction = {0, 0, 1.0f};
    scene.setLight(light, panel);
    pixels = renderOnce(device, *renderer, scene, view, *target);
    CHECK(std::abs(meanValue(pixels, view, panel.position) - panel.intensity) < 0.03f, "a disk again: %.3f",
          double(meanValue(pixels, view, panel.position)));
    scene.removeLight(light);
    pixels = renderOnce(device, *renderer, scene, view, *target);
    CHECK(sceneValue(at(pixels, pixelOf(view, panel.position))).g < 0.01f, "removed with its light");
    CHECK(scene.meshCount() == 1 && scene.materialCount() == 1 && scene.instanceCount() == 1 && scene.lightCount() == 0,
          "counts: %u meshes, %u materials, %u instances, %u lights", scene.meshCount(), scene.materialCount(),
          scene.instanceCount(), scene.lightCount());
}

// Ray-traced area light shadows: a 1 m box under a 1 m square light shadows the ground below it fully (the umbra is
// 1 m wide), the ground beside that partly (the penumbra of an area: about 40% of the light seen at 1.3 m from the
// middle), the open ground not at all.
QUENCH_TEST(renderer_ray_traced_area_light_shadows) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.localLightShadows = 1;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (!lightingActive(device, *renderer, "ray-traced local light shadows: "))
        QUENCH_SKIP("no ray tracing: unshadowed area lights");
    Scene scene(device);
    scene.setEnvironment(sunOnly());
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(10.0f)));
    const MeshId box = addMesh(scene, boxMesh(glm::vec3(0.5f)));
    const MaterialId grey = scene.addMaterial({.baseColor = glm::vec4(0.6f, 0.6f, 0.6f, 1.0f), .roughness = 0.9f});
    scene.addInstance({.mesh = ground, .material = grey});
    scene.addInstance({.mesh = box, .material = grey, .transform = glm::translate(glm::mat4(1.0f), {0, 1.5f, 0})});
    scene.addLight(
        {.type = LightType::Rect, .intensity = 6.0f, .position = {0, 3.0f, 0}, .range = 12.0f, .size = {1.0f, 1.0f}});
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    // From the side, so the light's shape and the box do not hide the measured ground.
    const View view = lookAt(glm::vec3(0, 4.0f, 6.0f), glm::vec3(0));
    const glm::vec3 umbra(0, 0, 0), penumbra(1.3f, 0, 0), open(2.5f, 0, 0);
    const std::vector<Rgba8> traced = settle(device, *renderer, scene, view, *target, 24);
    REQUIRE(traced.size() == kSize * kSize, "%zu pixels", traced.size());
    settings.localLightShadows = 0;
    renderer->setSettings(settings);
    const std::vector<Rgba8> unshadowed = settle(device, *renderer, scene, view, *target, 2);
    const float u = meanValue(traced, view, umbra) / meanValue(unshadowed, view, umbra);
    const float p = meanValue(traced, view, penumbra) / meanValue(unshadowed, view, penumbra);
    const float o = meanValue(traced, view, open) / meanValue(unshadowed, view, open);
    std::printf("  lit share under the box %.3f, beside it %.3f, in the open %.3f\n", double(u), double(p), double(o));
    CHECK(u < 0.1f, "the umbra: %.3f", double(u));
    CHECK(p > 0.2f && p < 0.8f, "the penumbra: %.3f", double(p));
    CHECK(o > 0.95f && o < 1.05f, "the open ground: %.3f", double(o));
}

// The path tracer and the raster agree on area lights: the scene of renderer_area_lights_match_their_integrals traced
// with next-event estimation on the lights and MIS against BRDF samples that hit their shapes.
QUENCH_TEST(renderer_path_tracer_area_lights) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    settings.pathTracer = true;
    settings.pathBounces = 2;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (!lightingActive(device, *renderer, "path tracer: ")) QUENCH_SKIP("no ray tracing: no path tracer");
    Scene scene(device);
    scene.setEnvironment(sunOnly());
    const MeshId ground = addMesh(scene, planeMesh(glm::vec2(5.0f)));
    // A smoother ground than the raster test's, so BRDF samples hit the shapes too and MIS weighs both.
    const float rough = 0.5f;
    scene.addInstance(
        {.mesh = ground, .material = scene.addMaterial({.baseColor = glm::vec4(1.0f), .roughness = rough})});
    const AreaScene a = areaScene();
    for (const LightDesc& l : a.lights) scene.addLight(l);
    scene.addLight(a.facing);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(a.eye, glm::vec3(0));
    const std::vector<Rgba8> traced = settle(device, *renderer, scene, view, *target, 256);
    REQUIRE(traced.size() == kSize * kSize, "%zu pixels", traced.size());
    settings.pathTracer = false;
    renderer->setSettings(settings);
    const std::vector<Rgba8> raster = settle(device, *renderer, scene, view, *target, 1);
    const char* names[3] = {"rect", "disk", "tube"};
    for (std::size_t i = 0; i < a.points.size(); ++i) {
        const float expected = meanIntegral(a.lights, view, a.points[i], a.eye, rough);
        const float t = meanValue(traced, view, a.points[i]), r = meanValue(raster, view, a.points[i]);
        std::printf("  under the %s: path traced %.3f, raster %.3f, integrated %.3f\n", names[i], double(t), double(r),
                    double(expected));
        CHECK(std::abs(t / expected - 1.0f) < 0.08f, "path traced under the %s: %.3f vs %.3f", names[i], double(t),
              double(expected));
        CHECK(std::abs(r / expected - 1.0f) < 0.12f, "raster under the %s: %.3f vs %.3f", names[i], double(r),
              double(expected));
    }
    const float shape = meanValue(traced, view, a.facing.position);
    std::printf("  the facing rect's surface: path traced %.3f, raster %.3f\n", double(shape),
                double(meanValue(raster, view, a.facing.position)));
    CHECK(std::abs(shape - a.facing.intensity) < 0.03f, "the path tracer shows its radiance: %.3f", double(shape));
}

namespace {

// The extent (pixels) of a highlight along a row and a column through its peak: pixels above half the peak.
glm::ivec2 highlightExtent(const std::vector<Rgba8>& pixels, glm::ivec2 c) {
    const float peak = sceneValue(at(pixels, c)).g;
    glm::ivec2 n(0);
    for (int i = 0; i < int(kSize); ++i) {
        if (sceneValue(at(pixels, {i, c.y})).g > 0.5f * peak) ++n.x;
        if (sceneValue(at(pixels, {c.x, i})).g > 0.5f * peak) ++n.y;
    }
    return n;
}

} // namespace

// Anisotropic GGX: a point light's highlight on brushed metal stretches along the anisotropy direction (the plane's
// tangent, +x, turned by the rotation), in the raster and in the path tracer.
QUENCH_TEST(renderer_anisotropic_highlight_stretches) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = plainSettings();
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    scene.setEnvironment(sunOnly());
    const MeshId plate = addMesh(scene, planeMesh(glm::vec2(5.0f)));
    MaterialDesc brushed{.baseColor = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f), .roughness = 0.3f, .metallic = 1.0f};
    const MaterialId isotropic = scene.addMaterial(brushed);
    brushed.anisotropy = 0.8f;
    const MaterialId alongX = scene.addMaterial(brushed);
    brushed.anisotropyRotation = glm::half_pi<float>();
    const MaterialId alongZ = scene.addMaterial(brushed);
    const InstanceId instance = scene.addInstance({.mesh = plate, .material = isotropic});
    scene.addLight({.type = LightType::Point, .intensity = 6.0f, .position = {0, 3.0f, 0}, .range = 20.0f});
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = lookAt(glm::vec3(0, 4.0f, 0.01f), glm::vec3(0), glm::vec3(0, 0, -1));
    const glm::ivec2 c = pixelOf(view, glm::vec3(0));
    const auto extent = [&](MaterialId m, int frames) {
        scene.setInstanceMaterial(instance, m);
        return highlightExtent(settle(device, *renderer, scene, view, *target, frames), c);
    };
    const glm::ivec2 iso = extent(isotropic, 1), x = extent(alongX, 1), z = extent(alongZ, 1);
    std::printf("  highlight width x height (pixels): isotropic %d x %d, along x %d x %d, rotated %d x %d\n", iso.x,
                iso.y, x.x, x.y, z.x, z.y);
    CHECK(std::abs(iso.x - iso.y) <= 2, "isotropic: round");
    CHECK(x.x > x.y * 3 / 2 && z.y > z.x * 3 / 2, "stretched along the direction: %d x %d, %d x %d", x.x, x.y, z.x,
          z.y);

    settings.pathTracer = true;
    renderer->setSettings(settings);
    if (!renderer->effects().pathTracer) QUENCH_SKIP("no ray tracing: no path tracer");
    const glm::ivec2 px = extent(alongX, 64), pz = extent(alongZ, 64);
    std::printf("  path traced: along x %d x %d, rotated %d x %d\n", px.x, px.y, pz.x, pz.y);
    CHECK(px.x > px.y * 3 / 2 && pz.y > pz.x * 3 / 2, "path traced, stretched: %d x %d, %d x %d", px.x, px.y, pz.x,
          pz.y);
    CHECK(std::abs(px.x - x.x) * 5 <= x.x + 5 && std::abs(px.y - x.y) * 5 <= x.y + 5,
          "as wide as the raster's: %d x %d", px.x, px.y);
}
