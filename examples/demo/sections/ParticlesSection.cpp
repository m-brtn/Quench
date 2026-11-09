// Particles and trails (docs/particles.md): a meadow at dusk under three light streams, flocks of glowing motes that
// weave around looping courses through the sky and leave tapering ribbons that shed twinkling sparkles; a field of
// twinkling points over the grass that grows denser as the sun goes down, fireflies, and a campfire's sparks, flames
// and smoke drifting in the weather's wind. The systems step on the GPU by compute where gpu::Caps allow it, else on
// the CPU; the streams and the field follow RenderSettings::particleDensity.

#include "Demo.h"

#include <quench/particles/Path.h>
#include <quench/render/Daylight.h>
#include <quench/weather/Weather.h>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;
using particles::Blend;
using particles::ColorCurve;
using particles::Facing;
using particles::ParticleDesc;
using particles::Shape;
using particles::Sprite;
using particles::TrailDesc;

constexpr float kTau = 2.0f * std::numbers::pi_v<float>;
const glm::vec3 kFire{-6.5f, 0.0f, -2.5f};
constexpr float kSunAzimuth = 310.0f; // degrees: ahead and to the left of the camera

// The air: no clouds, the wind given at 10 m (the weather's speed is at the cloud base, here 10 m), toward +x.
weather::Settings airSettings(float speed) {
    return {.coverage = 0.0f,
            .cirrus = 0.0f,
            .wind = {.direction = glm::vec2(1.0f, -0.3f), .speed = speed, .gusts = 0.4f, .turbulence = 0.3f},
            .cloudBase = 10.0f};
}

// A light stream: motes that follow one another around a closed course, weaving around it.
struct StreamLook {
    glm::vec3 color; // the ribbons' linear HDR color
    int heads;
    float speed; // m/s along the course
    float gap;   // m between two heads
    float braid; // m: how far the heads weave around the course
    std::uint32_t seed;
};

constexpr std::array<StreamLook, 3> kStreams{
    StreamLook{glm::vec3(2.6f, 1.8f, 0.9f), 8, 7.0f, 0.9f, 0.45f, 101}, // gold, a figure of eight across the sky
    StreamLook{glm::vec3(0.9f, 1.9f, 3.6f), 7, 8.0f, 1.1f, 0.55f, 202}, // blue, a wide ring with loops
    StreamLook{glm::vec3(3.0f, 1.0f, 2.3f), 5, 5.5f, 0.9f, 0.35f, 303}, // rose, low over the meadow
};

std::vector<glm::vec3> course(std::size_t which) {
    std::vector<glm::vec3> points;
    const int n = which == 1 ? 64 : 32;
    for (int i = 0; i < n; ++i) {
        const float a = kTau * float(i) / float(n);
        if (which == 0)
            points.emplace_back(13.0f * std::sin(a), 8.5f + 2.8f * std::sin(2.0f * a), -9.0f + 4.0f * std::cos(a));
        else if (which == 1)
            points.emplace_back(11.0f * std::cos(a) + 2.6f * std::cos(4.0f * a),
                                6.5f + 1.2f * std::sin(a) + 2.6f * std::sin(4.0f * a), -14.0f + 4.0f * std::sin(a));
        else
            points.emplace_back(7.5f * std::sin(a), 2.4f + 0.9f * std::sin(3.0f * a), -3.0f + 4.5f * std::cos(a));
    }
    return points;
}

// Fades in quickly, holds, fades out.
ColorCurve fadeInOut(float in, float out, float peak = 1.0f) {
    return {{0.0f, glm::vec4(peak, peak, peak, 0.0f)},
            {in, glm::vec4(peak, peak, peak, 1.0f)},
            {out, glm::vec4(1.0f)},
            {1.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f)}};
}

class ParticlesSection final : public Section {
public:
    ParticlesSection() {
        camera.target = glm::vec3(0.0f, 4.5f, -4.0f);
        camera.distance = 15.0f;
        camera.yaw = 0.0f;
        camera.pitch = glm::radians(-4.0f);
        camera.maxDistance = 60.0f;
        settings.bloomStrength = 0.15f;
        settings.shadowDistance = 60.0f;
    }

    std::string_view id() const override { return "particles"; }
    std::string_view title() const override { return "Particles"; }
    std::string_view about() const override {
        return "Light streams weave through the dusk sky and leave tapering ribbons that shed sparkles; twinkling "
               "points over the meadow (denser as the sun goes down), fireflies, and a campfire's sparks and smoke "
               "in the wind. Simulated on the GPU by compute, or on the CPU as the fallback.";
    }

    Result<void> init(Context& c) override {
        m_renderer = &c.renderer;
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> ground = addMesh(s, planeMesh(glm::vec2(300.0f), 75.0f));
        const Result<MeshId> box = addMesh(s, boxMesh(glm::vec3(0.5f)));
        const Result<MeshId> ball = addMesh(s, sphereMesh(1.0f, 16, 32));
        for (const Result<MeshId>* m : {&ground, &box, &ball})
            if (!*m) return Error{m->error()};
        s.addInstance({.mesh = *ground,
                       .material = addCheckerMaterial(c, s, glm::vec3(0.10f, 0.15f, 0.09f),
                                                      glm::vec3(0.08f, 0.13f, 0.08f), 4.0f, 0.95f)});
        // Standing stones, the hills on the horizon, the fire's ring of stones and its logs.
        const MaterialId stone = s.addMaterial({.baseColor = glm::vec4(0.32f, 0.31f, 0.3f, 1.0f), .roughness = 0.9f});
        const MaterialId hill = s.addMaterial({.baseColor = glm::vec4(0.08f, 0.11f, 0.08f, 1.0f), .roughness = 0.95f});
        const MaterialId wood = s.addMaterial({.baseColor = glm::vec4(0.12f, 0.07f, 0.04f, 1.0f), .roughness = 0.9f});
        Random random{0x9a27u};
        for (int i = 0; i < 6; ++i) {
            const float a = 0.5f + 0.42f * float(i);
            const float h = random.uniform(1.6f, 3.2f);
            const float width = random.uniform(0.6f, 0.9f);
            const glm::vec3 size(width, h, random.uniform(0.4f, 0.6f));
            s.addInstance({.mesh = *box,
                           .material = stone,
                           .transform = placed({5.0f + 6.0f * std::cos(a), h * 0.5f, -7.0f - 5.0f * std::sin(a)}, size,
                                               random.uniform(-0.6f, 0.6f))});
        }
        for (int i = 0; i < 9; ++i) {
            const float x = -70.0f + 17.0f * float(i) + random.uniform(-4.0f, 4.0f);
            const float r = random.uniform(10.0f, 22.0f);
            s.addInstance({.mesh = *ball,
                           .material = hill,
                           .transform = placed({x, -r * 0.55f, -70.0f - random.uniform(0.0f, 30.0f)},
                                               glm::vec3(r, r * 0.5f, r))});
        }
        for (int i = 0; i < 9; ++i) {
            const float a = kTau * float(i) / 9.0f;
            s.addInstance({.mesh = *ball,
                           .material = stone,
                           .transform = placed(kFire + glm::vec3(0.6f * std::cos(a), 0.06f, 0.6f * std::sin(a)),
                                               glm::vec3(0.2f, 0.14f, 0.17f), a)});
        }
        for (const float yaw : {0.5f, -0.6f})
            s.addInstance(
                {.mesh = *box,
                 .material = wood,
                 .transform = placed(kFire + glm::vec3(0.0f, 0.08f, 0.0f), glm::vec3(0.9f, 0.1f, 0.12f), yaw)});
        m_sun = s.addLight({.type = LightType::Directional});
        m_fireLight = s.addLight({.type = LightType::Point,
                                  .color = glm::vec3(1.0f, 0.45f, 0.16f),
                                  .intensity = 6.0f,
                                  .position = kFire + glm::vec3(0.0f, 0.5f, 0.0f),
                                  .range = 9.0f,
                                  .castsShadows = false});

        for (std::size_t i = 0; i < kStreams.size(); ++i) {
            Stream& stream = m_streams[i];
            const std::vector<glm::vec3> points = course(i);
            stream.path = particles::Path(points, true);
            Result<ParticleSystemId> sparkles = s.addParticles(sparklesDesc(kStreams[i]));
            Result<ParticleSystemId> dust = s.addParticles(dustDesc(kStreams[i], points));
            if (!sparkles) return Error{sparkles.error()};
            if (!dust) return Error{dust.error()};
            stream.sparkles = *sparkles;
            stream.dust = *dust;
            for (int k = 0; k < kStreams[i].heads; ++k) stream.heads.push_back(s.addTrail(trailDesc(kStreams[i])));
        }
        for (auto& [id, desc] : {std::pair{&m_field, fieldDesc()}, std::pair{&m_fireflies, firefliesDesc()},
                                 std::pair{&m_sparks, sparksDesc()}, std::pair{&m_flames, flamesDesc()},
                                 std::pair{&m_smoke, smokeDesc()}}) {
            Result<ParticleSystemId> system = s.addParticles(desc);
            if (!system) return Error{system.error()};
            *id = *system;
        }
        s.setParticleTransform(m_field, placed({0.0f, 0.55f, -6.0f}));
        s.setParticleTransform(m_fireflies, placed({-2.0f, 1.1f, 0.0f}));
        s.setParticleTransform(m_sparks, placed(kFire + glm::vec3(0.0f, 0.15f, 0.0f)));
        s.setParticleTransform(m_flames, placed(kFire + glm::vec3(0.0f, 0.1f, 0.0f)));
        s.setParticleTransform(m_smoke, placed(kFire + glm::vec3(0.0f, 0.7f, 0.0f)));
        applySun();
        applyShown();
        applySettings();
        moveStreams(true);
        return {};
    }

    void update(Context& c, double frameDt) override {
        (void)c;
        m_streamTime += frameDt * double(m_streamSpeed);
        m_air.update(frameDt);
        m_scene->setWeather(m_air);
        moveStreams(false);
        const double t = sectionTime;
        const float flicker = 0.8f + 0.12f * std::sin(float(t) * 17.0f) + 0.08f * std::sin(float(t) * 7.3f + 1.0f);
        m_scene->setLight(m_fireLight, {.type = LightType::Point,
                                        .color = glm::vec3(1.0f, 0.45f, 0.16f),
                                        .intensity = m_fire ? 6.0f * flicker : 0.0f,
                                        .position = kFire + glm::vec3(0.0f, 0.5f, 0.0f),
                                        .range = 9.0f,
                                        .castsShadows = false});
    }

    void controls(Context& c) override {
        (void)c;
        int simulation = m_simulation == ParticleSimulation::Gpu ? 0 : m_simulation == ParticleSimulation::Cpu ? 1 : 2;
        ImGui::SetNextItemWidth(itemWidth());
        if (ImGui::Combo("Simulation", &simulation, "GPU (compute)\0CPU\0Off\0")) {
            m_simulation = simulation == 0   ? ParticleSimulation::Gpu
                           : simulation == 1 ? ParticleSimulation::Cpu
                                             : ParticleSimulation::Off;
            applySettings();
        }
        ImGui::SetNextItemWidth(itemWidth());
        if (ImGui::SliderFloat("Density", &m_density, 0.1f, 1.0f, "%.2f")) applySettings();
        bool shown = false;
        shown |= ImGui::Checkbox("Light streams", &m_streamsOn);
        shown |= ImGui::Checkbox("Trail sparkles", &m_sparklesOn);
        shown |= ImGui::Checkbox("Sparkle field", &m_fieldOn);
        shown |= ImGui::Checkbox("Fireflies", &m_firefliesOn);
        shown |= ImGui::Checkbox("Campfire", &m_fire);
        if (shown) applyShown();
        ImGui::SetNextItemWidth(itemWidth());
        ImGui::SliderFloat("Stream speed", &m_streamSpeed, 0.0f, 2.0f, "x%.2f");
        ImGui::SetNextItemWidth(itemWidth());
        if (ImGui::SliderFloat("Trail length", &m_trailSeconds, 0.3f, 3.0f, "%.1f s")) applyTrails();
        ImGui::SetNextItemWidth(itemWidth());
        if (ImGui::SliderFloat("Dusk", &m_dusk, 0.0f, 1.0f, "%.2f")) applySun();
        ImGui::SetNextItemWidth(itemWidth());
        if (ImGui::SliderFloat("Wind at 10 m", &m_wind, 0.0f, 12.0f, "%.1f m/s"))
            m_air.setSettings(airSettings(m_wind));
        if (ImGui::Button("Burst of sparks")) m_scene->burstParticles(m_sparks, 150);
        ImGui::SameLine();
        if (ImGui::Button("Restart")) restart();
    }

    void stats(Context& c) override {
        const RenderStats& s = c.renderer.stats();
        const ParticleSimulation mode = c.renderer.effects().particles;
        ImGui::TextWrapped("Simulation: %s", mode == ParticleSimulation::Gpu   ? "GPU (compute)"
                                             : mode == ParticleSimulation::Cpu ? "CPU"
                                                                               : "off");
        ImGui::TextWrapped("%u systems, %u particles alive in %u slots, %u spawned this frame", s.particleSystems,
                           s.particles, s.particleSlots, s.particleSpawns);
        ImGui::TextWrapped("%u trails, %u strip vertices", s.trails, s.trailVertices);
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        const RenderStats& s = m_renderer->stats();
        if (name == "particles") return double(s.particles);
        if (name == "particle_systems") return double(s.particleSystems);
        if (name == "particle_slots") return double(s.particleSlots);
        if (name == "particle_spawns") return double(s.particleSpawns);
        if (name == "trails") return double(s.trails);
        if (name == "trail_vertices") return double(s.trailVertices);
        if (name == "simulation") return double(int(m_renderer->effects().particles));
        if (name == "scene_systems") return double(m_scene->particleSystemCount());
        if (name == "scene_slots") return double(m_scene->particleSlots());
        if (name == "scene_trails") return double(m_scene->trailCount());
        if (name == "stream_sparkles") {
            std::uint32_t n = 0;
            for (const Stream& stream : m_streams) n += m_scene->particlesAlive(stream.sparkles);
            return double(n);
        }
        if (name == "field_alive") return double(m_scene->particlesAlive(m_field));
        if (name == "fireflies_alive") return double(m_scene->particlesAlive(m_fireflies));
        if (name == "sparks_alive") return double(m_scene->particlesAlive(m_sparks));
        if (name == "smoke_alive") return double(m_scene->particlesAlive(m_smoke));
        if (name == "wind_at_fire") return double(glm::length(m_air.windAt(kFire + glm::vec3(0.0f, 1.0f, 0.0f))));
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        (void)c;
        const std::optional<float> number = parseFloat(value);
        const std::optional<bool> flag = parseBool(value);
        if (name == "simulation" && (value == "gpu" || value == "cpu" || value == "off")) {
            m_simulation = value == "gpu"   ? ParticleSimulation::Gpu
                           : value == "cpu" ? ParticleSimulation::Cpu
                                            : ParticleSimulation::Off;
            applySettings();
        } else if (name == "density" && number) {
            m_density = std::clamp(*number, 0.0f, 1.0f);
            applySettings();
        } else if ((name == "streams" || name == "sparkles" || name == "field" || name == "fireflies" ||
                    name == "fire") &&
                   flag) {
            (name == "streams"     ? m_streamsOn
             : name == "sparkles"  ? m_sparklesOn
             : name == "field"     ? m_fieldOn
             : name == "fireflies" ? m_firefliesOn
                                   : m_fire) = *flag;
            applyShown();
        } else if (name == "stream_speed" && number) {
            m_streamSpeed = std::max(*number, 0.0f);
        } else if (name == "trail_seconds" && number) {
            m_trailSeconds = std::clamp(*number, 0.1f, 10.0f);
            applyTrails();
        } else if (name == "dusk" && number) {
            m_dusk = std::clamp(*number, 0.0f, 1.0f);
            applySun();
        } else if (name == "wind" && number) {
            m_wind = std::max(*number, 0.0f);
            m_air.setSettings(airSettings(m_wind));
        } else if (name == "view" && (value == "wide" || value == "fire")) {
            if (value == "fire") {
                camera.target = kFire + glm::vec3(0.0f, 0.8f, 0.0f);
                camera.distance = 4.5f;
                camera.yaw = glm::radians(-60.0f);
                camera.pitch = glm::radians(8.0f);
            } else {
                camera = ParticlesSection().camera;
            }
            cameraCut = true;
        } else if (name == "burst" && number) {
            m_scene->burstParticles(m_sparks, std::uint32_t(std::max(*number, 0.0f)));
        } else if (name == "restart" && flag && *flag) {
            restart();
        } else {
            return false;
        }
        return true;
    }

    std::string perfDetails() const override {
        const RenderStats& s = m_renderer->stats();
        return std::format("particles={} trails={} trail_vertices={} simulation={}", s.particles, s.trails,
                           s.trailVertices, int(m_renderer->effects().particles));
    }

    void shutdown(Context& c) override {
        (void)c;
        m_scene.reset();
    }

private:
    struct Stream {
        particles::Path path;
        std::vector<TrailId> heads;
        ParticleSystemId sparkles;
        ParticleSystemId dust;
    };

    static ParticleDesc sparklesDesc(const StreamLook& look) {
        return {.maxParticles = 4096,
                .seed = look.seed,
                .rate = 0.0f,
                .lifetime = {0.8f, 2.0f},
                .speed = {0.02f, 0.2f},
                .spread = std::numbers::pi_v<float>,
                .inherit = 0.04f,
                .size = {0.03f, 0.06f},
                .gravity = glm::vec3(0.0f, -0.15f, 0.0f),
                .drag = 1.5f,
                .noise = 0.12f,
                .noiseScale = 2.0f,
                .color = look.color * 1.3f + glm::vec3(0.6f),
                .colorOverLife = fadeInOut(0.05f, 0.5f, 1.5f),
                .brightnessVariation = 0.6f,
                .twinkle = 0.7f,
                .twinkleRate = 3.0f,
                .twinkleSharpness = 4.0f,
                .softness = 0.1f,
                .nearFade = 0.4f,
                .minPixels = 1.5f};
    }

    // A faint dust of motes along the stream's course (a path emitter).
    static ParticleDesc dustDesc(const StreamLook& look, const std::vector<glm::vec3>& points) {
        return {.maxParticles = 512,
                .seed = look.seed + 1,
                .rate = 60.0f,
                .shape = Shape::Path,
                .extent = glm::vec3(0.9f),
                .path = points,
                .pathClosed = true,
                .lifetime = {2.0f, 4.0f},
                .speed = {0.0f, 0.1f},
                .spread = std::numbers::pi_v<float>,
                .size = {0.014f, 0.03f},
                .noise = 0.1f,
                .color = look.color * 0.7f,
                .colorOverLife = fadeInOut(0.2f, 0.7f),
                .brightnessVariation = 0.6f,
                .twinkle = 0.6f,
                .twinkleRate = 1.5f,
                .softness = 0.1f,
                .minPixels = 1.2f};
    }

    TrailDesc trailDesc(const StreamLook& look) const {
        return {.lifetime = m_trailSeconds,
                .spacing = 0.08f,
                .maxPoints = 512,
                .subdivisions = 4,
                .width = 0.09f,
                .widthOverAge = {{0.0f, 0.8f}, {0.06f, 1.0f}, {0.5f, 0.6f}, {1.0f, 0.0f}},
                .color = look.color,
                .colorOverAge = {{0.0f, glm::vec4(2.6f, 2.6f, 2.6f, 1.0f)},
                                 {0.1f, glm::vec4(1.6f, 1.6f, 1.6f, 1.0f)},
                                 {0.5f, glm::vec4(1.0f, 1.0f, 1.05f, 0.6f)},
                                 {1.0f, glm::vec4(0.8f, 0.8f, 0.9f, 0.0f)}},
                .headSize = 0.2f,
                .headColor = look.color * 1.5f + glm::vec3(2.0f),
                .softness = 0.2f,
                .minPixels = 1.5f};
    }

    // Twinkling points over the meadow.
    static ParticleDesc fieldDesc() {
        return {.maxParticles = 8192,
                .seed = 7,
                .rate = 1400.0f,
                .shape = Shape::Box,
                .extent = glm::vec3(20.0f, 0.45f, 12.0f),
                .lifetime = {2.5f, 5.0f},
                .speed = {0.0f, 0.06f},
                .spread = std::numbers::pi_v<float>,
                .size = {0.035f, 0.08f},
                .noise = 0.08f,
                .noiseScale = 3.0f,
                .color = glm::vec3(1.6f, 2.0f, 3.0f),
                .colorOverLife = fadeInOut(0.15f, 0.8f),
                .brightnessVariation = 0.7f,
                .twinkle = 0.6f,
                .twinkleRate = 0.8f,
                .twinkleSharpness = 2.5f,
                .softness = 0.15f,
                .minPixels = 1.3f};
    }

    static ParticleDesc firefliesDesc() {
        return {.maxParticles = 192,
                .seed = 9,
                .rate = 14.0f,
                .shape = Shape::Box,
                .extent = glm::vec3(9.0f, 0.8f, 6.0f),
                .lifetime = {6.0f, 10.0f},
                .speed = {0.05f, 0.2f},
                .spread = std::numbers::pi_v<float>,
                .size = {0.035f, 0.05f},
                .noise = 0.35f,
                .noiseScale = 2.5f,
                .noiseRate = 0.25f,
                .color = glm::vec3(2.6f, 3.6f, 0.7f),
                .colorOverLife = fadeInOut(0.1f, 0.85f),
                .twinkle = 1.0f,
                .twinkleRate = 0.35f,
                .twinkleSharpness = 3.0f,
                .softness = 0.2f};
    }

    static ParticleDesc sparksDesc() {
        return {.maxParticles = 1024,
                .seed = 11,
                .rate = 70.0f,
                .shape = Shape::Disc,
                .extent = glm::vec3(0.25f),
                .lifetime = {0.7f, 1.8f},
                .speed = {1.8f, 3.6f},
                .spread = 0.35f,
                .size = {0.012f, 0.022f},
                .gravity = glm::vec3(0.0f, -1.5f, 0.0f),
                .drag = 0.9f,
                .noise = 1.2f,
                .noiseScale = 1.0f,
                .noiseRate = 0.6f,
                .wind = 0.7f,
                .color = glm::vec3(9.0f, 3.2f, 0.7f),
                .colorOverLife = {{0.0f, glm::vec4(1.3f, 1.2f, 1.0f, 0.0f)},
                                  {0.05f, glm::vec4(1.3f, 1.2f, 1.0f, 1.0f)},
                                  {0.4f, glm::vec4(1.0f, 0.8f, 0.6f, 1.0f)},
                                  {1.0f, glm::vec4(0.6f, 0.25f, 0.1f, 0.0f)}},
                .twinkle = 0.4f,
                .twinkleRate = 8.0f,
                .facing = Facing::Velocity,
                .stretch = 0.06f,
                .softness = 0.05f,
                .minPixels = 1.2f};
    }

    static ParticleDesc flamesDesc() {
        return {.maxParticles = 128,
                .seed = 13,
                .rate = 40.0f,
                .shape = Shape::Disc,
                .extent = glm::vec3(0.2f),
                .lifetime = {0.35f, 0.7f},
                .speed = {0.6f, 1.2f},
                .spread = 0.25f,
                .size = {0.14f, 0.22f},
                .noise = 0.4f,
                .noiseScale = 0.5f,
                .color = glm::vec3(3.2f, 1.2f, 0.32f),
                .sizeOverLife = {{0.0f, 0.6f}, {0.3f, 1.0f}, {1.0f, 0.2f}},
                .colorOverLife = fadeInOut(0.15f, 0.5f),
                .sprite = Sprite::Soft,
                .softness = 0.15f};
    }

    static ParticleDesc smokeDesc() {
        return {.maxParticles = 128,
                .seed = 17,
                .rate = 7.0f,
                .lifetime = {4.0f, 7.0f},
                .speed = {0.5f, 0.9f},
                .spread = 0.2f,
                .size = {0.25f, 0.4f},
                .rotation = {0.0f, kTau},
                .spin = {-0.4f, 0.4f},
                .drag = 0.3f,
                .noise = 0.25f,
                .noiseScale = 3.0f,
                .wind = 1.0f,
                .color = glm::vec3(0.42f, 0.42f, 0.45f),
                .opacity = 0.3f,
                .sizeOverLife = {{0.0f, 0.5f}, {1.0f, 3.0f}},
                .colorOverLife = {{0.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f)},
                                  {0.12f, glm::vec4(1.0f, 1.0f, 1.0f, 0.8f)},
                                  {0.6f, glm::vec4(1.0f, 1.0f, 1.0f, 0.4f)},
                                  {1.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f)}},
                .blend = Blend::Alpha,
                .sprite = Sprite::Puff,
                .softness = 0.6f,
                .lit = true};
    }

    // Where head k of a stream is at stream time t: along the course, weaving around it.
    glm::vec3 headAt(std::size_t stream, int k, double t) const {
        const StreamLook& look = kStreams[stream];
        const particles::Path& path = m_streams[stream].path;
        const auto along = float(std::fmod(t * double(look.speed), double(path.length())) - double(k * look.gap));
        const glm::vec3 tangent = path.tangent(along);
        glm::vec3 side = glm::cross(tangent, glm::vec3(0.0f, 1.0f, 0.0f));
        side = glm::length(side) > 1e-3f ? glm::normalize(side) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 normal = glm::cross(side, tangent);
        const float phase = 2.39996f * float(k); // the golden angle: the heads spread evenly around the course
        const float w = 1.7f * float(t) + phase;
        return path.at(along) + look.braid * (side * std::sin(w) + normal * std::cos(0.8f * w + phase));
    }

    void moveStreams(bool jump) {
        for (std::size_t i = 0; i < m_streams.size(); ++i)
            for (int k = 0; k < int(m_streams[i].heads.size()); ++k)
                m_scene->moveTrail(m_streams[i].heads[std::size_t(k)], headAt(i, k, m_streamTime), sectionTime, jump);
    }

    void restart() {
        m_streamTime = 0.0;
        for (const Stream& stream : m_streams) {
            for (const TrailId head : stream.heads) m_scene->clearTrail(head);
            m_scene->restartParticles(stream.sparkles);
            m_scene->restartParticles(stream.dust);
        }
        for (const ParticleSystemId id : {m_field, m_fireflies, m_sparks, m_flames, m_smoke})
            m_scene->restartParticles(id);
        m_air.setTime(0.0);
        moveStreams(true);
    }

    void applySettings() {
        settings.particles = m_simulation;
        settings.particleDensity = m_density;
    }

    void applyShown() {
        Scene& s = *m_scene;
        for (const Stream& stream : m_streams) {
            for (const TrailId head : stream.heads) {
                s.setTrailVisible(head, m_streamsOn);
                s.setTrailSparkles(head, stream.sparkles, m_streamsOn && m_sparklesOn ? 6.0f : 0.0f);
            }
            s.setParticlesVisible(stream.sparkles, m_streamsOn);
            s.setParticlesVisible(stream.dust, m_streamsOn);
            s.setParticlesEmitting(stream.dust, m_streamsOn);
        }
        s.setParticlesVisible(m_field, m_fieldOn);
        s.setParticlesEmitting(m_field, m_fieldOn);
        s.setParticlesVisible(m_fireflies, m_firefliesOn);
        s.setParticlesEmitting(m_fireflies, m_firefliesOn);
        for (const ParticleSystemId id : {m_sparks, m_flames, m_smoke}) {
            s.setParticlesVisible(id, m_fire);
            s.setParticlesEmitting(id, m_fire);
        }
    }

    void applyTrails() {
        for (std::size_t i = 0; i < m_streams.size(); ++i)
            for (const TrailId head : m_streams[i].heads) m_scene->setTrail(head, trailDesc(kStreams[i]));
    }

    // From the golden hour (dusk 0: the daylight palette with the sun 10 degrees up) to the blue hour (1: the sun
    // 5 degrees under the horizon, a blue gradient sky with a warm glow where it set, haze over the meadow). The field
    // over the meadow thickens as the light goes: 0.3 of its rate at 0, all of it at 1.
    void applySun() {
        const float t = m_dusk;
        const glm::vec3 towardSun = skyDirection(10.0f - 15.0f * t, kSunAzimuth);
        const Daylight day = daylightFor(skyDirection(10.0f, kSunAzimuth));
        const Environment& a = day.environment;
        Environment b = daylightFor(skyDirection(-5.0f, kSunAzimuth)).environment;
        b.atmosphere = 0.0f;
        b.skyZenith = glm::vec3(0.020f, 0.045f, 0.160f);
        b.skyHorizon = glm::vec3(0.130f, 0.210f, 0.460f);
        b.skyGround = glm::vec3(0.030f, 0.040f, 0.070f);
        b.sunGlow = glm::vec3(0.30f, 0.16f, 0.10f);
        b.skyAmbient = glm::vec3(0.040f, 0.060f, 0.130f);
        b.groundAmbient = glm::vec3(0.015f, 0.015f, 0.020f);
        b.fogDensity = 0.012f;
        b.fogFalloff = 0.12f;
        b.fogMaxAmount = 0.8f;
        b.fogSunScatter = 0.3f;
        b.fogColor = glm::vec3(0.090f, 0.140f, 0.300f);
        b.exposure = 1.9f;
        const auto mix = [t](const auto& x, const auto& y) { return x + (y - x) * t; };
        const Environment e{.skyZenith = mix(a.skyZenith, b.skyZenith),
                            .skyHorizon = mix(a.skyHorizon, b.skyHorizon),
                            .skyGround = mix(a.skyGround, b.skyGround),
                            .sunGlow = mix(a.sunGlow, b.sunGlow),
                            .atmosphere = mix(a.atmosphere, b.atmosphere),
                            .sunDisc = mix(a.sunDisc, b.sunDisc),
                            .sunAngularRadius = mix(a.sunAngularRadius, b.sunAngularRadius),
                            .skyAmbient = mix(a.skyAmbient, b.skyAmbient),
                            .groundAmbient = mix(a.groundAmbient, b.groundAmbient),
                            .fogDensity = mix(a.fogDensity, b.fogDensity),
                            .fogFalloff = mix(a.fogFalloff, b.fogFalloff),
                            .fogBaseHeight = 0.0f,
                            .fogMaxAmount = mix(a.fogMaxAmount, b.fogMaxAmount),
                            .fogSunScatter = mix(a.fogSunScatter, b.fogSunScatter),
                            .fogColor = mix(a.fogColor, b.fogColor),
                            .exposure = mix(a.exposure, b.exposure)};
        m_scene->setEnvironment(e);
        m_scene->setLight(
            m_sun, {.type = LightType::Directional, .color = day.sunColor * (1.0f - t), .direction = -towardSun});
        ParticleDesc field = fieldDesc();
        field.rate *= 0.3f + 0.7f * t;
        (void)m_scene->setParticles(m_field, field);
    }

    const Renderer* m_renderer = nullptr;
    std::optional<Scene> m_scene;
    LightId m_sun, m_fireLight;
    std::array<Stream, kStreams.size()> m_streams;
    ParticleSystemId m_field, m_fireflies, m_sparks, m_flames, m_smoke;
    weather::Weather m_air{airSettings(3.0f)};
    double m_streamTime = 0.0;

    ParticleSimulation m_simulation = ParticleSimulation::Gpu;
    float m_density = 1.0f;
    bool m_streamsOn = true;
    bool m_sparklesOn = true;
    bool m_fieldOn = true;
    bool m_firefliesOn = true;
    bool m_fire = true;
    float m_streamSpeed = 1.0f;
    float m_trailSeconds = 2.2f;
    float m_dusk = 1.0f;
    float m_wind = 3.0f;
};

} // namespace

std::unique_ptr<Section> makeParticlesSection() { return std::make_unique<ParticlesSection>(); }

} // namespace demo
