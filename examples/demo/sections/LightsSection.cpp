// Many lights: a dark hall with up to 96 small lamps wandering among pillars and spheres. A frame takes the 64
// nearest local lights; the renderer lays the screen out in 20 x 12 tiles and a surface shades only the lamps that
// reach its tile (RenderSettings::lightTiles). Switched off, every pixel shades every lamp: the same picture, the
// cost of all of them.

#include "Demo.h"

#include <glm/ext/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;

constexpr int kMaxLamps = 96;
constexpr glm::vec2 kHall{15.0f, 10.0f}; // half size of the floor
constexpr std::array<glm::vec3, 8> kColors{glm::vec3(0.10f, 0.85f, 0.80f), glm::vec3(1.00f, 0.55f, 0.10f),
                                           glm::vec3(0.85f, 0.10f, 0.70f), glm::vec3(0.55f, 1.00f, 0.15f),
                                           glm::vec3(0.25f, 0.45f, 1.00f), glm::vec3(1.00f, 0.15f, 0.12f),
                                           glm::vec3(1.00f, 0.90f, 0.35f), glm::vec3(0.75f, 0.80f, 1.00f)};

class LightsSection final : public Section {
public:
    LightsSection() {
        camera.target = glm::vec3(0.0f, 0.6f, 0.0f);
        camera.distance = 26.0f;
        camera.yaw = glm::radians(30.0f);
        camera.pitch = glm::radians(42.0f);
        camera.maxDistance = 70.0f;
        settings.shadowCascades = 0; // no sun in the hall
        settings.bloomStrength = 0.1f;
    }

    std::string_view id() const override { return "lights"; }
    std::string_view title() const override { return "Many lights"; }
    std::string_view about() const override {
        return "64 small lamps in a dark hall, each lighting a few metres around it. The screen is cut into 20 x 12 "
               "tiles and a surface shades only the lamps that reach its tile; switch Tiles off and every pixel shades "
               "all of them: the same picture, at the cost the numbers show. Past 64 lamps the nearest 64 light.";
    }

    Result<void> init(Context& c) override {
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> floor = addMesh(s, planeMesh(kHall + glm::vec2(4.0f), 17.0f));
        const Result<MeshId> box = addMesh(s, boxMesh(glm::vec3(0.5f)));
        const Result<MeshId> ball = addMesh(s, sphereMesh(0.5f, 24, 48));
        const Result<MeshId> bulb = addMesh(s, sphereMesh(0.5f, 8, 12));
        for (const Result<MeshId>* m : {&floor, &box, &ball, &bulb})
            if (!*m) return Error{m->error()};

        s.addInstance({.mesh = *floor,
                       .material = addCheckerMaterial(c, s, glm::vec3(0.42f, 0.44f, 0.48f),
                                                      glm::vec3(0.30f, 0.32f, 0.36f), 2.0f, 0.55f)});
        // Pillars in rows, a few metal and plastic spheres between them: something for the lamps to light and shine in.
        const MaterialId stone = s.addMaterial({.baseColor = glm::vec4(0.55f, 0.56f, 0.6f, 1.0f), .roughness = 0.75f});
        const MaterialId steel =
            s.addMaterial({.baseColor = glm::vec4(0.8f, 0.82f, 0.85f, 1.0f), .roughness = 0.25f, .metallic = 1.0f});
        const MaterialId plastic = s.addMaterial({.baseColor = glm::vec4(0.9f, 0.9f, 0.88f, 1.0f), .roughness = 0.4f});
        for (int z = 0; z < 3; ++z)
            for (int x = 0; x < 5; ++x)
                s.addInstance({.mesh = *box,
                               .material = stone,
                               .transform = placed({(float(x) - 2.0f) * 6.0f, 1.5f, (float(z) - 1.0f) * 6.0f},
                                                   glm::vec3(0.8f, 3.0f, 0.8f)),
                               .castsShadows = false});
        for (int z = 0; z < 2; ++z)
            for (int x = 0; x < 4; ++x)
                s.addInstance(
                    {.mesh = *ball,
                     .material = (x + z) % 2 ? steel : plastic,
                     .transform = placed({(float(x) - 1.5f) * 6.0f, 0.7f, (float(z) - 0.5f) * 6.0f}, glm::vec3(1.4f)),
                     .castsShadows = false});

        // The hall has no sky: black outside, a trace of ambient light.
        Environment environment;
        environment.skyZenith = environment.skyHorizon = environment.skyGround = glm::vec3(0.003f, 0.004f, 0.006f);
        environment.sunGlow = environment.sunDisc = glm::vec3(0.0f);
        environment.atmosphere = 0.0f;
        environment.skyAmbient = glm::vec3(0.02f, 0.024f, 0.03f);
        environment.groundAmbient = glm::vec3(0.01f);
        s.setEnvironment(environment);

        // Every lamp: a glowing bulb and a point light on a path of its own.
        std::array<MaterialId, kColors.size()> glows;
        for (std::size_t i = 0; i < kColors.size(); ++i)
            glows[i] = s.addMaterial(
                {.baseColor = glm::vec4(kColors[i] * 0.2f, 1.0f), .emissive = kColors[i] * 7.0f, .roughness = 0.5f});
        Random random;
        for (int i = 0; i < kMaxLamps; ++i) {
            Lamp lamp;
            lamp.color = kColors[std::size_t(i) % kColors.size()];
            lamp.center.x = random.uniform(-kHall.x + 2.0f, kHall.x - 2.0f);
            lamp.center.y = random.uniform(-kHall.y + 2.0f, kHall.y - 2.0f);
            lamp.radius.x = random.uniform(0.8f, 2.6f);
            lamp.radius.y = random.uniform(0.8f, 2.6f);
            lamp.speed = random.uniform(0.25f, 0.8f) * (i % 2 ? 1.0f : -1.0f);
            lamp.phase = random.uniform(0.0f, 6.2831853f);
            lamp.height = random.uniform(0.5f, 1.7f);
            lamp.bulb = s.addInstance(
                {.mesh = *bulb, .material = glows[std::size_t(i) % kColors.size()], .castsShadows = false});
            lamp.light = s.addLight({.type = LightType::Point, .intensity = 0.0f, .castsShadows = false});
            m_lamps.push_back(lamp);
        }
        animate(0.0);
        return {};
    }

    void update(Context& c, double frameDt) override {
        (void)c;
        if (m_animate) m_time += frameDt;
        animate(m_time);
    }

    void controls(Context& c) override {
        (void)c;
        ImGui::SetNextItemWidth(itemWidth());
        ImGui::SliderInt("Lamps", &m_count, 0, kMaxLamps);
        ImGui::SetNextItemWidth(itemWidth());
        ImGui::SliderFloat("Reach (m)", &m_range, 1.0f, 12.0f, "%.1f");
        ImGui::SetNextItemWidth(itemWidth());
        ImGui::SliderFloat("Brightness", &m_intensity, 0.5f, 12.0f, "%.1f");
        ImGui::Checkbox("Tiles (shade only the lamps in reach)", &settings.lightTiles);
        ImGui::Checkbox("Move the lamps", &m_animate);
    }

    void stats(Context& c) override {
        const RenderStats& r = c.renderer.stats();
        ImGui::TextWrapped("%d lamps, %u light the frame", m_count, r.lights);
        ImGui::TextWrapped("Lamps a pixel shades: %.1f on average over the tiles (%u without tiles)",
                           double(r.lightTileRefs) / 240.0, r.lights);
        if (!c.app.deterministic() && c.app.stats().gpuMs >= 0.0)
            ImGui::TextWrapped("GPU %.2f ms a frame", c.app.stats().gpuMs);
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        if (name == "lamps") return double(m_count);
        if (name == "light_tiles") return settings.lightTiles ? 1.0 : 0.0;
        if (name == "reach") return double(m_range);
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        (void)c;
        const std::optional<float> number = parseFloat(value);
        const std::optional<bool> flag = parseBool(value);
        if (name == "lamps" && number) {
            m_count = std::clamp(int(*number), 0, kMaxLamps);
        } else if (name == "reach" && number) {
            m_range = std::clamp(*number, 0.5f, 30.0f);
        } else if (name == "brightness" && number) {
            m_intensity = std::clamp(*number, 0.0f, 50.0f);
        } else if (name == "light_tiles" && flag) {
            settings.lightTiles = *flag;
        } else if (name == "animate" && flag) {
            m_animate = *flag;
        } else {
            return false;
        }
        animate(m_time);
        return true;
    }

    std::string perfDetails() const override {
        return "lamps=" + std::to_string(m_count) + " light_tiles=" + (settings.lightTiles ? "1" : "0");
    }

    void shutdown(Context& c) override {
        (void)c;
        m_lamps.clear();
        m_scene.reset();
    }

private:
    struct Lamp {
        glm::vec3 color{1.0f};
        glm::vec2 center{0.0f}, radius{1.0f};
        float speed = 0.5f, phase = 0.0f, height = 1.0f;
        InstanceId bulb;
        LightId light;
    };

    void animate(double time) {
        const float t = float(time);
        for (std::size_t i = 0; i < m_lamps.size(); ++i) {
            const Lamp& lamp = m_lamps[i];
            const bool on = int(i) < m_count;
            const float a = lamp.phase + t * lamp.speed;
            const glm::vec3 position(lamp.center.x + std::cos(a) * lamp.radius.x,
                                     lamp.height + 0.2f * std::sin(a * 2.3f),
                                     lamp.center.y + std::sin(a * 1.3f) * lamp.radius.y);
            m_scene->setVisible(lamp.bulb, on);
            m_scene->setTransform(lamp.bulb, placed(position, glm::vec3(0.22f)));
            m_scene->setLight(lamp.light, {.type = LightType::Point,
                                           .color = lamp.color,
                                           .intensity = on ? m_intensity : 0.0f,
                                           .position = position,
                                           .range = m_range,
                                           .castsShadows = false});
        }
    }

    std::optional<Scene> m_scene;
    std::vector<Lamp> m_lamps;
    double m_time = 0.0;
    int m_count = 64;
    float m_range = 4.0f;
    float m_intensity = 3.0f;
    bool m_animate = true;
};

} // namespace

std::unique_ptr<Section> makeLightsSection() { return std::make_unique<LightsSection>(); }

} // namespace demo
