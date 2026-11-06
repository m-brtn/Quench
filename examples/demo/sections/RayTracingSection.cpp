// Ray tracing: the renderer's traced effects and MetalFX upscaling, where gpu::Caps offers them (the native Metal
// backend, --gpu-backend metal, on a GPU with ray queries in render passes): soft sun shadows, ambient occlusion,
// shadows of local lights, reflections, indirect light from a probe grid or per pixel, the engine's denoisers and
// MetalFX's denoising upscaler, and the reference path tracer. Elsewhere the same settings fall back to the cascaded
// shadow maps, no occlusion, unshadowed local lights, the sky's reflections and ambient light and bilinear scaling,
// and the section says why. The scene has what the rays must handle: a fence with holes (cut-out) lit by a spot
// behind it, a glass torus (translucent, tinted), a walking character (skinned, refitted every frame), a mirror, a
// strip of gold spheres from smooth to rough, a red and green corner with a white block and a lamp (colour bleeding,
// contact occlusion), a glowing panel (emissive light through the probes), under a turning sun.

#include "Demo.h"

#include <quench/animation/AnimationSystem.h>
#include <quench/assets/AssetManager.h>
#include <quench/render/SceneAssets.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;
using animation::AnimationSystem;

constexpr float kPi = std::numbers::pi_v<float>;
const glm::vec2 kWalkCenter(-2.9f, -0.6f);
constexpr float kWalkRadius = 1.3f; // m
constexpr float kWalkSpeed = 1.1f;  // m/s: the walk clip's speed
constexpr std::array<float, 5> kStripRoughness{0.02f, 0.12f, 0.22f, 0.34f, 0.48f};

// A 128 x 64 fence of diamond holes (alpha 0) between bars, sRGB.
std::vector<std::byte> fencePixels() {
    std::vector<std::byte> pixels(128 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 128; ++x) {
            const float cx = std::abs(float(x % 16) - 7.5f), cy = std::abs(float(y % 16) - 7.5f);
            const bool hole = cx + cy < 6.0f;
            std::byte* p = &pixels[std::size_t(y * 128 + x) * 4];
            p[0] = std::byte(170);
            p[1] = std::byte(120);
            p[2] = std::byte(80);
            p[3] = std::byte(hole ? 0 : 255);
        }
    return pixels;
}

class RayTracingSection final : public Section {
public:
    RayTracingSection() {
        camera.target = glm::vec3(-0.2f, 0.8f, -0.8f);
        camera.distance = 10.5f;
        camera.yaw = glm::radians(18.0f);
        camera.pitch = glm::radians(26.0f);
        camera.maxDistance = 60.0f;
        settings.sunShadows = SunShadows::RayTraced;
        settings.ambientOcclusion = AmbientOcclusion::RayTraced;
        settings.localLightShadows = 2;
        settings.reflections = Reflections::RayTraced;
        settings.indirectLight = IndirectLight::Probes;
    }

    std::string_view id() const override { return "raytracing"; }
    std::string_view title() const override { return "Ray tracing"; }
    std::string_view about() const override {
        return "Ray-traced sun and lamp shadows (soft, through the fence's holes, tinted by the glass, from the "
               "walking character), ambient occlusion, reflections (the mirror, the gold spheres from smooth to "
               "rough), indirect light (the red and green corner, the glowing panel), the engine's denoisers, "
               "MetalFX upscaling and denoising, and a reference path tracer: the native Metal backend on a GPU "
               "with ray queries. Elsewhere: cascaded shadow maps, no occlusion, unshadowed lamps, the sky's "
               "reflections and ambient light, bilinear scaling.";
    }

    Result<void> init(Context& c) override {
        // All of it costs more than a phone's frame at the phone's size (docs/performance.md): where MetalFX scales
        // up, the section renders at two thirds of the size with MetalFX's temporal upscaler and the engine's
        // denoiser. MetalFX's denoising upscaler is a choice in the panel: on the M4 Pro it costs about three times
        // as much at 1080p.
        if (c.app.device().caps().temporalUpscaling) {
            settings.upscaling = Upscaling::Temporal;
            settings.renderScale = 0.67f;
        }
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> ground = addMesh(s, planeMesh(glm::vec2(30.0f), 30.0f));
        const Result<MeshId> panel = addMesh(s, planeMesh(glm::vec2(1.6f, 0.8f)));
        const Result<MeshId> post = addMesh(s, boxMesh(glm::vec3(0.05f, 0.85f, 0.05f)));
        const Result<MeshId> torus = addMesh(s, torusMesh(0.65f, 0.2f, 64, 32));
        const Result<MeshId> column = addMesh(s, boxMesh(glm::vec3(0.3f, 1.4f, 0.3f)));
        const Result<MeshId> crate = addMesh(s, boxMesh(glm::vec3(0.45f)));
        const Result<MeshId> ball = addMesh(s, sphereMesh(0.3f, 24, 48));
        const Result<MeshId> mirror = addMesh(s, planeMesh(glm::vec2(0.8f, 1.1f)));
        const Result<MeshId> frame = addMesh(s, boxMesh(glm::vec3(0.88f, 1.18f, 0.04f)));
        const Result<MeshId> backWall = addMesh(s, boxMesh(glm::vec3(1.2f, 0.9f, 0.06f)));
        const Result<MeshId> sideWall = addMesh(s, boxMesh(glm::vec3(0.06f, 0.9f, 1.0f)));
        const Result<MeshId> block = addMesh(s, boxMesh(glm::vec3(0.35f)));
        const Result<MeshId> glow = addMesh(s, planeMesh(glm::vec2(0.4f, 0.6f)));
        for (const Result<MeshId>* m : {&ground, &panel, &post, &torus, &column, &crate, &ball, &mirror, &frame,
                                        &backWall, &sideWall, &block, &glow})
            if (!*m) return Error{m->error()};

        s.addInstance({.mesh = *ground,
                       .material = addCheckerMaterial(c, s, glm::vec3(0.6f, 0.58f, 0.54f),
                                                      glm::vec3(0.48f, 0.46f, 0.43f), 1.0f)});
        const std::vector<std::byte> fence = fencePixels();
        const Result<TextureId> fenceTexture =
            s.addTexture({.width = 128, .height = 64, .pixels = fence, .name = "fence"});
        if (!fenceTexture) return Error{fenceTexture.error()};
        MaterialDesc fenceMaterial{.roughness = 0.6f, .alphaCutoff = 0.5f, .doubleSided = true, .name = "fence"};
        fenceMaterial.textures[0] = *fenceTexture;
        const glm::mat4 upright = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        s.addInstance({.mesh = *panel,
                       .material = s.addMaterial(fenceMaterial),
                       .transform = placed({-0.8f, 0.85f, 2.2f}) * upright});
        const MaterialId wood = s.addMaterial({.baseColor = glm::vec4(0.4f, 0.27f, 0.16f, 1.0f), .roughness = 0.7f});
        for (const float x : {-2.45f, 0.85f})
            s.addInstance({.mesh = *post, .material = wood, .transform = placed({x, 0.85f, 2.2f})});
        // A spot behind the fence throws its holes forward onto the ground.
        s.addLight({.type = LightType::Spot,
                    .color = glm::vec3(0.75f, 0.85f, 1.0f),
                    .intensity = 20.0f,
                    .position = glm::vec3(-0.8f, 1.1f, 1.0f),
                    .direction = glm::normalize(glm::vec3(0.0f, -0.45f, 1.0f)),
                    .range = 8.0f,
                    .innerAngle = 0.55f,
                    .outerAngle = 0.85f,
                    .sourceRadius = 0.02f});

        m_torus = s.addInstance({.mesh = *torus,
                                 .material = s.addMaterial({.baseColor = glm::vec4(0.95f, 0.45f, 0.1f, 0.45f),
                                                            .roughness = 0.08f,
                                                            .blend = BlendMode::AlphaBlend,
                                                            .name = "amber glass"})});
        const MaterialId stone = s.addMaterial({.baseColor = glm::vec4(0.66f, 0.64f, 0.6f, 1.0f), .roughness = 0.8f});
        s.addInstance({.mesh = *column, .material = stone, .transform = placed({-4.8f, 1.4f, -2.6f})});
        s.addInstance({.mesh = *crate,
                       .material = s.addMaterial({.baseColor = glm::vec4(0.3f, 0.45f, 0.6f, 1.0f), .roughness = 0.6f}),
                       .transform = placed({-0.5f, 0.45f, -1.6f}, glm::vec3(1.0f), glm::radians(25.0f))});

        // The mirror in a dark frame, facing the scene.
        s.addInstance({.mesh = *frame, .material = wood, .transform = placed({-2.2f, 1.25f, -3.66f})});
        s.addInstance({.mesh = *mirror,
                       .material = s.addMaterial(
                           {.baseColor = glm::vec4(0.95f, 0.95f, 0.95f, 1.0f), .roughness = 0.03f, .metallic = 1.0f}),
                       .transform = placed({-2.2f, 1.25f, -3.61f}) * upright});
        // Gold from mirror-like to as rough as the traced reflections go (RenderSettings::reflectionRoughness).
        for (std::size_t i = 0; i < kStripRoughness.size(); ++i)
            s.addInstance({.mesh = *ball,
                           .material = s.addMaterial({.baseColor = glm::vec4(1.0f, 0.78f, 0.34f, 1.0f),
                                                      .roughness = kStripRoughness[i],
                                                      .metallic = 1.0f}),
                           .transform = placed({1.3f + 0.75f * float(i), 0.3f, 1.2f})});

        // The corner: a green back wall, a red side wall and a white block lit by a lamp in front of it.
        const auto matte = [&](glm::vec3 color) {
            return s.addMaterial({.baseColor = glm::vec4(color, 1.0f), .roughness = 0.9f});
        };
        s.addInstance(
            {.mesh = *backWall, .material = matte({0.1f, 0.55f, 0.12f}), .transform = placed({2.8f, 0.9f, -3.3f})});
        s.addInstance(
            {.mesh = *sideWall, .material = matte({0.7f, 0.08f, 0.06f}), .transform = placed({4.0f, 0.9f, -2.3f})});
        s.addInstance({.mesh = *block,
                       .material = matte({0.8f, 0.8f, 0.78f}),
                       .transform = placed({3.2f, 0.35f, -2.55f}, glm::vec3(1.0f), glm::radians(20.0f))});
        s.addLight({.type = LightType::Point,
                    .color = glm::vec3(1.0f, 0.78f, 0.5f),
                    .intensity = 18.0f,
                    .position = glm::vec3(2.4f, 1.3f, -1.6f),
                    .range = 6.0f,
                    .sourceRadius = 0.08f});

        // A warm glowing panel beside the walk: emissive light that reaches the rest through the indirect light.
        s.addInstance({.mesh = *glow,
                       .material = s.addMaterial({.baseColor = glm::vec4(0.05f, 0.05f, 0.05f, 1.0f),
                                                  .emissive = glm::vec3(5.0f, 3.0f, 1.2f),
                                                  .shadows = ShadowMode::None,
                                                  .name = "glow"}),
                       .transform = placed({-5.0f, 0.75f, -0.4f}) *
                                    glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
                       .castsShadows = false});

        m_sun = s.addLight({.type = LightType::Directional});
        applySun();

        // The character of the Animation section, on the section's own asset manager (as there).
        m_manager = std::make_unique<assets::AssetManager>(c.app.jobs(), c.app.assets().settings());
        m_assets.emplace(s, *m_manager);
        m_anim = std::make_unique<AnimationSystem>(*m_manager, c.app.jobs());
        m_model = m_assets->loadModel(mannequin::kModel);
        m_skeleton = m_anim->loadSkeleton(mannequin::kSkeleton);
        m_clips = m_anim->loadClips(mannequin::kClips);
        m_graph = m_anim->loadGraph(mannequin::kGraph);
        animation::CharacterDesc desc;
        desc.skeleton = m_skeleton;
        desc.clips.push_back(m_clips);
        desc.graph = m_graph;
        desc.transform = walkerTransform();
        m_character = m_anim->addCharacter(std::move(desc));
        m_anim->setFloat(m_character, "speed", kWalkSpeed);
        m_characterModel = m_assets->addModel(m_model, walkerTransform());
        animate();
        return {};
    }

    void update(Context& c, double frameDt) override {
        const float dt = std::min(float(frameDt), 0.1f);
        // The path tracer's mean starts again whenever something moves: while it runs, nothing does.
        const bool still = c.renderer.effects().pathTracer;
        if (m_turnSun && !still) {
            m_azimuth = std::fmod(m_azimuth + dt * 10.0f, 360.0f);
            applySun();
        }
        if (!still) m_spin += dt * 0.5f;
        animate();
        if (m_skin < 0 && m_model.ready() && !m_model->skins().empty())
            m_skin = m_anim->addSkin(m_character, m_model->skins()[0].joints, m_model->skins()[0].inverseBindMatrices);
        m_manager->update();
        m_assets->update();
        if (still && m_skin >= 0) return; // the pose and the place as they are
        m_anim->setFloat(m_character, "speed", m_walk ? kWalkSpeed : 0.0f);
        m_anim->update(dt);
        if (m_skin >= 0)
            m_assets->setJointPalette(m_characterModel, 0, m_anim->skinPalette(m_character, m_skin), m_teleport);
        m_assets->setTransform(m_characterModel, walkerTransform(), m_teleport);
        m_teleport = false;
        // On by the clips' root motion around the circle.
        const animation::RootMotion motion = m_anim->rootMotion(m_character);
        m_angle = std::fmod(m_angle + glm::length(glm::vec2(motion.translation.x, motion.translation.z)) / kWalkRadius,
                            2.0f * kPi);
        m_anim->setTransform(m_character, walkerTransform());
    }

    void controls(Context& c) override {
        const gpu::Caps& caps = c.app.device().caps();
        const ActiveEffects& e = c.renderer.effects();
        if (!caps.rayTracing) ImGui::TextWrapped("No ray tracing: %s", rayTracingWhyNot(caps));
        const auto fallback = [](bool active, const char* text) {
            if (!active) ImGui::TextDisabled("  fallback: %s", text);
        };

        int shadows = settings.sunShadows == SunShadows::RayTraced ? 1 : 0;
        if (ImGui::Combo("Sun shadows", &shadows,
                         "Shadow maps\0"
                         "Ray traced\0"))
            settings.sunShadows = shadows == 1 ? SunShadows::RayTraced : SunShadows::ShadowMaps;
        fallback(settings.sunShadows == e.sunShadows, "shadow maps");
        ImGui::SliderInt("Shadow rays", &settings.shadowRays, 1, 8);
        ImGui::SliderFloat("Softness", &settings.shadowSoftness, 0.0f, 6.0f, "%.1f x sun");
        bool occlusion = settings.ambientOcclusion == AmbientOcclusion::RayTraced;
        if (ImGui::Checkbox("Ambient occlusion", &occlusion))
            settings.ambientOcclusion = occlusion ? AmbientOcclusion::RayTraced : AmbientOcclusion::None;
        fallback(settings.ambientOcclusion == e.ambientOcclusion, "none");
        ImGui::SliderInt("Lamp shadows", &settings.localLightShadows, 0, kMaxRayLocalShadows);
        fallback(settings.localLightShadows == 0 || e.localLightShadows > 0, "unshadowed");
        bool reflections = settings.reflections == Reflections::RayTraced;
        if (ImGui::Checkbox("Traced reflections", &reflections))
            settings.reflections = reflections ? Reflections::RayTraced : Reflections::Sky;
        fallback(settings.reflections == e.reflections, "the sky's");
        int indirect = int(settings.indirectLight);
        if (ImGui::Combo("Indirect light", &indirect,
                         "Sky\0"
                         "Probe grid\0"
                         "Ray traced\0"))
            settings.indirectLight = IndirectLight(indirect);
        fallback(settings.indirectLight == e.indirectLight, "the sky's ambient light");
        int denoiser = int(settings.rayDenoiser);
        if (ImGui::Combo("Denoiser", &denoiser,
                         "Spatial\0"
                         "Temporal\0"))
            settings.rayDenoiser = RayDenoiser(denoiser);
        if (e.upscaling == Upscaling::TemporalDenoised) ImGui::TextDisabled("  MetalFX denoises (upscaling)");

        int upscaling = int(settings.upscaling);
        if (ImGui::Combo("Upscaling", &upscaling,
                         "Bilinear\0"
                         "MetalFX spatial\0"
                         "MetalFX temporal\0"
                         "MetalFX denoising\0"))
            settings.upscaling = Upscaling(upscaling);
        if (settings.upscaling != e.upscaling) ImGui::TextDisabled("  running: %s", upscalingName(e.upscaling));
        ImGui::SliderFloat("Render scale", &settings.renderScale, 0.5f, 1.0f, "%.2f");

        ImGui::Checkbox("Path tracer (reference)", &settings.pathTracer);
        fallback(!settings.pathTracer || e.pathTracer, "the real-time frame");
        if (e.pathTracer)
            ImGui::TextWrapped("%u samples; everything stands still while it runs", c.renderer.stats().pathSamples);
        ImGui::Checkbox("Turn the sun", &m_turnSun);
        ImGui::SameLine();
        ImGui::Checkbox("Walk", &m_walk);
        if (ImGui::SliderFloat("Sun elevation", &m_elevation, 5.0f, 85.0f, "%.0f deg")) applySun();
    }

    void stats(Context& c) override {
        const gpu::Caps& caps = c.app.device().caps();
        const ActiveEffects& e = c.renderer.effects();
        const RenderStats& r = c.renderer.stats();
        const bool traced = e.sunShadows == SunShadows::RayTraced ||
                            e.ambientOcclusion == AmbientOcclusion::RayTraced || e.localLightShadows > 0 ||
                            e.reflections == Reflections::RayTraced || e.indirectLight != IndirectLight::Sky ||
                            e.pathTracer;
        if (traced)
            ImGui::TextWrapped("Top level %u instances (%s); bottom levels built %u, refitted %u; %.1f MB",
                               r.rayInstances, r.rayTopRefit ? "refitted" : "built", r.rayBuilds, r.rayRefits,
                               double(r.rayMemoryBytes) / (1024.0 * 1024.0));
        else
            ImGui::TextWrapped("%u shadow draws in %d cascades", r.shadowDraws, e.shadowCascades);
        if (c.app.deterministic()) {
            ImGui::TextUnformatted("Times hidden (fixed clock)");
        } else if (!caps.gpuPassTiming) {
            ImGui::TextUnformatted("GPU pass times n/a on this backend");
        } else {
            c.app.requestPassTiming();
            // The renderer's ranges are inside the app's "game" range: what this section changes, side by side.
            double rays = 0.0, shadowRays = 0.0, denoise = 0.0, probes = 0.0, reflect = 0.0, indirect = 0.0, path = 0.0,
                   maps = 0.0, upscale = 0.0;
            for (const gpu::GpuTiming& t : c.app.stats().gpuPasses) {
                if (t.name == "accel" || t.name == "prepass" || t.name == "raytrace") rays += t.milliseconds;
                if (t.name == "rays") shadowRays += t.milliseconds;
                if (t.name == "denoise") denoise += t.milliseconds;
                if (t.name == "probes") probes += t.milliseconds;
                if (t.name == "reflections") reflect += t.milliseconds;
                if (t.name == "indirect") indirect += t.milliseconds;
                if (t.name == "path") path += t.milliseconds;
                if (t.name == "shadows") maps += t.milliseconds;
                if (t.name == "upscale") upscale += t.milliseconds;
            }
            if (traced && !e.pathTracer) {
                ImGui::Text("Rays GPU %.3f ms: shadows and AO %.3f, denoise %.3f", rays, shadowRays, denoise);
                ImGui::Text("  probes %.3f, reflections %.3f, indirect %.3f", probes, reflect, indirect);
            }
            if (e.pathTracer) ImGui::Text("Path tracer GPU %.3f ms", path);
            if (e.shadowCascades > 0) ImGui::Text("Shadow maps GPU %.3f ms", maps);
            if (e.upscaling != Upscaling::Bilinear) ImGui::Text("MetalFX GPU %.3f ms", upscale);
            ImGui::Text("Frame GPU %.3f ms", c.app.stats().gpuMs);
        }
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        if (name == "rt_ready") return m_skin >= 0 ? 1.0 : 0.0;
        if (name == "sun_azimuth") return double(m_azimuth);
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        (void)c;
        const std::optional<float> number = parseFloat(value);
        const std::optional<bool> flag = parseBool(value);
        if (name == "turn_sun" && flag) {
            m_turnSun = *flag;
        } else if (name == "walk" && flag) {
            m_walk = *flag;
        } else if (name == "sun_azimuth" && number) {
            m_azimuth = *number;
            applySun();
        } else if (name == "sun_elevation" && number) {
            m_elevation = std::clamp(*number, 5.0f, 85.0f);
            applySun();
        } else {
            return false;
        }
        return true;
    }

    std::string perfDetails() const override {
        return std::format("turn_sun={} walk={}", m_turnSun ? 1 : 0, m_walk ? 1 : 0);
    }

    void shutdown(Context& c) override {
        (void)c;
        if (m_characterModel) m_assets->removeModel(m_characterModel);
        m_model = {};
        m_skeleton = {};
        m_clips = {};
        m_graph = {};
        m_anim.reset();
        m_assets.reset();
        m_manager.reset();
        m_scene.reset();
    }

private:
    glm::mat4 walkerTransform() const {
        const glm::vec3 p(kWalkCenter.x + kWalkRadius * std::sin(m_angle), 0.0f,
                          kWalkCenter.y + kWalkRadius * std::cos(m_angle));
        return glm::rotate(glm::translate(glm::mat4(1.0f), p), m_angle + 0.5f * kPi, glm::vec3(0.0f, 1.0f, 0.0f));
    }

    void applySun() { setDaylight(*m_scene, m_sun, m_elevation, m_azimuth); }

    void animate() {
        glm::mat4 torus = glm::translate(glm::mat4(1.0f), glm::vec3(0.9f, 1.4f, -0.6f));
        torus = glm::rotate(torus, m_spin, glm::vec3(0.0f, 1.0f, 0.0f));
        torus = glm::rotate(torus, glm::radians(70.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        m_scene->setTransform(m_torus, torus);
    }

    std::optional<Scene> m_scene;
    InstanceId m_torus;
    LightId m_sun;
    std::unique_ptr<assets::AssetManager> m_manager;
    std::optional<SceneAssets> m_assets;
    std::unique_ptr<AnimationSystem> m_anim;
    assets::AssetRef<ModelAsset> m_model;
    assets::AssetRef<animation::SkeletonAsset> m_skeleton;
    assets::AssetRef<animation::ClipSetAsset> m_clips;
    assets::AssetRef<animation::GraphAsset> m_graph;
    animation::CharacterId m_character;
    ModelInstanceId m_characterModel;
    int m_skin = -1;
    float m_angle = 0.0f;
    float m_spin = 0.0f; // the torus's turn (rad)
    float m_elevation = 34.0f;
    float m_azimuth = 315.0f;
    bool m_turnSun = true, m_walk = true, m_teleport = true;
};

} // namespace

std::unique_ptr<Section> makeRayTracingSection() { return std::make_unique<RayTracingSection>(); }

} // namespace demo
