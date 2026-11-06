// Sky and fog: a wide plain with pillars and hills out to 200 m under the engine's sky (single-scattering atmosphere,
// or the plain horizon-zenith gradient as the simpler model), the daylight palette for any sun elevation, and height
// fog that thins upward and brightens toward the sun. Without the sky the background is the clear color.

#include "Demo.h"

#include <quench/render/Daylight.h>

#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;

class SkySection final : public Section {
public:
    SkySection() {
        camera.target = glm::vec3(0.0f, 3.0f, -30.0f);
        camera.distance = 30.0f;
        camera.yaw = 0.0f;
        camera.pitch = glm::radians(7.0f);
        camera.maxDistance = 120.0f;
        settings.shadowDistance = 120.0f;
        settings.clearColor = glm::vec3(0.25f, 0.3f, 0.36f);
    }

    std::string_view id() const override { return "sky"; }
    std::string_view title() const override { return "Sky and fog"; }
    std::string_view about() const override {
        return "The sky model and the daylight palette for any sun elevation, with height fog over a plain of "
               "pillars and hills. The gradient sky is the simpler fallback model; without the sky the background "
               "is the clear color.";
    }

    Result<void> init(Context& c) override {
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> ground = addMesh(s, planeMesh(glm::vec2(400.0f), 100.0f));
        const Result<MeshId> pillar = addMesh(s, boxMesh(glm::vec3(0.5f, 3.0f, 0.5f)));
        const Result<MeshId> hill = addMesh(s, sphereMesh(1.0f, 24, 48));
        for (const Result<MeshId>* m : {&ground, &pillar, &hill})
            if (!*m) return Error{m->error()};
        s.addInstance({.mesh = *ground,
                       .material = addCheckerMaterial(c, s, glm::vec3(0.36f, 0.42f, 0.26f),
                                                      glm::vec3(0.30f, 0.36f, 0.22f), 4.0f, 0.95f)});
        const MaterialId stone = s.addMaterial({.baseColor = glm::vec4(0.7f, 0.66f, 0.6f, 1.0f), .roughness = 0.85f});
        for (int i = 0; i < 20; ++i)
            for (const float x : {-5.0f, 5.0f})
                s.addInstance({.mesh = *pillar, .material = stone, .transform = placed({x, 3.0f, -10.0f * float(i)})});
        const MaterialId grass = s.addMaterial({.baseColor = glm::vec4(0.25f, 0.33f, 0.18f, 1.0f), .roughness = 0.95f});
        Random random{0x5eedu};
        for (int i = 0; i < 14; ++i) {
            const float side = i % 2 == 0 ? -1.0f : 1.0f;
            const float z = -30.0f - 13.0f * float(i);
            const float r = random.uniform(6.0f, 14.0f);
            s.addInstance(
                {.mesh = *hill,
                 .material = grass,
                 .transform = placed({side * random.uniform(18.0f, 40.0f), -r * 0.45f, z}, glm::vec3(r, r * 0.6f, r))});
        }
        m_sun = s.addLight({.type = LightType::Directional});
        apply();
        return {};
    }

    void update(Context& c, double frameDt) override {
        (void)c;
        if (!m_animateDay) return;
        m_elevation += float(frameDt) * 6.0f;
        if (m_elevation > 90.0f) m_elevation = -12.0f;
        apply();
    }

    void controls(Context& c) override {
        (void)c;
        bool changed = false;
        changed |= ImGui::SliderFloat("Sun elevation", &m_elevation, -12.0f, 90.0f, "%.0f deg");
        changed |= ImGui::SliderFloat("Sun azimuth", &m_azimuth, 0.0f, 360.0f, "%.0f deg");
        ImGui::Checkbox("Run the day", &m_animateDay);
        ImGui::Checkbox("Sky (off: clear color)", &settings.sky);
        changed |= ImGui::Checkbox("Atmosphere (off: gradient)", &m_atmosphere);
        changed |= ImGui::Checkbox("Sun disc", &m_sunDisc);
        ImGui::Separator();
        changed |= ImGui::SliderFloat("Fog density", &m_fogDensity, 0.0f, 0.08f, "%.3f /m");
        changed |= ImGui::SliderFloat("Height falloff", &m_fogFalloff, 0.0f, 0.5f, "%.2f /m");
        changed |= ImGui::SliderFloat("Fog base (m)", &m_fogBase, -10.0f, 20.0f, "%.1f");
        changed |= ImGui::SliderFloat("Fog maximum", &m_fogMax, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("Sun scatter", &m_fogScatter, 0.0f, 1.0f);
        if (changed) apply();
    }

    void stats(Context& c) override {
        (void)c;
        ImGui::TextWrapped("Exposure %.2f (from the palette)", double(m_scene->environment().exposure));
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        if (name == "sun_elevation") return double(m_elevation);
        if (name == "fog_density") return double(m_fogDensity);
        if (name == "atmosphere") return double(m_scene->environment().atmosphere);
        if (name == "exposure") return double(m_scene->environment().exposure);
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        (void)c;
        const std::optional<float> number = parseFloat(value);
        const std::optional<bool> flag = parseBool(value);
        if (name == "sun_elevation" && number) {
            m_elevation = std::clamp(*number, -12.0f, 90.0f);
        } else if (name == "sun_azimuth" && number) {
            m_azimuth = *number;
        } else if (name == "fog" && number) {
            m_fogDensity = std::max(*number, 0.0f);
        } else if (name == "fog_falloff" && number) {
            m_fogFalloff = std::max(*number, 0.0f);
        } else if (name == "atmosphere" && flag) {
            m_atmosphere = *flag;
        } else if (name == "sun_disc" && flag) {
            m_sunDisc = *flag;
        } else if (name == "animate" && flag) {
            m_animateDay = *flag;
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
        const glm::vec3 towardSun = skyDirection(m_elevation, m_azimuth);
        Daylight day = daylightFor(towardSun);
        Environment& e = day.environment;
        e.atmosphere = m_atmosphere ? 1.0f : 0.0f;
        if (!m_sunDisc) e.sunDisc = glm::vec3(0.0f);
        e.fogDensity = m_fogDensity;
        e.fogFalloff = m_fogFalloff;
        e.fogBaseHeight = m_fogBase;
        e.fogMaxAmount = m_fogMax;
        e.fogSunScatter = m_fogScatter;
        m_scene->setEnvironment(e);
        m_scene->setLight(m_sun, {.type = LightType::Directional, .color = day.sunColor, .direction = -towardSun});
    }

    std::optional<Scene> m_scene;
    LightId m_sun;
    float m_elevation = 9.0f;
    float m_azimuth = 35.0f;
    bool m_animateDay = false;
    bool m_atmosphere = true;
    bool m_sunDisc = true;
    float m_fogDensity = 0.006f;
    float m_fogFalloff = 0.1f;
    float m_fogBase = 0.0f;
    float m_fogMax = 0.9f;
    float m_fogScatter = 0.6f;
};

} // namespace

std::unique_ptr<Section> makeSkySection() { return std::make_unique<SkySection>(); }

} // namespace demo
