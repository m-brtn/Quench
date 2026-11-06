// HDR output: a low sun over a dark plain and a row of emissive panels from half to 32 times the radiance at the knee
// of the HDR shoulder (hdrKnee), next to a white card in the shade. With HDR output (--hdr on or auto, or the Mode
// control) the sun, the glow around it and the brighter panels go above paper white, up to the display's headroom,
// while the card and the UI stay at SDR brightness; in SDR the bright panels all look the same white. "SDR curve"
// renders the SDR tone curve into the HDR output for comparison; "Peak" replaces the display's headroom in the tone
// curve.

#include "Demo.h"

#include <quench/render/ToneMap.h>

#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;

constexpr std::array<float, 7> kPanelLevels{0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f};

class HdrSection final : public Section {
public:
    HdrSection() {
        // Looking slightly up, so the sun is in view above the panels.
        camera.target = glm::vec3(0.0f, 2.2f, -8.0f);
        camera.distance = 11.0f;
        camera.yaw = 0.0f;
        camera.pitch = glm::radians(-5.0f);
        settings.shadowDistance = 60.0f;
        settings.bloomStrength = 0.05f;
    }

    std::string_view id() const override { return "hdr"; }
    std::string_view title() const override { return "HDR output"; }
    std::string_view about() const override {
        return "HDR display output: with Mode on or auto the frame goes to the display in extended linear, up to its "
               "headroom; the sun and the bright panels rise above paper white, the card and the UI do not. On an "
               "SDR display, or where the GPU backend lacks HDR, the output stays SDR. Captures are SDR.";
    }

    Result<void> init(Context& c) override {
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> ground = addMesh(s, planeMesh(glm::vec2(200.0f), 50.0f));
        const Result<MeshId> panel = addMesh(s, boxMesh(glm::vec3(0.45f, 0.9f, 0.05f)));
        const Result<MeshId> card = addMesh(s, boxMesh(glm::vec3(0.6f, 0.9f, 0.05f)));
        const Result<MeshId> post = addMesh(s, boxMesh(glm::vec3(0.06f, 0.55f, 0.06f)));
        for (const Result<MeshId>* m : {&ground, &panel, &card, &post})
            if (!*m) return Error{m->error()};
        s.addInstance({.mesh = *ground,
                       .material = addCheckerMaterial(c, s, glm::vec3(0.12f, 0.12f, 0.13f),
                                                      glm::vec3(0.07f, 0.07f, 0.08f), 2.0f, 0.8f)});
        const MaterialId dark = s.addMaterial({.baseColor = glm::vec4(0.05f, 0.05f, 0.05f, 1.0f), .roughness = 0.6f});
        for (std::size_t i = 0; i < kPanelLevels.size(); ++i) {
            const float x = (float(i) - 3.0f) * 1.3f;
            m_panels[i] =
                s.addMaterial({.baseColor = glm::vec4(0.02f, 0.02f, 0.02f, 1.0f), .shadows = ShadowMode::None});
            s.addInstance({.mesh = *panel, .material = m_panels[i], .transform = placed({x, 2.0f, -8.0f})});
            s.addInstance({.mesh = *post, .material = dark, .transform = placed({x, 0.55f, -8.0f})});
        }
        const MaterialId white = s.addMaterial({.baseColor = glm::vec4(0.85f, 0.85f, 0.85f, 1.0f), .roughness = 0.9f});
        s.addInstance(
            {.mesh = *card, .material = white, .transform = placed({-5.4f, 0.9f, -6.0f}, glm::vec3(1.0f), 0.4f)});
        m_sun = s.addLight({.type = LightType::Directional});
        apply();
        return {};
    }

    void leave(Context& c) override {
        // The output mode is the app's: other sections keep it, as a game would.
        (void)c;
    }

    void controls(Context& c) override {
        app::App& app = c.app;
        int mode = int(app.hdrMode());
        ImGui::TextUnformatted("Mode");
        ImGui::SameLine();
        ImGui::RadioButton("off", &mode, int(app::HdrMode::Off));
        ImGui::SameLine();
        ImGui::RadioButton("auto", &mode, int(app::HdrMode::Auto));
        ImGui::SameLine();
        ImGui::RadioButton("on", &mode, int(app::HdrMode::On));
        if (mode != int(app.hdrMode())) app.setHdrMode(app::HdrMode(mode));
        ImGui::Checkbox("SDR curve (compare)", &m_sdrCurve);
        ImGui::Checkbox("Peak from the display", &m_displayPeak);
        if (!m_displayPeak) {
            ImGui::SliderFloat("Peak", &m_peak, 1.0f, 16.0f, "x %.1f", ImGuiSliderFlags_Logarithmic);
        }
        ImGui::Separator();
        bool changed = false;
        changed |= ImGui::SliderFloat("Panels", &m_emissive, 0.1f, 4.0f, "x %.2f", ImGuiSliderFlags_Logarithmic);
        changed |= ImGui::SliderFloat("Sun elevation", &m_elevation, -2.0f, 40.0f, "%.0f deg");
        if (changed) apply();
        ImGui::TextDisabled("Panels x0.5 .. x32");
    }

    void stats(Context& c) override {
        const app::App& app = c.app;
        const gpu::DisplayOutput& out = app.output();
        ImGui::TextWrapped("Display headroom %.2f (potential %.2f); HDR %s by %s", double(app.displayHeadroom()),
                           double(app.displayPotentialHeadroom()), app.hdrSupported() ? "supported" : "not supported",
                           c.app.device().caps().driver.c_str());
        ImGui::TextWrapped("Output: %s, headroom %.2f, paper white %.2f, %s",
                           gpu::swapchainCompositionName(out.composition), double(out.headroom), double(out.paperWhite),
                           gpu::formatInfo(app.swapchainFormat()).name);
        if (out.hdr() && app.displayHeadroom() <= 1.0f)
            ImGui::TextWrapped("This display reports no HDR range: values above paper white are clipped.");
    }

    gpu::DisplayOutput output(const gpu::DisplayOutput& display) const override {
        gpu::DisplayOutput o = display;
        if (!o.hdr()) return o;
        if (m_sdrCurve)
            o.headroom = 1.0f;
        else if (!m_displayPeak)
            o.headroom = m_peak;
        return o;
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        if (name == "sdr_curve") return m_sdrCurve ? 1.0 : 0.0;
        if (name == "peak") return m_displayPeak ? 0.0 : double(m_peak);
        if (name == "sun_elevation") return double(m_elevation);
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        (void)c;
        const std::optional<float> number = parseFloat(value);
        const std::optional<bool> flag = parseBool(value);
        if (name == "sdr_curve" && flag) {
            m_sdrCurve = *flag;
        } else if (name == "peak" && number) {
            m_displayPeak = *number <= 0.0f;
            m_peak = std::clamp(*number, 1.0f, 64.0f);
        } else if (name == "panels" && number) {
            m_emissive = std::max(*number, 0.0f);
        } else if (name == "sun_elevation" && number) {
            m_elevation = std::clamp(*number, -12.0f, 90.0f);
        } else {
            return false;
        }
        apply();
        return true;
    }

    void shutdown(Context& c) override {
        (void)c;
        m_scene.reset();
    }

private:
    void apply() {
        // Darker than the palette, so the sky around the sun is not white in SDR and the disc stands out.
        const Environment e = setDaylight(*m_scene, m_sun, m_elevation, 0.0f, 0.55f);
        // Level 1 is the knee of the HDR shoulder (0.6 of paper white): where HDR output starts to differ from SDR.
        const float knee = hdrKnee(settings.toneCurve).scene / e.exposure;
        for (std::size_t i = 0; i < kPanelLevels.size(); ++i)
            m_scene->setMaterial(m_panels[i],
                                 {.baseColor = glm::vec4(0.02f, 0.02f, 0.02f, 1.0f),
                                  .emissive = glm::vec3(1.0f, 0.9f, 0.75f) * (knee * kPanelLevels[i] * m_emissive),
                                  .shadows = ShadowMode::None});
    }

    std::optional<Scene> m_scene;
    LightId m_sun;
    std::array<MaterialId, kPanelLevels.size()> m_panels{};
    float m_elevation = 14.0f;
    float m_emissive = 1.0f;
    bool m_sdrCurve = false;
    bool m_displayPeak = true;
    float m_peak = 4.0f;
};

} // namespace

std::unique_ptr<Section> makeHdrSection() { return std::make_unique<HdrSection>(); }

} // namespace demo
