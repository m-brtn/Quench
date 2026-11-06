// Materials and lights: rows of dielectric and metal spheres from smooth to rough, a textured box, alpha-blended glass,
// an alpha-tested fence, an emissive lamp, a ground drawn by the demo's own surface shader (shaders/checker.hlsl), the
// sun, three moving point lights and a spot light. Glowing discs at the lights are the demo's own render pass
// (shaders/markers.hlsl, PassStage::Transparent).

#include "Demo.h"
#include "shaders/DemoUniforms.h"

#include <quench/core/Log.h>
#include <quench/render/Daylight.h>

#include <demo_markers_shaders.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <numbers>
#include <vector>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;

constexpr int kPointLights = 3;
constexpr std::array<glm::vec3, kPointLights> kPointColors{glm::vec3(1.0f, 0.25f, 0.12f), glm::vec3(0.25f, 1.0f, 0.35f),
                                                           glm::vec3(0.25f, 0.45f, 1.0f)};

// A 64 x 64 test pattern: colored quadrants with a grid, sRGB.
std::vector<std::byte> patternPixels() {
    std::vector<std::byte> pixels(64 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const bool grid = x % 16 == 0 || y % 16 == 0;
            const int q = (x / 32) + 2 * (y / 32);
            const std::array<std::array<int, 3>, 4> colors{
                {{230, 90, 60}, {240, 200, 70}, {70, 170, 220}, {120, 200, 110}}};
            const auto c = grid ? std::array<int, 3>{30, 30, 34} : colors[std::size_t(q)];
            std::byte* p = &pixels[std::size_t(y * 64 + x) * 4];
            for (int i = 0; i < 3; ++i) p[i] = std::byte(c[std::size_t(i)]);
            p[3] = std::byte(255);
        }
    return pixels;
}

// A 64 x 64 fence: opaque bars with transparent holes (alpha 0), for alpha testing.
std::vector<std::byte> fencePixels() {
    std::vector<std::byte> pixels(64 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const float cx = float(x % 16) - 7.5f, cy = float(y % 16) - 7.5f;
            const bool hole = cx * cx + cy * cy < 30.0f;
            std::byte* p = &pixels[std::size_t(y * 64 + x) * 4];
            p[0] = std::byte(200);
            p[1] = std::byte(205);
            p[2] = std::byte(215);
            p[3] = std::byte(hole ? 0 : 255);
        }
    return pixels;
}

class MaterialsSection final : public Section {
public:
    MaterialsSection() {
        camera.target = glm::vec3(0.0f, 0.7f, 0.3f);
        camera.distance = 9.5f;
        camera.yaw = glm::radians(18.0f);
        camera.pitch = glm::radians(24.0f);
        settings.dither = true;
    }

    std::string_view id() const override { return "materials"; }
    std::string_view title() const override { return "Materials and lights"; }
    std::string_view about() const override {
        return "Spheres from smooth to rough (top: dielectric, back: metal), a textured box, glass, an alpha-tested "
               "fence and a lamp, lit by the sun, three point lights and a spot. The ground is a game surface "
               "shader; the light discs are a game render pass.";
    }

    Result<void> init(Context& c) override {
        m_device = &c.app.device();
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> sphere = addMesh(s, sphereMesh(0.45f, 24, 48));
        const Result<MeshId> box = addMesh(s, boxMesh(glm::vec3(0.6f)));
        const Result<MeshId> torus = addMesh(s, torusMesh(0.55f, 0.2f, 64, 32));
        const Result<MeshId> ground = addMesh(s, planeMesh(glm::vec2(15.0f), 15.0f));
        const Result<MeshId> panel = addMesh(s, planeMesh(glm::vec2(0.7f, 0.9f)));
        for (const Result<MeshId>* m : {&sphere, &box, &torus, &ground, &panel})
            if (!*m) return Error{m->error()};

        const std::vector<std::byte> pattern = patternPixels(), fence = fencePixels();
        const Result<TextureId> patternTexture =
            s.addTexture({.width = 64, .height = 64, .pixels = pattern, .name = "pattern"});
        if (!patternTexture) return Error{patternTexture.error()};
        const Result<TextureId> fenceTexture =
            s.addTexture({.width = 64, .height = 64, .pixels = fence, .name = "fence"});
        if (!fenceTexture) return Error{fenceTexture.error()};

        m_checker = addCheckerMaterial(c, s, glm::vec3(0.42f, 0.43f, 0.45f), glm::vec3(0.22f, 0.23f, 0.25f), 1.0f);
        m_plainGround = s.addMaterial({.baseColor = glm::vec4(0.32f, 0.33f, 0.35f, 1.0f), .roughness = 0.85f});
        m_ground = s.addInstance({.mesh = *ground, .material = m_checker});

        for (int i = 0; i < 6; ++i) {
            const float roughness = std::max(0.05f, float(i) / 5.0f);
            const float x = -3.75f + 1.5f * float(i);
            const MaterialId plastic =
                s.addMaterial({.baseColor = glm::vec4(0.75f, 0.12f, 0.08f, 1.0f), .roughness = roughness});
            const MaterialId metal = s.addMaterial(
                {.baseColor = glm::vec4(1.0f, 0.78f, 0.34f, 1.0f), .roughness = roughness, .metallic = 1.0f});
            s.addInstance({.mesh = *sphere, .material = plastic, .transform = placed({x, 0.45f, 0.0f})});
            s.addInstance({.mesh = *sphere, .material = metal, .transform = placed({x, 0.45f, -1.5f})});
        }

        MaterialDesc textured{.roughness = 0.6f, .name = "pattern"};
        textured.textures[0] = *patternTexture;
        m_box = s.addInstance({.mesh = *box, .material = s.addMaterial(textured)});
        m_glass = s.addInstance({.mesh = *torus,
                                 .material = s.addMaterial({.baseColor = glm::vec4(0.55f, 0.8f, 1.0f, 0.3f),
                                                            .roughness = 0.08f,
                                                            .blend = BlendMode::AlphaBlend,
                                                            .name = "glass"})});
        MaterialDesc fenceMaterial{.roughness = 0.5f, .alphaCutoff = 0.5f, .doubleSided = true, .name = "fence"};
        fenceMaterial.textures[0] = *fenceTexture;
        const glm::mat4 upright = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        s.addInstance({.mesh = *panel,
                       .material = s.addMaterial(fenceMaterial),
                       .transform = placed({4.6f, 0.9f, 1.9f}, glm::vec3(1.0f), glm::radians(-20.0f)) * upright});
        s.addInstance({.mesh = *box,
                       .material = s.addMaterial({.baseColor = glm::vec4(0.05f, 0.05f, 0.05f, 1.0f),
                                                  .emissive = glm::vec3(9.0f, 5.0f, 2.0f),
                                                  .shadows = ShadowMode::None,
                                                  .name = "lamp"}),
                       .transform = placed({-1.6f, 0.12f, 2.6f}, glm::vec3(0.2f))});

        const glm::vec3 towardSun = skyDirection(38.0f, 35.0f);
        const Daylight day = daylightFor(towardSun);
        s.setEnvironment(day.environment);
        m_sunDesc = {.type = LightType::Directional, .color = day.sunColor, .direction = -towardSun};
        m_sun = s.addLight(m_sunDesc);
        for (int i = 0; i < kPointLights; ++i)
            m_points[std::size_t(i)] = s.addLight(
                {.type = LightType::Point, .color = kPointColors[std::size_t(i)], .intensity = 5.0f, .range = 6.0f});
        m_spot = s.addLight(spotDesc());
        animate(0.0);
        return {};
    }

    void enter(Context& c) override {
        m_pass = c.renderer.addPass(PassStage::Transparent, "demo light markers",
                                    [this](PassContext& context) { drawMarkers(context); });
    }

    void leave(Context& c) override {
        c.renderer.removePass(m_pass);
        m_pass = {};
    }

    void update(Context& c, double frameDt) override {
        (void)frameDt;
        if (m_animate) m_animTime = c.app.time();
        animate(m_animTime);
    }

    void controls(Context& c) override {
        if (ImGui::Checkbox("Sun", &m_sunOn)) applyLights();
        if (ImGui::SliderInt("Point lights", &m_pointCount, 0, kPointLights)) applyLights();
        if (ImGui::Checkbox("Spot light", &m_spotOn)) applyLights();
        if (ImGui::SliderFloat("Light intensity", &m_intensity, 0.0f, 4.0f)) applyLights();
        ImGui::Checkbox("Animate", &m_animate);
        ImGui::Checkbox("Light discs (game pass)", &m_markers);
        bool checker = m_useChecker;
        if (ImGui::Checkbox("Checker (game shader)", &checker)) setChecker(checker);
        if (!c.checker) ImGui::TextDisabled("checker shader unavailable: standard surface");
        // The glass torus's shadow: tinted through a transmittance map, or dithered (the fallback).
        int translucent = settings.translucentShadows == TranslucentShadows::Map ? 0 : 1;
        if (ImGui::Combo("Glass shadow", &translucent,
                         "tinted\0"
                         "dithered\0"))
            settings.translucentShadows = translucent == 0 ? TranslucentShadows::Map : TranslucentShadows::Dithered;
        if (settings.translucentShadows != c.renderer.effects().translucentShadows)
            ImGui::TextDisabled("  fallback: dithered");
    }

    void stats(Context& c) override {
        ImGui::Text("Light discs: %d draws", m_markerDraws);
        ImGui::Text("Transmittance: %u draws", c.renderer.stats().transmittanceDraws);
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        if (name == "point_lights") return double(m_pointCount);
        if (name == "spot") return m_spotOn ? 1.0 : 0.0;
        if (name == "sun") return m_sunOn ? 1.0 : 0.0;
        if (name == "markers") return m_markers ? 1.0 : 0.0;
        if (name == "marker_draws") return double(m_markerDraws);
        if (name == "checker") return m_useChecker ? 1.0 : 0.0;
        if (name == "materials") return double(m_scene->materialCount());
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        (void)c;
        const std::optional<bool> flag = parseBool(value);
        const std::optional<float> number = parseFloat(value);
        if (name == "point_lights" && number) {
            m_pointCount = std::clamp(int(*number), 0, kPointLights);
        } else if (name == "spot" && flag) {
            m_spotOn = *flag;
        } else if (name == "sun" && flag) {
            m_sunOn = *flag;
        } else if (name == "intensity" && number) {
            m_intensity = std::max(*number, 0.0f);
        } else if (name == "animate" && flag) {
            m_animate = *flag;
        } else if (name == "markers" && flag) {
            m_markers = *flag;
        } else if (name == "checker" && flag) {
            setChecker(*flag);
            return true;
        } else {
            return false;
        }
        applyLights();
        return true;
    }

    void shutdown(Context& c) override {
        (void)c;
        m_pipelines.clear();
        m_scene.reset();
    }

private:
    LightDesc spotDesc() const {
        return {.type = LightType::Spot,
                .color = glm::vec3(1.0f, 0.88f, 0.7f),
                .intensity = 30.0f * m_intensity,
                .position = glm::vec3(0.0f, 4.0f, -0.75f),
                .direction = glm::normalize(glm::vec3(0.0f, -1.0f, 0.1f)),
                .range = 9.0f,
                .innerAngle = 0.3f,
                .outerAngle = 0.45f};
    }

    void applyLights() {
        Scene& s = *m_scene;
        LightDesc sun = m_sunDesc;
        sun.intensity = m_sunOn ? 1.0f : 0.0f;
        s.setLight(m_sun, sun);
        LightDesc spot = spotDesc();
        if (!m_spotOn) spot.intensity = 0.0f;
        s.setLight(m_spot, spot);
        animate(m_animTime);
    }

    void animate(double time) {
        constexpr float kTau = 2.0f * std::numbers::pi_v<float>;
        Scene& s = *m_scene;
        for (int i = 0; i < kPointLights; ++i) {
            const float a = float(time) * 0.6f + kTau * float(i) / float(kPointLights);
            m_pointPositions[std::size_t(i)] = glm::vec3(3.2f * std::cos(a), 1.1f, -0.75f + 2.2f * std::sin(a));
            s.setLight(m_points[std::size_t(i)], {.type = LightType::Point,
                                                  .color = kPointColors[std::size_t(i)],
                                                  .intensity = i < m_pointCount ? 5.0f * m_intensity : 0.0f,
                                                  .position = m_pointPositions[std::size_t(i)],
                                                  .range = 6.0f});
        }
        const float t = float(time);
        s.setTransform(m_box, placed({-4.6f, 0.6f, 1.9f}, glm::vec3(1.0f), 0.4f + t * 0.5f));
        glm::mat4 glass = glm::translate(glm::mat4(1.0f), glm::vec3(1.4f, 1.0f, 2.2f));
        glass = glm::rotate(glass, t * 0.7f, glm::vec3(0.0f, 1.0f, 0.0f));
        glass = glm::rotate(glass, glm::radians(70.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        s.setTransform(m_glass, glass);
    }

    void setChecker(bool on) {
        m_useChecker = on;
        m_scene->setInstanceMaterial(m_ground, on ? m_checker : m_plainGround);
    }

    // The game pass: one camera-facing disc per light that is on, additive, depth tested against the scene.
    void drawMarkers(PassContext& context) {
        m_markerDraws = 0;
        if (!m_markers || !context.pass) return;
        shader::MarkerUniforms u{};
        const glm::mat4& view = context.frame.view;
        u.right = glm::vec4(view[0][0], view[1][0], view[2][0], 0.0f);
        u.up = glm::vec4(view[0][1], view[1][1], view[2][1], 0.0f);
        std::uint32_t count = 0;
        for (int i = 0; i < m_pointCount; ++i) {
            u.position[count] = glm::vec4(m_pointPositions[std::size_t(i)], 0.09f);
            u.color[count++] = glm::vec4(kPointColors[std::size_t(i)] * 12.0f, 0.0f);
        }
        if (m_spotOn) {
            u.position[count] = glm::vec4(spotDesc().position, 0.14f);
            u.color[count++] = glm::vec4(glm::vec3(1.0f, 0.88f, 0.7f) * 12.0f, 0.0f);
        }
        if (count == 0) return;
        const gpu::GraphicsPipeline* pipeline = markerPipeline(context);
        if (!pipeline) return;
        context.pass->bindPipeline(*pipeline);
        context.pass->pushUniforms(ShaderStage::Vertex, 1, u);
        context.pass->draw(6, count);
        m_markerDraws = int(count);
    }

    const gpu::GraphicsPipeline* markerPipeline(const PassContext& context) {
        const auto key = std::pair(context.colorFormat, context.depthFormat);
        if (const auto it = m_pipelines.find(key); it != m_pipelines.end()) return it->second ? &it->second : nullptr;
        const gpu::ColorTargetDesc color{.format = context.colorFormat, .blend = gpu::BlendState::additive()};
        Result<gpu::GraphicsPipeline> pipeline = m_device->createPipeline({
            .vertexShader = shaders::demo_markers.find("marker_vs"),
            .fragmentShader = shaders::demo_markers.find("marker_fs"),
            .depth = {.test = true, .write = false},
            .colorTargets = {&color, 1},
            .depthFormat = context.depthFormat,
            .name = "demo markers",
        });
        if (!pipeline) log::error("demo", "marker pipeline: {}", pipeline.error());
        gpu::GraphicsPipeline& slot = m_pipelines[key];
        if (pipeline) slot = std::move(*pipeline);
        return slot ? &slot : nullptr;
    }

    gpu::Device* m_device = nullptr;
    std::optional<Scene> m_scene;
    MaterialId m_checker, m_plainGround;
    InstanceId m_ground, m_box, m_glass;
    LightDesc m_sunDesc;
    LightId m_sun, m_spot;
    std::array<LightId, kPointLights> m_points{};
    std::array<glm::vec3, kPointLights> m_pointPositions{};
    std::map<std::pair<gpu::TextureFormat, gpu::TextureFormat>, gpu::GraphicsPipeline> m_pipelines;
    PassId m_pass;
    bool m_sunOn = true, m_spotOn = true, m_animate = true, m_markers = true, m_useChecker = true;
    int m_pointCount = kPointLights;
    int m_markerDraws = 0;
    float m_intensity = 1.0f;
    double m_animTime = 0.0;
};

} // namespace

std::unique_ptr<Section> makeMaterialsSection() { return std::make_unique<MaterialsSection>(); }

} // namespace demo
