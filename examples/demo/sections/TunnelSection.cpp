// Tunnel: area lights and anisotropic GGX. A closed tunnel at night lit only by its lamps: cyan strips in the floor,
// warm squares in the ceiling, a magenta panel on a wall, neon tubes, a round blue portal at the end (the eight area
// lights a frame takes) and two spots. Metal wall panels with normal, metallic and roughness maps, wet asphalt with a
// near-mirror puddle, a rubber mat and wheel stops, embossed plastic crates and brushed aluminium (anisotropic: the
// kick plates, the ribs and a sphere). The textures are written at build time (tools/assets/make_demo_tunnel.py) and
// cooked into KTX2. Where gpu::Caps offer ray tracing (the native Metal backend) the lamps cast soft traced shadows,
// the puddle and the metal reflect the tunnel and the path tracer is a toggle; elsewhere the lamps light and shine
// (raster LTC shading, diffuse and specular) without shadows and the reflections are the night sky's.

#include "Demo.h"

#include <quench/assets/AssetManager.h>
#include <quench/render/SceneAssets.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
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

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kHalfWidth = 3.0f;  // m; the vertical walls at x = -3 and 3
constexpr float kWallHeight = 3.0f; // then 45-degree chamfers up to the ceiling
constexpr float kHeight = 4.0f;
constexpr float kNear = 3.0f, kFar = -24.0f; // z of the open end and of the end wall
constexpr float kMid = 0.5f * (kNear + kFar);
constexpr float kHalfLength = 0.5f * (kNear - kFar);
constexpr float kPanelMeters = 3.0f; // one repeat of the panel texture (2 x 2 panels)
constexpr std::array<float, 5> kRibs{0.0f, -4.5f, -9.0f, -13.5f, -18.0f};
constexpr std::array<float, 2> kSquares{-7.0f, -16.0f}; // z of the ceiling squares

// The section's textures, cooked from tools/assets/make_demo_tunnel.py's files.
enum Texture : std::size_t {
    PanelColor,
    PanelNormal,
    PanelMr,
    AsphaltColor,
    AsphaltNormal,
    AsphaltMr,
    Puddle,
    PlasticNormal,
    RubberNormal,
    BrushedMr,
    TextureCount,
};
constexpr std::array<const char*, TextureCount> kTextures{
    "tunnel/panel_color.ktx2",    "tunnel/panel_normal.ktx2", "tunnel/panel_mr.ktx2", "tunnel/asphalt_color.ktx2",
    "tunnel/asphalt_normal.ktx2", "tunnel/asphalt_mr.ktx2",   "tunnel/puddle.ktx2",   "tunnel/plastic_normal.ktx2",
    "tunnel/rubber_normal.ktx2",  "tunnel/brushed_mr.ktx2",
};

// The lamps in groups the panel turns on and off.
enum Group : std::size_t { Strips, Squares, Panel, Tubes, Portal, Spots, GroupCount };
constexpr std::array<const char*, GroupCount> kGroupNames{"Floor strips", "Ceiling squares", "Wall panel",
                                                          "Neon tubes",   "Portal",          "Spots"};
constexpr std::array<const char*, GroupCount> kGroupKeys{"strips", "squares", "panel", "tubes", "portal", "spots"};

// A quad in the xz plane facing +y, `half` along x and z, its uv repeating `repeats` times along each.
MeshData quad(glm::vec2 half, glm::vec2 repeats) {
    MeshData mesh;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i)
            mesh.vertices.push_back({{(float(i) * 2.0f - 1.0f) * half.x, 0.0f, (float(j) * 2.0f - 1.0f) * half.y},
                                     {0.0f, 1.0f, 0.0f},
                                     {float(i) * repeats.x, float(j) * repeats.y},
                                     glm::vec4(1.0f),
                                     glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)});
    mesh.indices = {0, 2, 1, 1, 2, 3};
    return mesh;
}

// Places a quad (or any mesh made facing +y) at `center`, its x (the texture's u) along `u`, facing `normal`.
glm::mat4 frame(glm::vec3 center, glm::vec3 u, glm::vec3 normal) {
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(u, 0.0f);
    m[1] = glm::vec4(normal, 0.0f);
    m[2] = glm::vec4(glm::cross(u, normal), 0.0f);
    m[3] = glm::vec4(center, 1.0f);
    return m;
}

glm::mat4 turned(glm::vec3 position, float angle, glm::vec3 axis) {
    return glm::rotate(glm::translate(glm::mat4(1.0f), position), angle, axis);
}

class TunnelSection final : public Section {
public:
    TunnelSection() {
        camera.target = glm::vec3(0.0f, 1.1f, -7.5f);
        camera.distance = 9.5f;
        camera.yaw = 0.0f;
        camera.pitch = glm::radians(4.0f);
        camera.minDistance = 1.5f;
        camera.maxDistance = 10.0f;
        settings.localLightShadows = kMaxRayLocalShadows;
        settings.reflections = Reflections::RayTraced;
        settings.indirectLight = IndirectLight::Probes;
        settings.bloomStrength = 0.06f;
    }

    std::string_view id() const override { return "tunnel"; }
    std::string_view title() const override { return "Tunnel"; }
    std::string_view about() const override {
        return "Area lights (floor strips, ceiling squares, a wall panel, neon tubes, a round portal) and two spots in "
               "a tunnel of metal panels, wet asphalt with a puddle, rubber, embossed plastic and brushed aluminium "
               "(anisotropic). With ray tracing: soft lamp shadows, traced reflections and a path tracer. Elsewhere "
               "the lamps light and shine without shadows and reflect only the night sky.";
    }

    Result<void> init(Context& c) override {
        // As in the ray tracing section (docs/performance.md, "The tunnel"): two thirds of the size with MetalFX's
        // temporal upscaler where it runs; phones that trace at half the size with MetalFX's spatial upscaler,
        // without the probe grid or traced reflections (4.6 ms of the iPhone 17's 12.7 at 60 Hz) and two shadowed
        // lamps. Phones run at 60 frames per second also without ray tracing: eight area lights at the iPhone 17's
        // full size take about 12 ms.
        const gpu::Caps& caps = c.app.device().caps();
        m_limit60 = c.app.mobile();
        if (c.app.mobile() && caps.rayTracing) {
            settings.indirectLight = IndirectLight::Sky;
            settings.reflections = Reflections::Sky;
            settings.localLightShadows = 2;
            settings.renderScale = 0.5f;
            if (caps.spatialUpscaling) settings.upscaling = Upscaling::Spatial;
        } else if (caps.temporalUpscaling) {
            settings.upscaling = Upscaling::Temporal;
            settings.renderScale = 0.67f;
        }

        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        Environment night;
        night.skyZenith = glm::vec3(0.004f, 0.005f, 0.010f);
        night.skyHorizon = glm::vec3(0.008f, 0.009f, 0.014f);
        night.skyGround = glm::vec3(0.003f);
        night.sunGlow = night.sunDisc = glm::vec3(0.0f);
        night.atmosphere = 0.0f;
        night.skyAmbient = glm::vec3(0.008f, 0.010f, 0.016f);
        night.groundAmbient = glm::vec3(0.004f);
        night.fogDensity = 0.015f;
        night.fogFalloff = 0.2f;
        night.fogColor = glm::vec3(0.012f, 0.016f, 0.03f);
        night.fogSunScatter = 0.0f;
        night.exposure = 1.25f;
        s.setEnvironment(night);

        m_manager = std::make_unique<assets::AssetManager>(c.app.jobs(), c.app.assets().settings());
        m_assets.emplace(s, *m_manager);
        for (std::size_t i = 0; i < TextureCount; ++i) m_textures[i] = m_assets->loadTexture(kTextures[i]);
        const auto texture = [&](Texture t) { return m_textures[t]->texture(); };

        const float wallRepeats = 2.0f * kHalfLength / kPanelMeters;
        const float chamfer = std::sqrt(2.0f) * (kHeight - kWallHeight);
        const Result<MeshId> floor = addMesh(s, quad({kHalfWidth, kHalfLength}, {kHalfWidth, kHalfLength}));
        const Result<MeshId> wall = addMesh(s, quad({kHalfLength, 0.5f * kWallHeight}, {wallRepeats, 1.0f}));
        const Result<MeshId> slope =
            addMesh(s, quad({kHalfLength, 0.5f * chamfer}, {wallRepeats, chamfer / kPanelMeters}));
        const float ceilingHalf = kHalfWidth - (kHeight - kWallHeight);
        const Result<MeshId> ceiling =
            addMesh(s, quad({kHalfLength, ceilingHalf}, {wallRepeats, 2.0f * ceilingHalf / kPanelMeters}));
        const Result<MeshId> end = addMesh(s, quad({kHalfWidth, 0.5f * kHeight}, {2.0f, kHeight / kPanelMeters}));
        const Result<MeshId> kick = addMesh(s, quad({kHalfLength, 0.35f}, {2.0f * kHalfLength / 1.5f, 0.45f}));
        const Result<MeshId> pillar = addMesh(s, boxMesh(glm::vec3(0.07f, 0.5f * kWallHeight, 0.14f)));
        const Result<MeshId> brace = addMesh(s, boxMesh(glm::vec3(0.07f, 0.5f * chamfer, 0.14f)));
        const Result<MeshId> beam = addMesh(s, boxMesh(glm::vec3(ceilingHalf, 0.07f, 0.14f)));
        const Result<MeshId> crate = addMesh(s, boxMesh(glm::vec3(0.35f)));
        const Result<MeshId> sphere = addMesh(s, sphereMesh(0.55f, 48, 96));
        const Result<MeshId> plinth = addMesh(s, boxMesh(glm::vec3(0.45f, 0.15f, 0.45f)));
        const Result<MeshId> mat = addMesh(s, quad({0.8f, 1.4f}, {2.0f, 3.5f}));
        const Result<MeshId> stop = addMesh(s, boxMesh(glm::vec3(0.7f, 0.07f, 0.12f)));
        const Result<MeshId> puddle = addMesh(s, quad({1.6f, 1.1f}, {1.0f, 1.0f}));
        const Result<MeshId> housing = addMesh(s, boxMesh(glm::vec3(0.62f, 0.025f, 0.62f)));
        for (const Result<MeshId>* m : {&floor, &wall, &slope, &ceiling, &end, &kick, &pillar, &brace, &beam, &crate,
                                        &sphere, &plinth, &mat, &stop, &puddle, &housing})
            if (!*m) return Error{m->error()};

        MaterialDesc panels{.roughness = 1.0f, .metallic = 1.0f, .name = "metal panels"};
        panels.textures = {texture(PanelColor), texture(PanelMr), texture(PanelNormal), TextureId{}};
        const MaterialId metal = s.addMaterial(panels);
        MaterialDesc asphalt{.roughness = 1.0f, .metallic = 1.0f, .name = "wet asphalt"};
        asphalt.textures = {texture(AsphaltColor), texture(AsphaltMr), texture(AsphaltNormal), TextureId{}};
        MaterialDesc water{.roughness = 0.02f, .alphaCutoff = 0.5f, .name = "puddle"};
        water.textures[0] = texture(Puddle);
        MaterialDesc plastic{.baseColor = glm::vec4(0.85f, 0.32f, 0.05f, 1.0f), .roughness = 0.42f, .name = "plastic"};
        plastic.textures[2] = texture(PlasticNormal);
        MaterialDesc whitePlastic = plastic;
        whitePlastic.baseColor = glm::vec4(0.8f, 0.8f, 0.77f, 1.0f);
        MaterialDesc rubber{.baseColor = glm::vec4(0.025f, 0.025f, 0.028f, 1.0f), .roughness = 0.88f, .name = "rubber"};
        rubber.textures[2] = texture(RubberNormal);
        // Brushed aluminium: the highlight stretches across the brushing (the texture's streaks run along u).
        m_aluminium = MaterialDesc{.baseColor = glm::vec4(0.91f, 0.92f, 0.92f, 1.0f),
                                   .roughness = 1.0f,
                                   .metallic = 1.0f,
                                   .name = "brushed aluminium",
                                   .anisotropy = m_anisotropy,
                                   .anisotropyRotation = 0.5f * kPi};
        m_aluminium.textures[1] = texture(BrushedMr);
        m_aluminiumMaterial = s.addMaterial(m_aluminium);
        const MaterialId dark = s.addMaterial({.baseColor = glm::vec4(0.05f, 0.05f, 0.055f, 1.0f), .roughness = 0.5f});

        const glm::vec3 x(1.0f, 0.0f, 0.0f), y(0.0f, 1.0f, 0.0f), z(0.0f, 0.0f, 1.0f);
        const float r2 = std::sqrt(0.5f);
        s.addInstance({.mesh = *floor, .material = s.addMaterial(asphalt), .transform = frame({0, 0, kMid}, x, y)});
        s.addInstance(
            {.mesh = *wall, .material = metal, .transform = frame({-kHalfWidth, 0.5f * kWallHeight, kMid}, -z, x)});
        s.addInstance(
            {.mesh = *wall, .material = metal, .transform = frame({kHalfWidth, 0.5f * kWallHeight, kMid}, z, -x)});
        const float slopeX = kHalfWidth - 0.5f * (kHeight - kWallHeight), slopeY = 0.5f * (kWallHeight + kHeight);
        s.addInstance({.mesh = *slope,
                       .material = metal,
                       .transform = frame({-slopeX, slopeY, kMid}, -z, glm::vec3(r2, -r2, 0.0f))});
        s.addInstance({.mesh = *slope,
                       .material = metal,
                       .transform = frame({slopeX, slopeY, kMid}, z, glm::vec3(-r2, -r2, 0.0f))});
        s.addInstance({.mesh = *ceiling, .material = metal, .transform = frame({0, kHeight, kMid}, -z, -y)});
        s.addInstance({.mesh = *end, .material = metal, .transform = frame({0, 0.5f * kHeight, kFar}, x, z)});

        // Brushed aluminium: kick plates along both walls, ribs every 4.5 m and a sphere on a plinth.
        s.addInstance({.mesh = *kick,
                       .material = m_aluminiumMaterial,
                       .transform = frame({-kHalfWidth + 0.01f, 0.35f, kMid}, -z, x)});
        s.addInstance({.mesh = *kick,
                       .material = m_aluminiumMaterial,
                       .transform = frame({kHalfWidth - 0.01f, 0.35f, kMid}, z, -x)});
        for (const float rz : kRibs) {
            for (const float side : {-1.0f, 1.0f}) {
                s.addInstance({.mesh = *pillar,
                               .material = m_aluminiumMaterial,
                               .transform = placed({side * (kHalfWidth - 0.07f), 0.5f * kWallHeight, rz})});
                s.addInstance(
                    {.mesh = *brace,
                     .material = m_aluminiumMaterial,
                     .transform = turned({side * (slopeX - 0.05f), slopeY - 0.05f, rz}, -side * 0.25f * kPi, z)});
            }
            s.addInstance(
                {.mesh = *beam, .material = m_aluminiumMaterial, .transform = placed({0, kHeight - 0.07f, rz})});
        }
        s.addInstance({.mesh = *plinth, .material = dark, .transform = placed({1.4f, 0.15f, -12.0f})});
        s.addInstance({.mesh = *sphere, .material = m_aluminiumMaterial, .transform = placed({1.4f, 0.85f, -12.0f})});

        // Embossed plastic crates by the left wall, rubber on the right.
        const MaterialId orange = s.addMaterial(plastic), white = s.addMaterial(whitePlastic);
        s.addInstance({.mesh = *crate, .material = orange, .transform = turned({-2.05f, 0.35f, -6.2f}, 0.18f, y)});
        s.addInstance({.mesh = *crate, .material = orange, .transform = turned({-1.95f, 0.35f, -7.05f}, -0.12f, y)});
        s.addInstance({.mesh = *crate, .material = white, .transform = turned({-2.0f, 1.05f, -6.6f}, 0.45f, y)});
        const MaterialId black = s.addMaterial(rubber);
        s.addInstance({.mesh = *mat, .material = black, .transform = frame({1.7f, 0.006f, -4.5f}, x, y)});
        for (const float sz : {-15.0f, -16.6f, -18.2f})
            s.addInstance({.mesh = *stop, .material = black, .transform = placed({1.9f, 0.07f, sz})});
        // Where the far ceiling square and the violet tube show in it from the section's camera.
        m_puddle = s.addInstance(
            {.mesh = *puddle, .material = s.addMaterial(water), .transform = turned({-0.3f, 0.003f, -3.9f}, 0.35f, y)});

        // The lamps. Radiance (color times intensity) as an emissive surface: the shapes show it.
        const auto add = [&](Group g, const LightDesc& desc) { m_lights[g].push_back({desc, {}}); };
        for (const float side : {-1.0f, 1.0f})
            add(Strips, {.type = LightType::Rect,
                         .color = glm::vec3(0.15f, 0.85f, 1.0f),
                         .intensity = 7.0f,
                         .position = {side * 2.55f, 0.004f, kMid - 0.5f},
                         .direction = y,
                         .range = 9.0f,
                         .size = {2.0f * kHalfLength - 1.0f, 0.06f},
                         .tangent = z});
        for (const float sz : kSquares) {
            s.addInstance({.mesh = *housing, .material = dark, .transform = placed({0, kHeight - 0.025f, sz})});
            add(Squares, {.type = LightType::Rect,
                          .color = glm::vec3(1.0f, 0.86f, 0.72f),
                          .intensity = 10.0f,
                          .position = {0, kHeight - 0.055f, sz},
                          .direction = -y,
                          .range = 12.0f,
                          .size = {1.1f, 1.1f}});
        }
        add(Panel, {.type = LightType::Rect,
                    .color = glm::vec3(1.0f, 0.18f, 0.62f),
                    .intensity = 6.0f,
                    .position = {-kHalfWidth + 0.02f, 1.6f, -10.5f},
                    .direction = x,
                    .range = 9.0f,
                    .size = {1.2f, 1.6f},
                    .tangent = z});
        add(Tubes, {.type = LightType::Tube,
                    .color = glm::vec3(1.0f, 0.38f, 0.08f),
                    .intensity = 24.0f,
                    .position = {kHalfWidth - 0.12f, 1.6f, -7.0f},
                    .range = 8.0f,
                    .size = {2.2f, 0.035f},
                    .tangent = y});
        add(Tubes, {.type = LightType::Tube,
                    .color = glm::vec3(0.6f, 0.25f, 1.0f),
                    .intensity = 24.0f,
                    .position = {0, kHeight - 0.4f, -20.0f},
                    .range = 9.0f,
                    .size = {3.6f, 0.03f},
                    .tangent = x});
        add(Portal, {.type = LightType::Disk,
                     .color = glm::vec3(0.25f, 0.55f, 1.0f),
                     .intensity = 2.5f,
                     .position = {0, 1.9f, kFar + 0.03f},
                     .direction = z,
                     .range = 14.0f,
                     .size = {1.3f, 0.0f}});
        add(Spots, {.type = LightType::Spot,
                    .color = glm::vec3(1.0f, 0.7f, 0.3f),
                    .intensity = 30.0f,
                    .position = {-1.6f, kHeight - 0.15f, -5.8f},
                    .direction = glm::normalize(glm::vec3(-0.15f, -1.0f, -0.3f)),
                    .range = 7.0f,
                    .innerAngle = 0.25f,
                    .outerAngle = 0.45f,
                    .sourceRadius = 0.03f});
        add(Spots, spotOnSphere());
        for (std::size_t g = 0; g < GroupCount; ++g) applyGroup(g);
        return {};
    }

    void enter(Context& c) override {
        m_outsideRate = c.app.maxFrameRate();
        applyFrameLimit(c);
    }

    void leave(Context& c) override {
        if (c.app.maxFrameRate() != m_outsideRate) c.app.setMaxFrameRate(m_outsideRate);
    }

    void update(Context& c, double frameDt) override {
        m_manager->update();
        m_assets->update();
        // The path tracer's mean starts again whenever something moves: while it runs, nothing does.
        if (m_animate && !c.renderer.effects().pathTracer) {
            m_sweep += float(std::min(frameDt, 0.1)) * 0.6f;
            if (m_on[Spots] && m_lights[Spots].size() == 2 && m_lights[Spots][1].id)
                m_scene->setLight(m_lights[Spots][1].id, spotOnSphere());
        }
    }

    void controls(Context& c) override {
        const gpu::Caps& caps = c.app.device().caps();
        const ActiveEffects& e = c.renderer.effects();
        if (!caps.rayTracing)
            ImGui::TextWrapped("No ray tracing: %s The lamps light and shine without shadows; reflections are the "
                               "night sky's.",
                               rayTracingWhyNot(caps));
        int mode = settings.pathTracer ? 1 : 0;
        ImGui::TextUnformatted("Rendering");
        ImGui::SameLine();
        if (ImGui::RadioButton("Ray traced", &mode, 0)) settings.pathTracer = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Path traced", &mode, 1)) settings.pathTracer = true;
        if (settings.pathTracer && !e.pathTracer) ImGui::TextDisabled("  fallback: the real-time frame");
        if (e.pathTracer)
            ImGui::TextWrapped("%u samples; everything stands still while it runs", c.renderer.stats().pathSamples);
        ImGui::SliderInt("Lamp shadows", &settings.localLightShadows, 0, kMaxRayLocalShadows);
        if (settings.localLightShadows > 0 && e.localLightShadows == 0) ImGui::TextDisabled("  fallback: unshadowed");
        bool reflections = settings.reflections == Reflections::RayTraced;
        if (ImGui::Checkbox("Traced reflections", &reflections))
            settings.reflections = reflections ? Reflections::RayTraced : Reflections::Sky;
        if (settings.reflections != e.reflections) ImGui::TextDisabled("  fallback: the sky's");
        ImGui::TextDisabled("More options: Graphics (G)");

        ImGui::SeparatorText("Lamps");
        for (std::size_t g = 0; g < GroupCount; ++g) {
            if (ImGui::Checkbox(kGroupNames[g], &m_on[g])) applyGroup(g);
            if (g % 2 == 0 && g + 1 < GroupCount) ImGui::SameLine(itemWidth() * 0.75f);
        }
        if (ImGui::SliderFloat("Aluminium anisotropy", &m_anisotropy, 0.0f, 1.0f, "%.2f")) applyAnisotropy();
        bool puddle = m_showPuddle;
        if (ImGui::Checkbox("Puddle", &puddle)) setPuddle(puddle);
        ImGui::SameLine();
        ImGui::Checkbox("Sweep the spot", &m_animate);
        if (ImGui::Checkbox("60 Hz (less heat)", &m_limit60)) applyFrameLimit(c);
    }

    void stats(Context& c) override {
        const gpu::Caps& caps = c.app.device().caps();
        const RenderStats& r = c.renderer.stats();
        ImGui::Text("%u lamps this frame, %u of them area lights", r.lights, r.areaLights);
        if (c.app.deterministic()) {
            ImGui::TextUnformatted("Times hidden (fixed clock)");
        } else if (caps.gpuPassTiming) {
            c.app.requestPassTiming();
            double rays = 0.0, denoise = 0.0, reflect = 0.0, path = 0.0, hdr = 0.0;
            for (const gpu::GpuTiming& t : c.app.stats().gpuPasses) {
                if (t.name == "accel" || t.name == "prepass" || t.name == "raytrace") rays += t.milliseconds;
                if (t.name == "denoise") denoise += t.milliseconds;
                if (t.name == "reflections") reflect += t.milliseconds;
                if (t.name == "path") path += t.milliseconds;
                if (t.name == "hdr") hdr += t.milliseconds;
            }
            if (c.renderer.effects().pathTracer)
                ImGui::Text("Path tracer GPU %.3f ms", path);
            else
                ImGui::Text("Shading GPU %.3f ms; rays %.3f (denoise %.3f, reflections %.3f)", hdr, rays, denoise,
                            reflect);
            ImGui::Text("Frame GPU %.3f ms", c.app.stats().gpuMs);
        } else {
            ImGui::TextUnformatted("GPU pass times n/a on this backend");
        }
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        if (name == "tunnel_ready") return texturesReady() ? 1.0 : 0.0;
        if (name == "anisotropy") return double(m_anisotropy);
        if (name == "sweep") return m_animate ? 1.0 : 0.0;
        if (name == "limit_60hz") return m_limit60 ? 1.0 : 0.0;
        if (name == "puddle") return m_showPuddle ? 1.0 : 0.0;
        for (std::size_t g = 0; g < GroupCount; ++g)
            if (name == kGroupKeys[g]) return m_on[g] ? 1.0 : 0.0;
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        const std::optional<float> number = parseFloat(value);
        const std::optional<bool> flag = parseBool(value);
        if (name == "anisotropy" && number) {
            m_anisotropy = std::clamp(*number, 0.0f, 1.0f);
            applyAnisotropy();
        } else if (name == "sweep" && flag) {
            m_animate = *flag;
        } else if (name == "limit_60hz" && flag) {
            m_limit60 = *flag;
            applyFrameLimit(c);
        } else if (name == "puddle" && flag) {
            setPuddle(*flag);
        } else {
            for (std::size_t g = 0; g < GroupCount; ++g)
                if (name == kGroupKeys[g] && flag) {
                    m_on[g] = *flag;
                    applyGroup(g);
                    return true;
                }
            return false;
        }
        return true;
    }

    std::string perfDetails() const override {
        return std::format("sweep={} limit_60hz={}", m_animate ? 1 : 0, m_limit60 ? 1 : 0);
    }

    void shutdown(Context& c) override {
        (void)c;
        for (assets::AssetRef<TextureAsset>& t : m_textures) t = {};
        m_assets.reset();
        m_manager.reset();
        m_scene.reset();
    }

private:
    struct Lamp {
        LightDesc desc;
        LightId id;
    };

    bool texturesReady() const {
        return std::ranges::all_of(m_textures, [](const assets::AssetRef<TextureAsset>& t) { return t.ready(); });
    }

    // The white spot circles slowly around the aluminium sphere.
    LightDesc spotOnSphere() const {
        const glm::vec3 position(1.4f + 0.6f * std::cos(m_sweep), kHeight - 0.15f, -12.0f + 0.9f * std::sin(m_sweep));
        return {.type = LightType::Spot,
                .color = glm::vec3(0.85f, 0.9f, 1.0f),
                .intensity = 40.0f,
                .position = position,
                .direction = glm::normalize(glm::vec3(1.4f, 0.85f, -12.0f) - position),
                .range = 8.0f,
                .innerAngle = 0.2f,
                .outerAngle = 0.38f,
                .sourceRadius = 0.03f};
    }

    void applyGroup(std::size_t g) {
        for (Lamp& lamp : m_lights[g]) {
            if (m_on[g] && !lamp.id) {
                lamp.id = m_scene->addLight(lamp.desc);
            } else if (!m_on[g] && lamp.id) {
                m_scene->removeLight(lamp.id);
                lamp.id = {};
            }
        }
    }

    void applyAnisotropy() {
        m_aluminium.anisotropy = m_anisotropy;
        m_scene->setMaterial(m_aluminiumMaterial, m_aluminium);
    }

    void setPuddle(bool show) {
        m_showPuddle = show;
        m_scene->setVisible(m_puddle, show);
    }

    // 60 frames per second while the section is current, unless the app already runs slower (--max-frame-rate).
    void applyFrameLimit(Context& c) {
        double rate = m_outsideRate;
        if (m_limit60 && (rate <= 0.0 || rate > 60.0)) rate = 60.0;
        if (c.app.maxFrameRate() != rate) c.app.setMaxFrameRate(rate);
    }

    std::optional<Scene> m_scene;
    std::unique_ptr<assets::AssetManager> m_manager;
    std::optional<SceneAssets> m_assets;
    std::array<assets::AssetRef<TextureAsset>, TextureCount> m_textures;
    std::array<std::vector<Lamp>, GroupCount> m_lights;
    std::array<bool, GroupCount> m_on{true, true, true, true, true, true};
    MaterialDesc m_aluminium;
    MaterialId m_aluminiumMaterial;
    InstanceId m_puddle;
    float m_anisotropy = 0.8f;
    float m_sweep = 0.0f; // the white spot's angle around the sphere (rad)
    bool m_showPuddle = true;
    bool m_animate = true;
    bool m_limit60 = false;
    double m_outsideRate = 0.0; // the app's frame rate limit outside the section
};

} // namespace

std::unique_ptr<Section> makeTunnelSection() { return std::make_unique<TunnelSection>(); }

} // namespace demo
