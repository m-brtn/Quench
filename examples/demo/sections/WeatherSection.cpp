// Weather (docs/weather.md): the clouds of a quench_weather field over a plain kilometres wide, seen from a few
// hundred metres up: fair-weather cumulus to towering cumulus, cumulonimbus and an overcast stratus deck, high cirrus,
// one raining cumulonimbus placed as a cell in a blue sky, all drifting with the wind and changing shape; their
// shadows on the ground, the sun dimming and the sky's light greying as the cover closes, the sky cube with the clouds
// in the lake's reflection, sunsets from the sky's own light. Quality: volumetric clouds at the render size or at half
// of it with accumulation over frames, or the 2D cloud layer (the fallback).

#include "Demo.h"

#include <quench/render/Daylight.h>
#include <quench/weather/Weather.h>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;
namespace wx = quench::weather;

// Settings of the clouds, in the order of the panel's quality list.
enum class Quality : int { Full, Reduced, Layer, Off };
constexpr std::array<const char*, 4> kQualityNames{"Volumetric, full size", "Volumetric, half size", "2D layer", "Off"};
constexpr std::array<const char*, 4> kQualityIds{"full", "reduced", "layer", "off"};
constexpr std::array<const char*, 4> kTypeNames{"Stratus", "Cumulus", "Towering cumulus", "Cumulonimbus"};
constexpr std::array<const char*, 6> kPresets{"fair", "shower", "towering", "storm", "overcast", "cirrus"};
constexpr std::array<const char*, 6> kPresetNames{"Fair weather", "Sun shower", "Towering",
                                                  "Storm",        "Overcast",   "Cirrus"};
// The weather's clock at the start: the field after 20 minutes, so the clouds are not at their seed's first shape.
constexpr double kStartTime = 1200.0;
// The single raining cloud: ahead of the camera, right of the view's centre; its shadow falls ahead on the left.
constexpr glm::vec2 kShowerAt{2600.0f, -10500.0f};

class WeatherSection final : public Section {
public:
    WeatherSection() {
        // A few hundred metres up, looking a little above the horizon: the clouds, their shadows on the plain, the
        // lake.
        camera.target = glm::vec3(0.0f, 320.0f, -600.0f);
        camera.distance = 600.0f;
        camera.yaw = 0.0f;
        camera.pitch = glm::radians(-10.0f);
        camera.minDistance = 30.0f;
        camera.maxDistance = 3000.0f;
        settings.shadowDistance = 2500.0f;
        settings.clearColor = glm::vec3(0.25f, 0.3f, 0.36f);
        applyQuality();
    }

    std::string_view id() const override { return "weather"; }
    std::string_view title() const override { return "Weather"; }
    std::string_view about() const override {
        return "Clouds from a weather map that the wind moves and that changes shape: cumulus to cumulonimbus, an "
               "overcast deck, cirrus, a single raining cloud in a blue sky. Their shadows cross the plain, the sun "
               "dims and the light greys as the cover closes, the lake reflects them, and the sky's own light tints "
               "them at sunset. Volumetric clouds, or the 2D cloud layer as the cheap setting and fallback.";
    }

    Result<void> init(Context& c) override {
        // Phones start at half size with accumulation, fewer steps and one sky cube face per frame, at 60 frames per
        // second (docs/performance.md, "Clouds").
        if (c.app.mobile()) {
            m_quality = Quality::Reduced;
            m_steps = 48;
            m_cubeFaces = 1;
            m_limit60 = true;
            applyQuality();
        }
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> ground = addMesh(s, planeMesh(glm::vec2(20000.0f), 500.0f));
        const Result<MeshId> lake = addMesh(s, planeMesh(glm::vec2(320.0f, 200.0f)));
        const Result<MeshId> hill = addMesh(s, sphereMesh(1.0f, 24, 48));
        const Result<MeshId> tower = addMesh(s, boxMesh(glm::vec3(8.0f, 60.0f, 8.0f)));
        for (const Result<MeshId>* m : {&ground, &lake, &hill, &tower})
            if (!*m) return Error{m->error()};
        s.addInstance({.mesh = *ground,
                       .material = addCheckerMaterial(c, s, glm::vec3(0.30f, 0.38f, 0.20f),
                                                      glm::vec3(0.26f, 0.33f, 0.17f), 40.0f, 0.95f)});
        const MaterialId water =
            s.addMaterial({.baseColor = glm::vec4(0.02f, 0.04f, 0.05f, 1.0f), .roughness = 0.04f, .name = "lake"});
        s.addInstance({.mesh = *lake, .material = water, .transform = placed({-150.0f, 0.4f, -1100.0f})});
        const MaterialId grass = s.addMaterial({.baseColor = glm::vec4(0.25f, 0.32f, 0.17f, 1.0f), .roughness = 0.95f});
        const MaterialId rock = s.addMaterial({.baseColor = glm::vec4(0.42f, 0.4f, 0.36f, 1.0f), .roughness = 0.9f});
        Random random{0x3a7e2u};
        for (int i = 0; i < 48; ++i) {
            const float angle = random.uniform(0.0f, 6.2831853f);
            const float distance = random.uniform(1500.0f, 14000.0f);
            const float r = random.uniform(120.0f, 520.0f);
            const glm::vec3 at{std::sin(angle) * distance, -r * 0.55f, -std::cos(angle) * distance - 2000.0f};
            s.addInstance({.mesh = *hill,
                           .material = i % 3 == 0 ? rock : grass,
                           .transform = placed(at, glm::vec3(r, r * random.uniform(0.6f, 0.9f), r))});
        }
        const MaterialId concrete =
            s.addMaterial({.baseColor = glm::vec4(0.62f, 0.6f, 0.56f, 1.0f), .roughness = 0.8f});
        for (int i = 0; i < 6; ++i)
            s.addInstance({.mesh = *tower,
                           .material = concrete,
                           .transform = placed({-420.0f + 170.0f * float(i), 60.0f, -760.0f - 90.0f * float(i % 2)})});
        m_sun = s.addLight({.type = LightType::Directional});
        preset("fair");
        m_weather.setTime(kStartTime);
        applySun();
        s.setWeather(m_weather);
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
        m_stats = c.renderer.stats();
        const double dt = std::min(frameDt, 0.1);
        m_weather.update(dt * double(m_timeScale));
        if (m_runDay) {
            m_elevation += float(dt) * 3.0f;
            if (m_elevation > 70.0f) m_elevation = -4.0f;
        }
        applySun();
        m_scene->setWeather(m_weather);
    }

    void controls(Context& c) override {
        (void)c;
        ImGui::TextUnformatted("Weather");
        for (std::size_t i = 0; i < kPresets.size(); ++i) {
            if (i % 3 != 0) ImGui::SameLine();
            if (ImGui::Button(kPresetNames[i])) preset(kPresets[i]);
        }
        wx::Settings w = m_weather.settings();
        bool changed = false;
        changed |= ImGui::SliderFloat("Coverage", &w.coverage, 0.0f, 1.0f);
        int type = int(w.type);
        ImGui::SetNextItemWidth(itemWidth());
        if (ImGui::Combo("Cloud type", &type, kTypeNames.data(), int(kTypeNames.size()))) {
            w.type = wx::CloudType(type);
            changed = true;
        }
        changed |= ImGui::SliderFloat("Type variation", &w.typeVariation, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("Precipitation", &w.precipitation, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("Cirrus", &w.cirrus, 0.0f, 1.0f);
        bool shower = m_shower != wx::kNoCell;
        if (ImGui::Checkbox("A raining cumulonimbus", &shower)) setShower(shower);
        ImGui::Separator();
        changed |= ImGui::SliderFloat("Wind (m/s)", &w.wind.speed, 0.0f, 40.0f, "%.0f");
        if (ImGui::SliderFloat("Wind from (deg)", &m_windFrom, 0.0f, 360.0f, "%.0f")) {
            w.wind.direction = windDirection(m_windFrom);
            changed = true;
        }
        changed |= ImGui::SliderFloat("Change of shape", &w.evolution, 0.0f, 4.0f);
        ImGui::SliderFloat("Time scale", &m_timeScale, 0.0f, 60.0f, "%.0f x");
        if (changed) m_weather.setSettings(w);
        ImGui::Separator();
        ImGui::SliderFloat("Sun elevation", &m_elevation, -4.0f, 80.0f, "%.0f deg");
        ImGui::SliderFloat("Sun azimuth", &m_azimuth, 0.0f, 360.0f, "%.0f deg");
        ImGui::Checkbox("Run the day", &m_runDay);
        if (ImGui::Button("Sunset")) {
            m_elevation = 3.0f;
            m_azimuth = 25.0f;
        }
        ImGui::Separator();
        int quality = int(m_quality);
        ImGui::SetNextItemWidth(itemWidth());
        if (ImGui::Combo("Clouds", &quality, kQualityNames.data(), int(kQualityNames.size()))) {
            m_quality = Quality(quality);
            applyQuality();
        }
        if (ImGui::SliderInt("March steps", &m_steps, 16, 128)) applyQuality();
        ImGui::Checkbox("Cloud shadows", &settings.cloudShadows);
        bool cube = settings.skyCubeSize > 0;
        if (ImGui::Checkbox("Sky cube (reflections)", &cube)) settings.skyCubeSize = cube ? 64u : 0u;
        if (ImGui::SliderInt("Cube faces per frame", &m_cubeFaces, 1, 6)) applyQuality();
        if (ImGui::Checkbox("60 Hz (less heat)", &m_limit60)) applyFrameLimit(c);
    }

    void stats(Context& c) override {
        (void)c;
        const char* drawn = m_stats.clouds == Clouds::Volumetric ? "volumetric"
                            : m_stats.clouds == Clouds::Layer    ? "2D layer"
                                                                 : "none";
        ImGui::TextWrapped("Clouds %s%s", drawn,
                           m_stats.clouds == Clouds::Volumetric
                               ? std::format(", marched at {} x {}", m_stats.cloudWidth, m_stats.cloudHeight).c_str()
                               : "");
        ImGui::TextWrapped("Overcast %.0f %% around the camera, sky cube faces %u", double(m_stats.cloudCover) * 100.0,
                           m_stats.skyCubeFaces);
        const glm::vec2 here = cameraGround();
        ImGui::TextWrapped("Precipitation below the camera %.2f, nearest %.1f km away",
                           double(m_weather.precipitation(glm::vec3(here.x, 0.0f, here.y))),
                           double(m_weather.distanceToPrecipitation(here)) / 1000.0);
        ImGui::TextWrapped("Weather time %.0f s", m_weather.time());
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        if (name == "clouds") return double(int(m_stats.clouds));
        if (name == "cloud_width") return double(m_stats.cloudWidth);
        if (name == "cloud_height") return double(m_stats.cloudHeight);
        if (name == "cloud_cover") return double(m_stats.cloudCover);
        if (name == "sky_cube_faces") return double(m_stats.skyCubeFaces);
        if (name == "coverage") return double(m_weather.settings().coverage);
        if (name == "cloud_type") return double(int(m_weather.settings().type));
        if (name == "cells") return double(m_weather.cellCount());
        if (name == "weather_time") return m_weather.time();
        if (name == "sun_elevation") return double(m_elevation);
        if (name == "limit_60hz") return m_limit60 ? 1.0 : 0.0;
        if (name == "rain_here" || name == "rain_distance") {
            const glm::vec2 here = cameraGround();
            return name == "rain_here" ? double(m_weather.precipitation(glm::vec3(here.x, 0.0f, here.y)))
                                       : double(m_weather.distanceToPrecipitation(here));
        }
        if (name == "shower_rain") {
            const std::optional<wx::Cell> cell = m_weather.cell(m_shower);
            return cell ? double(m_weather.sample(cell->position).precipitation) : 0.0;
        }
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        const std::optional<float> number = parseFloat(value);
        const std::optional<bool> flag = parseBool(value);
        wx::Settings w = m_weather.settings();
        if (name == "preset") {
            return preset(value);
        } else if (name == "quality") {
            const auto it = std::find(kQualityIds.begin(), kQualityIds.end(), value);
            if (it == kQualityIds.end()) return false;
            m_quality = Quality(it - kQualityIds.begin());
            applyQuality();
            return true;
        } else if (name == "coverage" && number) {
            w.coverage = std::clamp(*number, 0.0f, 1.0f);
        } else if (name == "cloud_type" && number) {
            w.type = wx::CloudType(std::clamp(int(*number), 0, 3));
        } else if (name == "type_variation" && number) {
            w.typeVariation = std::clamp(*number, 0.0f, 1.0f);
        } else if (name == "precipitation" && number) {
            w.precipitation = std::clamp(*number, 0.0f, 1.0f);
        } else if (name == "cirrus" && number) {
            w.cirrus = std::clamp(*number, 0.0f, 1.0f);
        } else if (name == "wind_speed" && number) {
            w.wind.speed = std::max(*number, 0.0f);
        } else if (name == "wind_from" && number) {
            m_windFrom = *number;
            w.wind.direction = windDirection(m_windFrom);
        } else if (name == "evolution" && number) {
            w.evolution = std::max(*number, 0.0f);
        } else if (name == "seed" && number) {
            w.seed = std::uint32_t(std::max(*number, 0.0f));
        } else if (name == "shower" && flag) {
            setShower(*flag);
            return true;
        } else if (name == "time_scale" && number) {
            m_timeScale = std::max(*number, 0.0f);
            return true;
        } else if (name == "weather_time" && number) {
            m_weather.setTime(double(*number));
            if (m_shower != wx::kNoCell) setShower(true); // back where it was placed
            return true;
        } else if (name == "sun_elevation" && number) {
            m_elevation = std::clamp(*number, -4.0f, 80.0f);
            applySun();
            return true;
        } else if (name == "sun_azimuth" && number) {
            m_azimuth = *number;
            applySun();
            return true;
        } else if (name == "run_day" && flag) {
            m_runDay = *flag;
            return true;
        } else if (name == "steps" && number) {
            m_steps = std::clamp(int(*number), 16, 128);
            applyQuality();
            return true;
        } else if (name == "cloud_shadows" && flag) {
            settings.cloudShadows = *flag;
            return true;
        } else if (name == "sky_cube" && flag) {
            settings.skyCubeSize = *flag ? 64u : 0u;
            return true;
        } else if (name == "cube_faces" && number) {
            m_cubeFaces = std::clamp(int(*number), 1, 6);
            applyQuality();
            return true;
        } else if (name == "limit_60hz" && flag) {
            m_limit60 = *flag;
            applyFrameLimit(c);
            return true;
        } else {
            return false;
        }
        m_weather.setSettings(w);
        return true;
    }

    std::string perfDetails() const override {
        return std::format("clouds={} steps={} cube_faces={} limit_60hz={}", kQualityIds[std::size_t(m_quality)],
                           m_steps, m_cubeFaces, m_limit60 ? 1 : 0);
    }

    void shutdown(Context& c) override {
        (void)c;
        m_scene.reset();
    }

private:
    bool preset(std::string_view name) {
        wx::Settings w = m_weather.settings();
        w.typeVariation = 0.15f;
        w.precipitation = 0.0f;
        w.cirrus = 0.15f;
        bool shower = false;
        if (name == "fair") {
            w.coverage = 0.32f;
            w.type = wx::CloudType::Cumulus;
            shower = true;
        } else if (name == "shower") {
            w.coverage = 0.05f;
            w.type = wx::CloudType::Cumulus;
            shower = true;
        } else if (name == "towering") {
            w.coverage = 0.42f;
            w.type = wx::CloudType::Towering;
            w.typeVariation = 0.3f;
            w.precipitation = 0.3f;
            w.cirrus = 0.3f;
        } else if (name == "storm") {
            w.coverage = 0.8f;
            w.type = wx::CloudType::Cumulonimbus;
            w.typeVariation = 0.35f;
            w.precipitation = 0.9f;
            w.cirrus = 0.0f;
        } else if (name == "overcast") {
            w.coverage = 1.0f;
            w.type = wx::CloudType::Stratus;
            w.typeVariation = 0.1f;
            w.precipitation = 0.3f;
            w.cirrus = 0.0f;
        } else if (name == "cirrus") {
            w.coverage = 0.03f;
            w.type = wx::CloudType::Cumulus;
            w.cirrus = 0.9f;
        } else {
            return false;
        }
        m_weather.setSettings(w);
        setShower(shower);
        return true;
    }

    // The single raining cloud, placed ahead of the camera's start (drifting with the wind from there).
    void setShower(bool on) {
        if (m_shower != wx::kNoCell) m_weather.removeCell(m_shower);
        m_shower = wx::kNoCell;
        if (on)
            m_shower = m_weather.addCell({.position = kShowerAt,
                                          .radius = 3200.0f,
                                          .type = wx::CloudType::Cumulonimbus,
                                          .coverage = 1.0f,
                                          .precipitation = 1.0f});
    }

    static glm::vec2 windDirection(float fromDegrees) {
        const float a = glm::radians(fromDegrees);
        return {-std::sin(a), std::cos(a)}; // from the north (-z) at 0, blowing toward +z
    }

    glm::vec2 cameraGround() const {
        const glm::vec3 eye = camera.eye();
        return {eye.x, eye.z};
    }

    void applyQuality() {
        settings.cloudSteps = m_steps;
        settings.skyCubeFacesPerFrame = m_cubeFaces;
        switch (m_quality) {
        case Quality::Full:
            settings.clouds = Clouds::Volumetric;
            settings.cloudResolution = 1.0f;
            settings.cloudTemporal = true;
            break;
        case Quality::Reduced:
            settings.clouds = Clouds::Volumetric;
            settings.cloudResolution = 0.5f;
            settings.cloudTemporal = true;
            break;
        case Quality::Layer: settings.clouds = Clouds::Layer; break;
        case Quality::Off: settings.clouds = Clouds::Off; break;
        }
    }

    // The daylight palette for the sun, its exposure raised as the cover closes (a photographer's compensation:
    // the renderer leaves exposure to the game).
    void applySun() {
        const glm::vec3 towardSun = skyDirection(m_elevation, m_azimuth);
        Daylight day = daylightFor(towardSun);
        Environment& e = day.environment;
        e.fogDensity = 0.00012f;
        e.fogFalloff = 0.0012f;
        e.fogMaxAmount = 0.8f;
        e.fogSunScatter = 0.5f;
        const glm::vec3 eye = camera.eye();
        const float cover = m_quality == Quality::Off ? 0.0f : m_weather.cover({eye.x, eye.z}, 6000.0f);
        e.exposure *= 1.0f + 0.9f * cover;
        m_scene->setEnvironment(e);
        m_scene->setLight(m_sun, {.type = LightType::Directional, .color = day.sunColor, .direction = -towardSun});
    }

    // 60 frames per second while the section is current, unless the app already runs slower (--max-frame-rate).
    void applyFrameLimit(Context& c) {
        double rate = m_outsideRate;
        if (m_limit60 && (rate <= 0.0 || rate > 60.0)) rate = 60.0;
        if (c.app.maxFrameRate() != rate) c.app.setMaxFrameRate(rate);
    }

    std::optional<Scene> m_scene;
    LightId m_sun;
    wx::Weather m_weather{
        wx::Settings{.seed = 7, .wind = {.direction = {0.0f, 1.0f}, .speed = 12.0f}, .evolution = 1.0f}};
    wx::CellId m_shower = wx::kNoCell;
    RenderStats m_stats{};
    Quality m_quality = Quality::Reduced;
    int m_steps = 64;
    int m_cubeFaces = 2;
    float m_windFrom = 0.0f;
    float m_timeScale = 10.0f;
    float m_elevation = 30.0f;
    float m_azimuth = 50.0f;
    bool m_runDay = false;
    bool m_limit60 = false;
    double m_outsideRate = 0.0;
};

} // namespace

std::unique_ptr<Section> makeWeatherSection() { return std::make_unique<WeatherSection>(); }

} // namespace demo
