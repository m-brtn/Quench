// The scene's particles and trails on the GPU (docs/particles.md): small offscreen renders against a black
// background, checked by their pixels; the GPU step against the CPU one, the depth test, TAA's history, trails.

#include "GpuTest.h"
#include "render/Effects.h"

#include <quench/render/MeshBuilder.h>
#include <quench/render/Projection.h>
#include <quench/render/Renderer.h>

#include <glm/ext/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace quench;
using namespace quench::render;
using test::Rgba8;

namespace {

constexpr std::uint32_t kSize = 64;

Result<gpu::Texture> makeTarget(gpu::Device& device) {
    return device.createTexture({.format = gpu::TextureFormat::Rgba8Unorm,
                                 .usage = gpu::TextureUsage::ColorTarget,
                                 .width = kSize,
                                 .height = kSize,
                                 .name = "particle test target"});
}

// A black background, nothing that varies between frames or blurs across pixels.
RenderSettings blackSettings(ParticleSimulation mode = ParticleSimulation::Gpu) {
    RenderSettings s;
    s.antiAliasing = AntiAliasing::None;
    s.bloom = false;
    s.shadowCascades = 0;
    s.vignette = 0.0f;
    s.dither = false;
    s.sky = false;
    s.particles = mode;
    return s;
}

Environment dark() {
    Environment e;
    e.skyAmbient = e.groundAmbient = glm::vec3(0.0f);
    e.skyZenith = e.skyHorizon = e.skyGround = e.sunGlow = e.sunDisc = glm::vec3(0.0f);
    e.atmosphere = 0.0f;
    return e;
}

View viewAt(double time, glm::vec3 eye = glm::vec3(0.0f, 0.0f, 4.0f)) {
    return {
        .view = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0, 1, 0)), .fovY = 0.9f, .nearPlane = 0.1f, .time = time};
}

glm::ivec2 pixelOf(const View& view, glm::vec3 p) {
    const glm::vec4 c = reversedPerspective(view.fovY, 1.0f, view.nearPlane) * view.view * glm::vec4(p, 1.0f);
    const glm::vec2 ndc = glm::vec2(c) / c.w;
    return {int((ndc.x * 0.5f + 0.5f) * float(kSize)), int((0.5f - ndc.y * 0.5f) * float(kSize))};
}

Rgba8 at(const std::vector<Rgba8>& pixels, glm::ivec2 p) { return pixels[std::size_t(p.y) * kSize + std::size_t(p.x)]; }
int sum(Rgba8 c) { return c.r + c.g + c.b; }

std::vector<Rgba8> renderOnce(gpu::Device& device, Renderer& renderer, Scene& scene, const View& view,
                              const gpu::Texture& target) {
    gpu::CommandBuffer commands = device.acquireCommandBuffer();
    renderer.render(scene, view, commands, target);
    CHECK(commands.submit(), "submit");
    return test::readRgba8(device, target);
}

// A still glow that lives long: one per burst, where the emitter is.
particles::ParticleDesc stillGlow() {
    particles::ParticleDesc d;
    d.maxParticles = 16;
    d.rate = 0.0f;
    d.lifetime = {100.0f, 100.0f};
    d.speed = {0.0f, 0.0f};
    d.size = {0.25f, 0.25f};
    d.color = glm::vec3(4.0f, 2.0f, 1.0f);
    d.colorOverLife = particles::ColorCurve(glm::vec4(1.0f));
    d.softness = 0.0f;
    d.nearFade = 0.0f;
    return d;
}

ParticleSystemId add(Scene& scene, const particles::ParticleDesc& desc, glm::vec3 place = glm::vec3(0.0f)) {
    Result<ParticleSystemId> id = scene.addParticles(desc, glm::translate(glm::mat4(1.0f), place));
    CHECK(id, "addParticles: %s", id ? "" : id.error().c_str());
    return id ? *id : ParticleSystemId{};
}

} // namespace

QUENCH_TEST(renderer_particles_glow_on_gpu_and_cpu) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, blackSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    scene.setEnvironment(dark());
    const ParticleSystemId glow = add(scene, stillGlow());
    scene.burstParticles(glow, 1);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = viewAt(0.0);
    const glm::ivec2 centre = pixelOf(view, glm::vec3(0.0f));
    const std::vector<Rgba8> gpuPixels = renderOnce(device, *renderer, scene, view, *target);
    const RenderStats gpuStats = renderer->stats();
    const Rgba8 c = at(gpuPixels, centre);
    CHECK(c.r > 200 && c.r > c.g && c.g > c.b, "an orange glow at the centre: %d %d %d", c.r, c.g, c.b);
    CHECK(sum(at(gpuPixels, {2, 2})) == 0, "black away from it");
    CHECK(gpuStats.particleSystems == 1 && gpuStats.particles == 1 && gpuStats.particleSpawns == 1,
          "stats: %u systems, %u particles, %u spawns", gpuStats.particleSystems, gpuStats.particles,
          gpuStats.particleSpawns);
    const bool compute = device.caps().computeShaders;
    CHECK(renderer->effects().particles == (compute ? ParticleSimulation::Gpu : ParticleSimulation::Cpu),
          "simulation %d", int(renderer->effects().particles));

    // On the CPU the system starts again: the same burst draws the same glow.
    renderer->setSettings(blackSettings(ParticleSimulation::Cpu));
    scene.burstParticles(glow, 1);
    const std::vector<Rgba8> cpuPixels = renderOnce(device, *renderer, scene, viewAt(1.0), *target);
    CHECK(renderer->effects().particles == ParticleSimulation::Cpu, "CPU simulation");
    CHECK(renderer->stats().particles == 1, "%u particles on the CPU", renderer->stats().particles);
    int worst = 0;
    for (std::size_t i = 0; i < gpuPixels.size(); ++i) {
        worst = std::max(worst, std::abs(gpuPixels[i].r - cpuPixels[i].r));
        worst = std::max(worst, std::abs(gpuPixels[i].g - cpuPixels[i].g));
        worst = std::max(worst, std::abs(gpuPixels[i].b - cpuPixels[i].b));
    }
    std::printf("  glow %d %d %d, CPU against GPU: at most %d levels\n", c.r, c.g, c.b, worst);
    CHECK(worst <= 2, "the CPU's glow matches the GPU's within %d levels", worst);

    // Off: nothing drawn, nothing stepped.
    renderer->setSettings(blackSettings(ParticleSimulation::Off));
    const std::vector<Rgba8> off = renderOnce(device, *renderer, scene, viewAt(2.0), *target);
    CHECK(sum(at(off, centre)) == 0 && renderer->stats().particleSystems == 0, "no particles when off");
}

QUENCH_TEST(renderer_particles_move_as_on_the_cpu) {
    QUENCH_GPU_DEVICE(device);
    if (!device.caps().computeShaders) QUENCH_SKIP("no compute shaders");
    // Particles in a swirl that rise, slow down and fall: every part of the motion and the look, frame by frame.
    particles::ParticleDesc d;
    d.maxParticles = 512;
    d.rate = 120.0f;
    d.shape = particles::Shape::Sphere;
    d.extent = glm::vec3(0.3f);
    d.lifetime = {1.0f, 1.6f};
    d.speed = {0.8f, 1.2f};
    d.spread = 0.6f;
    d.gravity = glm::vec3(0.0f, -1.0f, 0.0f);
    d.drag = 0.8f;
    d.noise = 0.6f;
    d.noiseScale = 1.5f;
    d.size = {0.04f, 0.08f};
    d.color = glm::vec3(2.0f, 1.0f, 3.0f);
    d.twinkle = 0.5f;
    d.twinkleRate = 3.0f;
    d.softness = 0.0f;
    d.nearFade = 0.0f;
    std::vector<Rgba8> images[2];
    std::uint32_t alive[2] = {0, 0};
    for (int run = 0; run < 2; ++run) {
        Result<Renderer> renderer =
            Renderer::create(device, blackSettings(run == 0 ? ParticleSimulation::Gpu : ParticleSimulation::Cpu));
        REQUIRE(renderer, "%s", renderer.error().c_str());
        Scene scene(device);
        scene.setEnvironment(dark());
        add(scene, d, glm::vec3(0.0f, -0.5f, 0.0f));
        Result<gpu::Texture> target = makeTarget(device);
        REQUIRE(target, "%s", target.error().c_str());
        for (int frame = 0; frame <= 60; ++frame) {
            gpu::CommandBuffer commands = device.acquireCommandBuffer();
            renderer->render(scene, viewAt(frame / 60.0), commands, *target);
            CHECK(commands.submit(), "submit");
        }
        images[run] = test::readRgba8(device, *target);
        alive[run] = renderer->stats().particles;
    }
    CHECK(alive[0] == alive[1] && alive[0] > 100, "the same particles alive: %u on the GPU, %u on the CPU", alive[0],
          alive[1]);
    double total = 0.0;
    int lit = 0;
    for (std::size_t i = 0; i < images[0].size(); ++i) {
        total += std::abs(images[0][i].r - images[1][i].r) + std::abs(images[0][i].g - images[1][i].g) +
                 std::abs(images[0][i].b - images[1][i].b);
        lit += sum(images[0][i]) > 30 ? 1 : 0;
    }
    const double mean = total / double(images[0].size() * 3);
    std::printf("  %u alive, %d pixels lit, GPU against CPU: mean difference %.3f levels\n", alive[0], lit, mean);
    CHECK(lit > 40, "%d pixels lit", lit);
    CHECK(mean < 1.0, "the GPU's frame matches the CPU's: mean difference %.3f levels", mean);
}

QUENCH_TEST(renderer_particles_hide_behind_surfaces) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, blackSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    scene.setEnvironment(dark());
    // A black wall in front of the left glow; the right one in the open, the middle one 5 cm in front of the back wall.
    Result<MeshId> box = scene.addMesh(boxMesh(glm::vec3(0.4f, 0.4f, 0.05f)).desc());
    REQUIRE(box, "%s", box.error().c_str());
    const MaterialId black = scene.addMaterial({.baseColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)});
    scene.addInstance(
        {.mesh = *box, .material = black, .transform = glm::translate(glm::mat4(1.0f), {-1.0f, 0, 1.0f})});
    scene.addInstance({.mesh = *box, .material = black, .transform = glm::translate(glm::mat4(1.0f), {0, 0, -0.1f})});
    particles::ParticleDesc d = stillGlow();
    d.softness = 1.0f;
    const ParticleSystemId left = add(scene, d, {-1.0f, 0, 0});
    const ParticleSystemId right = add(scene, d, {1.0f, 0, 0});
    const ParticleSystemId middle = add(scene, d, {0, 0, 0});
    for (const ParticleSystemId id : {left, right, middle}) scene.burstParticles(id, 1);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = viewAt(0.0);
    const std::vector<Rgba8> soft = renderOnce(device, *renderer, scene, view, *target);
    const int behind = sum(at(soft, pixelOf(view, {-1.0f, 0, 0})));
    const int open = sum(at(soft, pixelOf(view, {1.0f, 0, 0})));
    const int nearWall = sum(at(soft, pixelOf(view, {0, 0, 0})));
    CHECK(open > 300, "bright in the open: %d", open);
    // A hard cut in front of the back wall: bright again.
    d.softness = 0.0f;
    CHECK(scene.setParticles(middle, d), "setParticles");
    scene.setParticlesVisible(left, false);
    const std::vector<Rgba8> hard = renderOnce(device, *renderer, scene, viewAt(0.1), *target);
    const int cut = sum(at(hard, pixelOf(view, {0, 0, 0})));
    const int wall = sum(at(hard, pixelOf(view, {-1.0f, 0, 0})));
    CHECK(std::abs(behind - wall) <= 2, "hidden behind the wall: %d, the wall alone %d", behind, wall);
    CHECK(cut > 300 && nearWall < cut / 2, "fading out near the wall behind it: %d, %d without softness", nearWall,
          cut);
}

QUENCH_TEST(renderer_particles_stretch_along_their_motion) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, blackSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    scene.setEnvironment(dark());
    particles::ParticleDesc d = stillGlow();
    d.speed = {10.0f, 10.0f};
    d.direction = glm::vec3(1.0f, 0.0f, 0.0f);
    d.spread = 0.0f;
    d.size = {0.1f, 0.1f};
    d.color = glm::vec3(8.0f);
    d.facing = particles::Facing::Velocity;
    d.stretch = 0.1f; // a metre behind it
    const ParticleSystemId spark = add(scene, d, {0.5f, 0, 0});
    scene.burstParticles(spark, 1);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const View view = viewAt(0.0);
    const std::vector<Rgba8> pixels = renderOnce(device, *renderer, scene, view, *target);
    const glm::ivec2 head = pixelOf(view, {0.5f, 0, 0});
    int along = 0, across = 0;
    for (int x = 0; x < int(kSize); ++x) along += sum(at(pixels, {x, head.y})) > 20 ? 1 : 0;
    for (int y = 0; y < int(kSize); ++y) across += sum(at(pixels, {head.x, y})) > 20 ? 1 : 0;
    CHECK(along >= 3 * across && along >= 8, "drawn out along its motion: %d pixels along, %d across", along, across);
    CHECK(sum(at(pixels, pixelOf(view, {0.0f, 0, 0}))) > sum(at(pixels, pixelOf(view, {1.0f, 0, 0}))),
          "behind it, not ahead of it");
}

QUENCH_TEST(renderer_particles_leave_no_taa_history) {
    QUENCH_GPU_DEVICE(device);
    RenderSettings settings = blackSettings();
    settings.antiAliasing = AntiAliasing::Taa;
    Result<Renderer> renderer = Renderer::create(device, settings);
    REQUIRE(renderer, "%s", renderer.error().c_str());
    if (renderer->effects().antiAliasing != AntiAliasing::Taa) QUENCH_SKIP("no TAA on this device");
    Scene scene(device);
    scene.setEnvironment(dark());
    const ParticleSystemId glow = add(scene, stillGlow());
    scene.burstParticles(glow, 1);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    const glm::ivec2 centre = pixelOf(viewAt(0.0), glm::vec3(0.0f));
    std::vector<Rgba8> pixels;
    for (int frame = 0; frame < 8; ++frame)
        pixels = renderOnce(device, *renderer, scene, viewAt(frame / 60.0), *target);
    const int shown = sum(at(pixels, centre));
    scene.setParticlesVisible(glow, false);
    pixels = renderOnce(device, *renderer, scene, viewAt(8 / 60.0), *target);
    const int gone = sum(at(pixels, centre));
    CHECK(shown > 300, "the glow with TAA: %d", shown);
    CHECK(gone == 0, "hidden, it leaves nothing in TAA's history: %d", gone);
}

QUENCH_TEST(renderer_trails_fade_behind_the_head) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, blackSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    scene.setEnvironment(dark());
    particles::TrailDesc d;
    d.lifetime = 1.0f;
    d.spacing = 0.05f;
    d.width = 0.3f;
    d.color = glm::vec3(4.0f);
    d.softness = 0.0f;
    const TrailId trail = scene.addTrail(d);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    // The head crosses the view from left to right in a second, a move and a frame at 60 Hz.
    std::vector<Rgba8> pixels;
    for (int i = 0; i <= 60; ++i) {
        const double t = i / 60.0;
        scene.moveTrail(trail, glm::vec3(-1.5f + 3.0f * float(t), 0.0f, 0.0f), t);
        pixels = renderOnce(device, *renderer, scene, viewAt(t), *target);
    }
    const View view = viewAt(1.0);
    const int head = sum(at(pixels, pixelOf(view, {1.35f, 0, 0})));
    const int middle = sum(at(pixels, pixelOf(view, {0.0f, 0, 0})));
    const int tail = sum(at(pixels, pixelOf(view, {-1.45f, 0, 0})));
    const int off = sum(at(pixels, pixelOf(view, {0.0f, 0.5f, 0})));
    CHECK(head > middle && middle > tail, "brighter toward the head: %d, %d, %d", head, middle, tail);
    CHECK(head > 300 && tail < 60, "bright at the head, faded at the tail: %d, %d", head, tail);
    CHECK(off == 0, "nothing beside it: %d", off);
    CHECK(renderer->stats().trails == 1 && renderer->stats().trailVertices > 30, "stats: %u trails, %u vertices",
          renderer->stats().trails, renderer->stats().trailVertices);
    // The head stops: a second later on the renderer's clock the strip has faded out.
    pixels = renderOnce(device, *renderer, scene, viewAt(2.1), *target);
    CHECK(sum(at(pixels, pixelOf(view, {1.35f, 0, 0}))) == 0 && renderer->stats().trails == 0,
          "faded out after the head stopped");
}

QUENCH_TEST(renderer_trails_fade_where_seen_end_on) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, blackSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    scene.setEnvironment(dark());
    particles::TrailDesc d;
    d.lifetime = 2.0f;
    d.width = 0.6f;
    d.widthOverAge = particles::FloatCurve(1.0f);
    d.color = glm::vec3(4.0f);
    d.softness = 0.0f;
    const TrailId trail = scene.addTrail(d);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    // The head flies away from the camera, 3 to 7 m from it, wobbling by 5 mm: the strip points along the view, and
    // its direction on the screen turns from point to point.
    std::vector<Rgba8> pixels;
    for (int i = 0; i <= 60; ++i) {
        const double t = i / 60.0;
        const glm::vec3 head(0.005f * std::sin(30.0f * float(t)), 0.005f * std::cos(23.0f * float(t)),
                             1.0f - 4.0f * float(t));
        scene.moveTrail(trail, head, t);
        pixels = renderOnce(device, *renderer, scene, viewAt(t), *target);
    }
    const glm::ivec2 centre = pixelOf(viewAt(1.0), glm::vec3(0.0f));
    int fanned = 0;
    for (int y = 0; y < int(kSize); ++y)
        for (int x = 0; x < int(kSize); ++x)
            if (std::abs(x - centre.x) + std::abs(y - centre.y) > 2 && sum(at(pixels, {x, y})) > 30) ++fanned;
    // The same strip from the side.
    const View side = viewAt(1.0, glm::vec3(4.0f, 0.0f, 0.0f));
    pixels = renderOnce(device, *renderer, scene, side, *target);
    int lit = 0;
    for (const Rgba8& p : pixels) lit += sum(p) > 30 ? 1 : 0;
    std::printf("  end on: %d pixels lit away from it; from the side: %d\n", fanned, lit);
    CHECK(fanned <= 8, "no fan of quads round a strip seen end on: %d pixels", fanned);
    CHECK(lit > 400, "seen from the side it is drawn: %d pixels", lit);
}

QUENCH_TEST(renderer_trails_shed_sparkles) {
    QUENCH_GPU_DEVICE(device);
    Result<Renderer> renderer = Renderer::create(device, blackSettings());
    REQUIRE(renderer, "%s", renderer.error().c_str());
    Scene scene(device);
    scene.setEnvironment(dark());
    particles::ParticleDesc sparkles = stillGlow();
    sparkles.maxParticles = 256;
    sparkles.size = {0.03f, 0.03f};
    const ParticleSystemId system = add(scene, sparkles);
    const TrailId trail = scene.addTrail({});
    scene.setTrailSparkles(trail, system, 20.0f);
    Result<gpu::Texture> target = makeTarget(device);
    REQUIRE(target, "%s", target.error().c_str());
    for (int i = 0; i <= 10; ++i) {
        scene.moveTrail(trail, glm::vec3(float(i) * 0.1f, 0.0f, 0.0f), i / 60.0);
        renderOnce(device, *renderer, scene, viewAt(i / 60.0), *target);
    }
    // 1 m at 20 per metre: 20 sparkles, the first move places the head only.
    CHECK(scene.particlesAlive(system) == 20, "%u sparkles along a metre", scene.particlesAlive(system));
}

QUENCH_TEST(renderer_particles_effects_fall_back) {
    using quench::render::detail::DeviceSupport;
    using quench::render::detail::selectEffects;
    const DeviceSupport full{.sampledDepth = true, .particles = true, .particleCompute = true};
    RenderSettings s;
    CHECK(selectEffects(s, full).particles == ParticleSimulation::Gpu, "on the GPU");
    DeviceSupport noCompute = full;
    noCompute.particleCompute = false;
    CHECK(selectEffects(s, noCompute).particles == ParticleSimulation::Cpu, "on the CPU without compute");
    DeviceSupport noDepth = full;
    noDepth.particles = false;
    CHECK(selectEffects(s, noDepth).particles == ParticleSimulation::Off, "none without a depth texture to sample");
    s.particles = ParticleSimulation::Cpu;
    CHECK(selectEffects(s, full).particles == ParticleSimulation::Cpu, "on the CPU when asked");
    s.particles = ParticleSimulation::Off;
    CHECK(selectEffects(s, full).particles == ParticleSimulation::Off, "off");
}

QUENCH_TEST(renderer_particle_slots_are_limited) {
    QUENCH_GPU_DEVICE(device);
    Scene scene(device);
    particles::ParticleDesc d;
    d.maxParticles = particles::kMaxSystemParticles;
    std::vector<ParticleSystemId> ids;
    for (std::uint32_t i = 0; i < particles::kMaxSceneParticles / particles::kMaxSystemParticles; ++i) {
        Result<ParticleSystemId> id = scene.addParticles(d);
        CHECK(id, "system %u", i);
        if (id) ids.push_back(*id);
    }
    CHECK(scene.particleSlots() == particles::kMaxSceneParticles, "%u slots", scene.particleSlots());
    d.maxParticles = 1;
    CHECK(!scene.addParticles(d), "no slot left");
    // A removed system's slots are given out again, and a bigger desc that does not fit keeps the old one.
    scene.removeParticles(ids[1]);
    d.maxParticles = 1000;
    Result<ParticleSystemId> small = scene.addParticles(d);
    d.maxParticles = particles::kMaxSystemParticles - 1000;
    Result<ParticleSystemId> rest = scene.addParticles(d);
    CHECK(small && rest, "two systems in the freed slots");
    CHECK(scene.particleSlots() == particles::kMaxSceneParticles, "full again: %u slots", scene.particleSlots());
    d.maxParticles = 2000;
    CHECK(small && !scene.setParticles(*small, d), "too big for what is left");
    CHECK(small && scene.particleDesc(*small)->maxParticles == 1000, "the old desc kept");
    CHECK(scene.particleSystemCount() == std::uint32_t(ids.size()) + 1, "%u systems", scene.particleSystemCount());
    scene.removeParticles(ids[0]);
    CHECK(scene.particleSlots() == particles::kMaxSceneParticles - particles::kMaxSystemParticles, "%u slots",
          scene.particleSlots());
}
