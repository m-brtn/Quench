// Post-processing and anti-aliasing: glowing rings and lamps for bloom, thin poles and fast spinning blades for
// aliasing and motion, at dusk. The controls switch the anti-aliasing (TAA with motion vectors, FXAA, none; TAA falls
// back to FXAA where the device lacks what it needs), the render scale, bloom, the tone curve, exposure, vignette and
// dithering.

#include "Demo.h"

#include <quench/render/Daylight.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;

class PostSection final : public Section {
public:
    PostSection() {
        camera.target = glm::vec3(0.0f, 1.2f, 0.0f);
        camera.distance = 8.5f;
        camera.yaw = glm::radians(-20.0f);
        camera.pitch = glm::radians(14.0f);
        settings.bloomStrength = 0.08f;
    }

    std::string_view id() const override { return "post"; }
    std::string_view title() const override { return "Post-processing"; }
    std::string_view about() const override {
        return "Bloom, tone mapping and anti-aliasing at dusk: glowing rings and lamps, thin poles and spinning "
               "blades. TAA falls back to FXAA on devices without sampled depth or Rg16Float targets.";
    }

    Result<void> init(Context& c) override {
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> ground = addMesh(s, planeMesh(glm::vec2(40.0f), 40.0f));
        const Result<MeshId> ring = addMesh(s, torusMesh(0.8f, 0.06f, 96, 16));
        const Result<MeshId> lamp = addMesh(s, sphereMesh(0.18f, 16, 32));
        const Result<MeshId> pole = addMesh(s, boxMesh(glm::vec3(0.02f, 1.5f, 0.02f)));
        const Result<MeshId> blade = addMesh(s, boxMesh(glm::vec3(1.3f, 0.05f, 0.02f)));
        const Result<MeshId> hub = addMesh(s, boxMesh(glm::vec3(0.15f, 1.0f, 0.15f)));
        for (const Result<MeshId>* m : {&ground, &ring, &lamp, &pole, &blade, &hub})
            if (!*m) return Error{m->error()};
        s.addInstance({.mesh = *ground,
                       .material = addCheckerMaterial(c, s, glm::vec3(0.30f, 0.30f, 0.32f),
                                                      glm::vec3(0.12f, 0.12f, 0.14f), 0.5f, 0.6f)});
        const std::array<glm::vec3, 3> neon{glm::vec3(8.0f, 0.6f, 4.0f), glm::vec3(0.4f, 5.0f, 8.0f),
                                            glm::vec3(8.0f, 6.0f, 0.6f)};
        for (int i = 0; i < 3; ++i) {
            const MaterialId glow = s.addMaterial({.baseColor = glm::vec4(0.02f, 0.02f, 0.02f, 1.0f),
                                                   .emissive = neon[std::size_t(i)],
                                                   .castsShadows = false});
            glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(-2.6f + 2.6f * float(i), 1.0f, -1.5f));
            m = glm::rotate(m, glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
            s.addInstance({.mesh = *ring, .material = glow, .transform = m});
            s.addInstance(
                {.mesh = *lamp, .material = glow, .transform = placed({-2.6f + 2.6f * float(i), 0.18f, 0.4f})});
        }
        const MaterialId white = s.addMaterial({.baseColor = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f), .roughness = 0.4f});
        for (int i = 0; i < 9; ++i) {
            const float x = -4.0f + float(i);
            s.addInstance(
                {.mesh = *pole, .material = white, .transform = placed({x, 1.5f, 1.8f}, glm::vec3(1.0f), 0.3f)});
        }
        const MaterialId dark =
            s.addMaterial({.baseColor = glm::vec4(0.1f, 0.1f, 0.12f, 1.0f), .roughness = 0.5f, .metallic = 1.0f});
        s.addInstance({.mesh = *hub, .material = dark, .transform = placed({3.6f, 1.0f, -0.2f})});
        const MaterialId bladeMaterial =
            s.addMaterial({.baseColor = glm::vec4(0.95f, 0.5f, 0.1f, 1.0f), .roughness = 0.35f});
        for (InstanceId& b : m_blades) b = s.addInstance({.mesh = *blade, .material = bladeMaterial});

        const glm::vec3 towardSun = skyDirection(5.0f, 250.0f);
        const Daylight day = daylightFor(towardSun);
        m_environment = day.environment;
        s.addLight({.type = LightType::Directional, .color = day.sunColor, .direction = -towardSun});
        applyEnvironment();
        animate(0.0);
        return {};
    }

    void update(Context& c, double frameDt) override {
        (void)frameDt;
        if (m_spin) m_spinTime = c.app.time();
        animate(m_spinTime);
    }

    void ui(Context& c) override {
        const float w = ImGui::GetFontSize() * 8.0f;
        int aa = int(settings.antiAliasing);
        ImGui::TextUnformatted("Anti-aliasing");
        ImGui::RadioButton("none", &aa, 0);
        ImGui::SameLine();
        ImGui::RadioButton("FXAA", &aa, 1);
        ImGui::SameLine();
        ImGui::RadioButton("TAA", &aa, 2);
        settings.antiAliasing = AntiAliasing(aa);
        if (c.renderer.effects().antiAliasing != settings.antiAliasing)
            ImGui::TextDisabled("running %s (fallback)", antiAliasingName(c.renderer.effects().antiAliasing));
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("Render scale", &settings.renderScale, 0.25f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("Sharpen", &settings.sharpen, 0.0f, 1.0f);
        ImGui::Checkbox("Spin the blades", &m_spin);
        ImGui::Separator();
        ImGui::Checkbox("Bloom", &settings.bloom);
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("Strength", &settings.bloomStrength, 0.0f, 0.3f, "%.3f");
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("Threshold", &settings.bloomThreshold, 0.2f, 4.0f, "%.2f");
        int curve = int(settings.toneCurve);
        ImGui::SetNextItemWidth(w);
        if (ImGui::Combo("Tone curve", &curve, "ACES\0AgX\0")) settings.toneCurve = ToneCurve(curve);
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("Exposure", &m_exposure, 0.25f, 4.0f, "x %.2f", ImGuiSliderFlags_Logarithmic))
            applyEnvironment();
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("Vignette", &settings.vignette, 0.0f, 1.0f);
        ImGui::Checkbox("Dither", &settings.dither);
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        if (name == "exposure") return double(m_exposure);
        if (name == "spin") return m_spin ? 1.0 : 0.0;
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        (void)c;
        const std::optional<float> number = parseFloat(value);
        const std::optional<bool> flag = parseBool(value);
        if (name == "exposure" && number) {
            m_exposure = std::clamp(*number, 0.05f, 16.0f);
            applyEnvironment();
        } else if (name == "spin" && flag) {
            m_spin = *flag;
        } else {
            return false;
        }
        return true;
    }

    void shutdown(Context& c) override {
        (void)c;
        m_scene.reset();
    }

private:
    void applyEnvironment() {
        Environment e = m_environment;
        e.exposure *= m_exposure;
        m_scene->setEnvironment(e);
    }

    void animate(double time) {
        constexpr float kTau = 2.0f * std::numbers::pi_v<float>;
        for (std::size_t i = 0; i < m_blades.size(); ++i) {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(3.6f, 1.9f, 0.0f));
            m = glm::rotate(m, float(time) * 5.0f + kTau * float(i) / float(m_blades.size()), glm::vec3(0, 0, 1));
            m = glm::translate(m, glm::vec3(1.3f, 0.0f, 0.0f));
            m_scene->setTransform(m_blades[i], m);
        }
    }

    std::optional<Scene> m_scene;
    std::array<InstanceId, 3> m_blades{};
    Environment m_environment;
    float m_exposure = 1.0f;
    bool m_spin = true;
    double m_spinTime = 0.0;
};

} // namespace

std::unique_ptr<Section> makePostSection() { return std::make_unique<PostSection>(); }

} // namespace demo
