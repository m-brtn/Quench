// Shadows: the sun's cascaded shadow maps over a long row of columns (the cascades cover 80 m by default), a spinning
// torus, a fence of thin bars and a bouncing ball near the camera. The controls change the cascades, the map size,
// the distance and the filter; with 0 cascades (or a device without depth texture arrays) the scene has no shadows.

#include "Demo.h"

#include <quench/render/Daylight.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;

class ShadowsSection final : public Section {
public:
    ShadowsSection() {
        camera.target = glm::vec3(0.0f, 1.0f, -3.0f);
        camera.distance = 12.0f;
        camera.yaw = glm::radians(24.0f);
        camera.pitch = glm::radians(20.0f);
        camera.maxDistance = 90.0f;
    }

    std::string_view id() const override { return "shadows"; }
    std::string_view title() const override { return "Shadows"; }
    std::string_view about() const override {
        return "Cascaded shadow maps of the sun: columns out to 90 m, a spinning torus, thin bars and a bouncing "
               "ball. Fewer cascades, smaller maps or a longer distance blur the shadows; 0 cascades turns them off.";
    }

    Result<void> init(Context& c) override {
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> ground = addMesh(s, planeMesh(glm::vec2(70.0f), 70.0f));
        const Result<MeshId> column = addMesh(s, boxMesh(glm::vec3(0.35f, 1.5f, 0.35f)));
        const Result<MeshId> bar = addMesh(s, boxMesh(glm::vec3(0.03f, 0.8f, 0.03f)));
        const Result<MeshId> rail = addMesh(s, boxMesh(glm::vec3(1.6f, 0.04f, 0.04f)));
        const Result<MeshId> torus = addMesh(s, torusMesh(0.7f, 0.22f, 64, 32));
        const Result<MeshId> sphere = addMesh(s, sphereMesh(0.4f, 24, 48));
        for (const Result<MeshId>* m : {&ground, &column, &bar, &rail, &torus, &sphere})
            if (!*m) return Error{m->error()};

        s.addInstance({.mesh = *ground,
                       .material = addCheckerMaterial(c, s, glm::vec3(0.55f, 0.52f, 0.46f),
                                                      glm::vec3(0.40f, 0.38f, 0.34f), 2.0f)});
        const MaterialId stone = s.addMaterial({.baseColor = glm::vec4(0.62f, 0.6f, 0.56f, 1.0f), .roughness = 0.8f});
        for (int i = 0; i < 12; ++i)
            for (const float x : {-3.0f, 3.0f})
                s.addInstance(
                    {.mesh = *column, .material = stone, .transform = placed({x, 1.5f, -4.0f - 8.0f * float(i)})});
        const MaterialId metal =
            s.addMaterial({.baseColor = glm::vec4(0.75f, 0.76f, 0.78f, 1.0f), .roughness = 0.35f, .metallic = 1.0f});
        for (int i = 0; i < 17; ++i)
            s.addInstance(
                {.mesh = *bar, .material = metal, .transform = placed({-1.6f + 0.2f * float(i), 0.8f, 3.0f})});
        s.addInstance({.mesh = *rail, .material = metal, .transform = placed({0.0f, 1.6f, 3.0f})});
        m_torus = s.addInstance(
            {.mesh = *torus,
             .material = s.addMaterial({.baseColor = glm::vec4(0.15f, 0.45f, 0.8f, 1.0f), .roughness = 0.3f})});
        m_ball = s.addInstance(
            {.mesh = *sphere,
             .material = s.addMaterial({.baseColor = glm::vec4(0.85f, 0.3f, 0.1f, 1.0f), .roughness = 0.4f})});
        m_sun = s.addLight({.type = LightType::Directional});
        applySun();
        animate(0.0);
        return {};
    }

    void update(Context& c, double frameDt) override {
        if (m_animateSun) {
            m_azimuth = std::fmod(m_azimuth + float(frameDt) * 12.0f, 360.0f);
            applySun();
        }
        animate(c.app.time());
    }

    void ui(Context& c) override {
        (void)c;
        const float w = ImGui::GetFontSize() * 8.0f;
        ImGui::SetNextItemWidth(w);
        ImGui::SliderInt("Cascades", &settings.shadowCascades, 0, kMaxCascades);
        static constexpr std::array<std::uint32_t, 4> kSizes{512, 1024, 2048, 4096};
        int size = int(std::find(kSizes.begin(), kSizes.end(), settings.shadowMapSize) - kSizes.begin());
        ImGui::SetNextItemWidth(w);
        if (ImGui::Combo("Map size", &size,
                         "512\0"
                         "1024\0"
                         "2048\0"
                         "4096\0"))
            settings.shadowMapSize = kSizes[std::size_t(size)];
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("Distance (m)", &settings.shadowDistance, 10.0f, 200.0f, "%.0f");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderInt("Filter", &settings.shadowFilter, 0, 2,
                         settings.shadowFilter == 0   ? "1 tap"
                         : settings.shadowFilter == 1 ? "4 taps"
                                                      : "12 taps");
        ImGui::Separator();
        ImGui::SetNextItemWidth(w);
        bool changed = ImGui::SliderFloat("Sun elevation", &m_elevation, 2.0f, 90.0f, "%.0f deg");
        ImGui::SetNextItemWidth(w);
        changed |= ImGui::SliderFloat("Sun azimuth", &m_azimuth, 0.0f, 360.0f, "%.0f deg");
        if (changed) applySun();
        ImGui::Checkbox("Turn the sun", &m_animateSun);
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        if (name == "sun_elevation") return double(m_elevation);
        if (name == "sun_azimuth") return double(m_azimuth);
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        (void)c;
        const std::optional<float> number = parseFloat(value);
        const std::optional<bool> flag = parseBool(value);
        if (name == "sun_elevation" && number) {
            m_elevation = std::clamp(*number, -10.0f, 90.0f);
        } else if (name == "sun_azimuth" && number) {
            m_azimuth = *number;
        } else if (name == "animate" && flag) {
            m_animateSun = *flag;
        } else {
            return false;
        }
        applySun();
        return true;
    }

    void shutdown(Context& c) override {
        (void)c;
        m_scene.reset();
    }

private:
    void applySun() {
        const glm::vec3 towardSun = skyDirection(m_elevation, m_azimuth);
        const Daylight day = daylightFor(towardSun);
        m_scene->setEnvironment(day.environment);
        m_scene->setLight(m_sun, {.type = LightType::Directional, .color = day.sunColor, .direction = -towardSun});
    }

    void animate(double time) {
        const float t = float(time);
        glm::mat4 torus = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.7f, 0.0f));
        torus = glm::rotate(torus, t * 0.8f, glm::vec3(0.0f, 1.0f, 0.0f));
        torus = glm::rotate(torus, glm::radians(60.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        m_scene->setTransform(m_torus, torus);
        const float bounce = std::abs(std::sin(t * 2.2f));
        m_scene->setTransform(m_ball, placed({-2.2f, 0.4f + 1.6f * bounce, 1.0f}));
    }

    std::optional<Scene> m_scene;
    InstanceId m_torus, m_ball;
    LightId m_sun;
    float m_elevation = 32.0f;
    float m_azimuth = 50.0f;
    bool m_animateSun = false;
};

} // namespace

std::unique_ptr<Section> makeShadowsSection() { return std::make_unique<ShadowsSection>(); }

} // namespace demo
