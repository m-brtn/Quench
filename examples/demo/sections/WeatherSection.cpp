// Weather (docs/weather.md): the clouds of a quench_weather field over a plain kilometres wide, seen from a few
// hundred metres up: fair-weather cumulus to towering cumulus, cumulonimbus and an overcast stratus deck, high cirrus,
// one raining cumulonimbus placed as a cell in a blue sky, all drifting with the wind and changing shape; their
// shadows on the ground, the sun dimming and the sky's light greying as the cover closes, the sky cube with the clouds
// in the lake's reflection, sunsets from the sky's own light, rain shafts under raining clouds. Quality: volumetric
// clouds at the render size or at half of it with accumulation over frames, or the 2D cloud layer (the fallback).
// The rain (stage 13 part 2) at a place on the ground behind the sky view's camera: a paved square with a shelter, a
// pond and a dark block behind them, wet where the rain reaches, puddles and rings on the water, the rain's streaks
// and splashes, a rainbow in a sun shower, and the engine's rain sound (audio::WeatherAudio) by the rain at the camera,
// the distance to the nearest rain and the roof over it. The snow (part 3) by the weather's temperature: flakes that
// drift with the wind, a cover that builds on the paving, the roof and the crates while it snows (none under the
// shelter) and melts in a thaw, and snow showers seen from the air. The wind (part 4): a meadow beside the place, its
// grass, bushes and trees swaying in the weather's wind (quench/Wind.hlsli, shaders/plants.hlsl) as its gusts sweep
// across them, rain and snow carried by the same gusts, lightning in a storm's clouds that lights the scene, and the
// wind's and the thunder's sound.

#include "Demo.h"

#include <quench/audio/WeatherAudio.h>
#include <quench/render/Daylight.h>
#include <quench/weather/Weather.h>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>
#include <vector>

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
constexpr std::array<const char*, 13> kPresets{"fair",   "shower",       "towering",   "storm",   "overcast",
                                               "cirrus", "rain_here",    "downpour",   "passing", "snowfall",
                                               "thaw",   "snow_showers", "windy_storm"};
constexpr std::array<const char*, 13> kPresetNames{
    "Fair weather", "Sun shower",     "Towering", "Storm", "Overcast",     "Cirrus",     "Rain here",
    "Downpour",     "Passing shower", "Snowfall", "Thaw",  "Snow showers", "Windy storm"};
enum class View : int { Sky, Ground, Cloud, Shelter, Above, Meadow };
constexpr std::array<const char*, 6> kViewNames{"From the air",   "On the ground",    "Toward the cloud",
                                                "Under the roof", "Over the shelter", "In the meadow"};
constexpr std::array<const char*, 6> kViewIds{"sky", "ground", "cloud", "shelter", "above", "meadow"};
constexpr std::array<const char*, 4> kRainNames{"Off", "Low", "Medium", "High"};
constexpr std::array<const char*, 4> kRainIds{"off", "low", "medium", "high"};
// The weather's temperature at the ground in the rain and cloud presets (the standard atmosphere's), and in the snow
// presets.
constexpr float kMildTemperature = 15.0f;
constexpr float kSnowTemperature = -4.0f;
// The wind at the cloud base (m/s; about half of it near the ground): the presets', and a lighter one for the
// snowfall and the thaw, so the flakes drift rather than race.
constexpr float kWindSpeed = 12.0f;
constexpr float kSnowWindSpeed = 2.0f;
// The weather's clock at the start: the field after 20 minutes, so the clouds are not at their seed's first shape.
constexpr double kStartTime = 1200.0;
// The single raining cloud: ahead of the camera, right of the view's centre; its shadow falls ahead on the left.
constexpr glm::vec2 kShowerAt{2600.0f, -10500.0f};
// The place on the ground for the rain: behind the sky view's camera, out of its picture.
constexpr glm::vec3 kPlace{0.0f, 0.0f, 300.0f};
constexpr glm::vec3 kShelter = kPlace + glm::vec3(5.0f, 0.0f, -12.0f);
// Rain here: one raining cumulonimbus standing behind the place, the camera in the rain at its edge; ahead a blue sky
// and the low sun shining under the cloud onto the rain (a sun shower; toward the cloud, a rainbow in its rain).
constexpr glm::vec2 kRainHereAt{-150.0f, 600.0f};
// Passing shower: a cell three kilometres ahead that the wind brings over the place and on.
constexpr glm::vec2 kPassingAt{0.0f, -2700.0f};
// The meadow for the wind: east of the place, out of the other views' pictures. Its grass, bushes and trees sway
// about their roots (InstanceDesc::custom: height, stiffness, flutter, patch; shaders/plants.hlsl).
constexpr glm::vec3 kMeadow{150.0f, 0.0f, 300.0f};
constexpr float kMeadowHalf = 16.0f;
constexpr float kPatchSize = 4.0f; // m, a square of grass in one instance
constexpr int kGrassPatches = 3;   // shapes of them, turned by quarters across the meadow
// The windy storm: a gale from the west-southwest with strong gusts, its thunderstorm's lightning four times as
// frequent as the weather's default.
constexpr float kGaleSpeed = 22.0f;
constexpr float kGaleFrom = 250.0f;
constexpr float kStormLightning = 4.0f;
// Thunder is heard from flashes within this distance.
constexpr float kThunderRadius = 25000.0f;
// The weather sound's layers as scenario values (sound_light, ...), in audio::WeatherLayer order.
constexpr std::array<std::string_view, 6> kSoundValues{"light", "heavy", "far", "cover", "wind_light", "wind_strong"};

class WeatherSection final : public Section {
public:
    WeatherSection() {
        settings.clearColor = glm::vec3(0.25f, 0.3f, 0.36f);
        settings.rain = Rain::Medium;
        settings.rainbow = true;
        settings.snow = Snow::Medium;
        settings.lightning = false;
        setView(View::Sky);
        applyQuality();
    }

    std::string_view id() const override { return "weather"; }
    std::string_view title() const override { return "Weather"; }
    std::string_view about() const override {
        return "Clouds from a weather map that the wind moves and that changes shape: cumulus to cumulonimbus, an "
               "overcast deck, cirrus, a single raining cloud in a blue sky. Their shadows cross the plain, the sun "
               "dims and the light greys as the cover closes, the lake reflects them, and the sky's own light tints "
               "them at sunset. On the ground: rain only where a cloud rains, none under the roof, wet paving, "
               "puddles and rings on the pond that dry after the shower, a rainbow, and the rain's sound. Below "
               "freezing it snows: flakes drifting in the wind, a cover building on the paving, the roof and the "
               "crates but not under the roof, melting in a thaw, and snow showers seen from the air. In the meadow "
               "the wind's gusts sweep across the grass, bushes and trees, drive the rain and the snow, and a windy "
               "storm flashes with lightning and rolls with thunder.";
    }

    Result<void> init(Context& c) override {
        // Phones start at half size with accumulation, fewer steps and one sky cube face per frame, at 60 frames per
        // second (docs/performance.md, "Clouds"), and with the rain's low quality without the shafts ("Rain").
        if (c.app.mobile()) {
            m_quality = Quality::Reduced;
            m_steps = 48;
            m_cubeFaces = 1;
            m_limit60 = true;
            settings.rain = Rain::Low;
            settings.rainShafts = false;
            settings.snow = Snow::Low;
            applyQuality();
        }
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> ground = addMesh(s, planeMesh(glm::vec2(20000.0f), 500.0f));
        const Result<MeshId> lake = addMesh(s, planeMesh(glm::vec2(320.0f, 200.0f)));
        const Result<MeshId> hill = addMesh(s, sphereMesh(1.0f, 24, 48));
        const Result<MeshId> tower = addMesh(s, boxMesh(glm::vec3(8.0f, 60.0f, 8.0f)));
        const Result<MeshId> paving = addMesh(s, boxMesh(glm::vec3(12.0f, 0.05f, 20.0f)));
        const Result<MeshId> pond = addMesh(s, planeMesh(glm::vec2(3.5f, 4.5f)));
        const Result<MeshId> post = addMesh(s, boxMesh(glm::vec3(0.06f, 1.3f, 0.06f)));
        const Result<MeshId> roof = addMesh(s, boxMesh(glm::vec3(2.6f, 0.06f, 2.0f)));
        const Result<MeshId> crate = addMesh(s, boxMesh(glm::vec3(0.4f)));
        const Result<MeshId> block = addMesh(s, boxMesh(glm::vec3(14.0f, 6.0f, 5.0f)));
        for (const Result<MeshId>* m : {&ground, &lake, &hill, &tower, &paving, &pond, &post, &roof, &crate, &block})
            if (!*m) return Error{m->error()};
        // Grass: half as wet as bare ground (the rain darkens it, its blades do not turn glossy).
        MaterialDesc turf{.surface = c.checker,
                          .baseColor = glm::vec4(0.28f, 0.355f, 0.185f, 1.0f),
                          .roughness = 0.95f,
                          .name = "checker",
                          .wetness = 0.5f};
        turf.params[0] = glm::vec4(0.30f, 0.38f, 0.20f, 0.95f);
        turf.params[1] = glm::vec4(0.26f, 0.33f, 0.17f, 0.95f * 0.9f);
        turf.params[2] = glm::vec4(40.0f, 0.0f, 0.0f, 0.0f);
        s.addInstance({.mesh = *ground, .material = s.addMaterial(turf)});
        const MaterialId water = s.addMaterial(
            {.baseColor = glm::vec4(0.02f, 0.04f, 0.05f, 1.0f), .roughness = 0.04f, .name = "lake", .water = true});
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

        // The rain's place: paving whose hollows fill with puddles, a pool, a shelter with a crate under its roof
        // (dry) and the same crate in the open (wet), and a dark block behind them to see the streaks against.
        const MaterialId stone = s.addMaterial(
            {.baseColor = glm::vec4(0.42f, 0.41f, 0.39f, 1.0f), .roughness = 0.75f, .name = "paving", .puddles = 1.0f});
        s.addInstance(
            {.mesh = *paving, .material = stone, .transform = placed(kPlace + glm::vec3(0.0f, 0.05f, -12.0f))});
        const MaterialId pondWater = s.addMaterial(
            {.baseColor = glm::vec4(0.02f, 0.035f, 0.04f, 1.0f), .roughness = 0.03f, .name = "pond", .water = true});
        s.addInstance(
            {.mesh = *pond, .material = pondWater, .transform = placed(kPlace + glm::vec3(-8.0f, 0.12f, -22.0f))});
        const MaterialId metal = s.addMaterial(
            {.baseColor = glm::vec4(0.3f, 0.32f, 0.34f, 1.0f), .roughness = 0.45f, .metallic = 1.0f, .name = "metal"});
        for (const glm::vec2 corner :
             {glm::vec2(-2.4f, -1.8f), glm::vec2(2.4f, -1.8f), glm::vec2(-2.4f, 1.8f), glm::vec2(2.4f, 1.8f)})
            s.addInstance({.mesh = *post,
                           .material = metal,
                           .transform = placed(kShelter + glm::vec3(corner.x, 1.4f, corner.y))});
        s.addInstance({.mesh = *roof, .material = metal, .transform = placed(kShelter + glm::vec3(0.0f, 2.76f, 0.0f))});
        const MaterialId wood =
            s.addMaterial({.baseColor = glm::vec4(0.55f, 0.38f, 0.22f, 1.0f), .roughness = 0.7f, .name = "crate"});
        s.addInstance({.mesh = *crate,
                       .material = wood,
                       .transform = placed(kShelter + glm::vec3(-1.2f, 0.5f, 0.4f), glm::vec3(1.0f), 0.3f)});
        s.addInstance({.mesh = *crate,
                       .material = wood,
                       .transform = placed(kPlace + glm::vec3(-4.0f, 0.5f, -9.0f), glm::vec3(1.0f), 0.5f)});
        const MaterialId brick =
            s.addMaterial({.baseColor = glm::vec4(0.2f, 0.17f, 0.15f, 1.0f), .roughness = 0.85f, .name = "block"});
        s.addInstance({.mesh = *block, .material = brick, .transform = placed(kPlace + glm::vec3(4.0f, 6.0f, -50.0f))});
        if (Result<void> plants = addMeadow(c.plants, s); !plants) return plants;

        m_sun = s.addLight({.type = LightType::Directional});
        preset("fair");
        m_weather.setTime(kStartTime);
        m_weather.setWetnessFocus(cameraGround());
        applySun();
        s.setWeather(m_weather);
        m_sound.emplace(c.app.audio());
        m_sound->setEnabled(false);
        return {};
    }

    void enter(Context& c) override {
        m_outsideRate = c.app.maxFrameRate();
        applyFrameLimit(c);
        if (m_sound) m_sound->setEnabled(m_soundOn);
    }

    void leave(Context& c) override {
        if (c.app.maxFrameRate() != m_outsideRate) c.app.setMaxFrameRate(m_outsideRate);
        if (m_sound) m_sound->setEnabled(false);
    }

    void update(Context& c, double frameDt) override {
        m_stats = c.renderer.stats();
        const double dt = std::min(frameDt, 0.1);
        m_weather.setWetnessFocus(cameraGround());
        m_weather.update(dt * double(m_timeScale));
        passStrikes();
        if (m_runDay) {
            m_elevation += float(dt) * 3.0f;
            if (m_elevation > 70.0f) m_elevation = -4.0f;
        }
        applySun();
        m_scene->setWeather(m_weather);
        updateSound(frameDt);
    }

    void controls(Context& c) override {
        ImGui::SetNextItemWidth(itemWidth());
        int view = int(m_view);
        if (ImGui::Combo("View", &view, kViewNames.data(), int(kViewNames.size()))) setView(View(view));
        ImGui::TextUnformatted("Weather");
        // Apart from the sliders' IDs: the Cirrus preset and the Cirrus slider share a label.
        ImGui::PushID("presets");
        for (std::size_t i = 0; i < kPresets.size(); ++i) {
            if (i % 3 != 0) ImGui::SameLine();
            if (ImGui::Button(kPresetNames[i])) preset(kPresets[i]);
        }
        ImGui::PopID();
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
        changed |= ImGui::SliderFloat("Temperature (C)", &w.temperature, -15.0f, 25.0f, "%.1f");
        ImGui::Separator();
        changed |= ImGui::SliderFloat("Wind (m/s)", &w.wind.speed, 0.0f, 40.0f, "%.0f");
        if (ImGui::SliderFloat("Wind from (deg)", &m_windFrom, 0.0f, 360.0f, "%.0f")) {
            w.wind.direction = windDirection(m_windFrom);
            changed = true;
        }
        changed |= ImGui::SliderFloat("Gusts", &w.wind.gusts, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("Turbulence", &w.wind.turbulence, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("Lightning rate", &w.lightning, 0.0f, 8.0f, "%.1f");
        changed |= ImGui::SliderFloat("Change of shape", &w.evolution, 0.0f, 4.0f);
        ImGui::SliderFloat("Time scale", &m_timeScale, 0.0f, 60.0f, "%.0f x");
        if (changed) m_weather.setSettings(w);
        ImGui::Separator();
        // A sun the person has placed is theirs: the next preset leaves it where it is.
        if (ImGui::SliderFloat("Sun elevation", &m_elevation, -4.0f, 80.0f, "%.0f deg")) m_presetSun = false;
        if (ImGui::SliderFloat("Sun azimuth", &m_azimuth, 0.0f, 360.0f, "%.0f deg")) m_presetSun = false;
        ImGui::Checkbox("Run the day", &m_runDay);
        if (m_runDay) m_presetSun = false;
        if (ImGui::Button("Sunset")) {
            m_elevation = 3.0f;
            m_azimuth = 25.0f;
            m_presetSun = false;
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
        ImGui::Separator();
        int rain = int(settings.rain);
        ImGui::SetNextItemWidth(itemWidth());
        if (ImGui::Combo("Rain", &rain, kRainNames.data(), int(kRainNames.size()))) settings.rain = Rain(rain);
        int snow = int(settings.snow);
        ImGui::SetNextItemWidth(itemWidth());
        if (ImGui::Combo("Snow", &snow, kRainNames.data(), int(kRainNames.size()))) settings.snow = Snow(snow);
        ImGui::Checkbox("Rain shafts", &settings.rainShafts);
        ImGui::SameLine();
        ImGui::Checkbox("Rainbow", &settings.rainbow);
        ImGui::Checkbox("Lightning and thunder", &settings.lightning);
        if (ImGui::Checkbox("Weather sound", &m_soundOn) && m_sound) m_sound->setEnabled(m_soundOn);
        if (ImGui::SliderFloat("Sound volume", &m_soundVolume, 0.0f, 1.0f) && m_sound)
            m_sound->setVolume(m_soundVolume);
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
        ImGui::TextWrapped("Precipitation at the camera %.2f, nearest rain %.1f km away", double(rainHere()),
                           double(m_rainDistance) / 1000.0);
        const wx::Wetness wet = m_weather.wetness(cameraGround());
        ImGui::TextWrapped("Ground wetness %.2f, standing water %.2f, snow cover %.2f", double(wet.ground),
                           double(wet.water), double(wet.snow));
        ImGui::TextWrapped("%.1f C at the camera, %.0f %% of the precipitation as snow",
                           double(m_weather.temperature(camera.eye())),
                           double(m_weather.snowShare(camera.eye())) * 100.0);
        if (m_stats.rain != Rain::Off)
            ImGui::TextWrapped("Rain %s: %u streaks, %u splashes, %u objects in the rain map, cover %.2f%s%s",
                               kRainNames[std::size_t(m_stats.rain)], m_stats.rainStreaks, m_stats.rainSplashes,
                               m_stats.rainOccluders, double(std::max(m_stats.rainCover, 0.0f)),
                               m_stats.rainShafts ? ", shafts" : "", m_stats.rainbow ? ", rainbow" : "");
        if (m_stats.snow != Snow::Off)
            ImGui::TextWrapped("Snow %s: %u flakes, snowfall %.2f at the camera", kRainNames[std::size_t(m_stats.snow)],
                               m_stats.snowFlakes, double(m_stats.snowAtCamera));
        ImGui::TextWrapped("Wind %.1f m/s at 10 m here (mean %.1f)", double(windHere()), double(meanWindHere()));
        if (settings.lightning)
            ImGui::TextWrapped("Lightning %.2f, %u flashes lit, %u heard so far", double(m_stats.lightning),
                               m_stats.lightningFlashes, m_strikesHeard);
        if (m_sound && m_sound->enabled()) {
            ImGui::TextWrapped("Rain sound: light %.2f, heavy %.2f, far %.2f, on the roof %.2f",
                               double(m_sound->gain(audio::WeatherLayer::RainLight)),
                               double(m_sound->gain(audio::WeatherLayer::RainHeavy)),
                               double(m_sound->gain(audio::WeatherLayer::RainFar)),
                               double(m_sound->gain(audio::WeatherLayer::RainCover)));
            ImGui::TextWrapped("Wind sound: light %.2f, strong %.2f; thunder %u played, %zu on its way",
                               double(m_sound->gain(audio::WeatherLayer::WindLight)),
                               double(m_sound->gain(audio::WeatherLayer::WindStrong)), m_sound->thunderPlayed(),
                               m_sound->thunderWaiting());
        }
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
        if (name == "view") return double(int(m_view));
        if (name == "rain") return double(int(m_stats.rain));
        if (name == "rain_at_camera") return double(m_stats.rainAtCamera);
        if (name == "rain_cover") return double(m_stats.rainCover);
        if (name == "rain_streaks") return double(m_stats.rainStreaks);
        if (name == "rain_splashes") return double(m_stats.rainSplashes);
        if (name == "rain_occluders") return double(m_stats.rainOccluders);
        if (name == "rain_shafts") return m_stats.rainShafts ? 1.0 : 0.0;
        if (name == "rainbow") return m_stats.rainbow ? 1.0 : 0.0;
        if (name == "wetness") return double(m_weather.wetness(cameraGround()).ground);
        if (name == "standing_water") return double(m_weather.wetness(cameraGround()).water);
        if (name == "snow") return double(int(m_stats.snow));
        if (name == "snow_at_camera") return double(m_stats.snowAtCamera);
        if (name == "snow_share") return double(m_stats.snowShareAtCamera);
        if (name == "snow_flakes") return double(m_stats.snowFlakes);
        if (name == "snow_cover") return double(m_weather.wetness(cameraGround()).snow);
        if (name == "temperature") return double(m_weather.settings().temperature);
        if (name == "gusts") return double(m_weather.settings().wind.gusts);
        if (name == "turbulence") return double(m_weather.settings().wind.turbulence);
        if (name == "wind_here") return double(windHere());
        if (name == "mean_wind_here") return double(meanWindHere());
        if (name == "lightning") return double(m_stats.lightning);
        if (name == "lightning_flashes") return double(m_stats.lightningFlashes);
        if (name == "strikes_heard") return double(m_strikesHeard);
        if (name == "plants") return double(m_plants);
        if (name == "thunder_played" && m_sound) return double(m_sound->thunderPlayed());
        if (name == "thunder_waiting" && m_sound) return double(m_sound->thunderWaiting());
        if (name.starts_with("sound_") && m_sound) {
            for (std::size_t i = 0; i < audio::kWeatherLayers; ++i)
                if (name.substr(6) == kSoundValues[i]) return double(m_sound->gain(audio::WeatherLayer(i)));
            if (name == "sound_playing") {
                int playing = 0;
                for (std::size_t i = 0; i < audio::kWeatherLayers; ++i)
                    playing += m_sound->sound(audio::WeatherLayer(i)).valid() ? 1 : 0;
                return double(playing);
            }
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
            m_presetSun = false;
            applySun();
            return true;
        } else if (name == "sun_azimuth" && number) {
            m_azimuth = *number;
            m_presetSun = false;
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
        } else if (name == "view") {
            const auto it = std::find(kViewIds.begin(), kViewIds.end(), value);
            if (it == kViewIds.end()) return false;
            setView(View(it - kViewIds.begin()));
            return true;
        } else if (name == "rain") {
            const auto it = std::find(kRainIds.begin(), kRainIds.end(), value);
            if (it == kRainIds.end()) return false;
            settings.rain = Rain(it - kRainIds.begin());
            return true;
        } else if (name == "snow") {
            const auto it = std::find(kRainIds.begin(), kRainIds.end(), value);
            if (it == kRainIds.end()) return false;
            settings.snow = Snow(it - kRainIds.begin());
            return true;
        } else if (name == "temperature" && number) {
            w.temperature = *number;
        } else if (name == "rain_shafts" && flag) {
            settings.rainShafts = *flag;
            return true;
        } else if (name == "rainbow" && flag) {
            settings.rainbow = *flag;
            return true;
        } else if (name == "sound" && flag) {
            m_soundOn = *flag;
            if (m_sound) m_sound->setEnabled(m_soundOn);
            return true;
        } else if (name == "gusts" && number) {
            w.wind.gusts = std::clamp(*number, 0.0f, 1.0f);
        } else if (name == "turbulence" && number) {
            w.wind.turbulence = std::clamp(*number, 0.0f, 1.0f);
        } else if (name == "lightning_rate" && number) {
            w.lightning = std::max(*number, 0.0f);
        } else if (name == "lightning" && flag) {
            settings.lightning = *flag;
            return true;
        } else if (name == "flash" && flag) {
            return *flag && toNextFlash();
        } else {
            return false;
        }
        m_weather.setSettings(w);
        return true;
    }

    std::string perfDetails() const override {
        return std::format(
            "clouds={} steps={} cube_faces={} limit_60hz={} view={} rain={} shafts={} rainbow={} snow={} lightning={}",
            kQualityIds[std::size_t(m_quality)], m_steps, m_cubeFaces, m_limit60 ? 1 : 0, kViewIds[std::size_t(m_view)],
            kRainIds[std::size_t(settings.rain)], settings.rainShafts ? 1 : 0, settings.rainbow ? 1 : 0,
            kRainIds[std::size_t(settings.snow)], settings.lightning ? 1 : 0);
    }

    void shutdown(Context& c) override {
        (void)c;
        m_sound.reset();
        m_scene.reset();
    }

private:
    bool preset(std::string_view name) {
        wx::Settings w = m_weather.settings();
        w.typeVariation = 0.15f;
        w.precipitation = 0.0f;
        w.cirrus = 0.15f;
        w.temperature = kMildTemperature;
        w.wind.speed = kWindSpeed;
        w.wind.gusts = 0.0f;
        w.wind.turbulence = 0.0f;
        w.lightning = 1.0f;
        // Two presets place the sun for their picture (a low sun ahead for the sun shower, the sun behind for the
        // passing one). The next preset without a sun of its own puts it back where it was: left there, a snowfall
        // chosen after the sun shower kept its sun at 12 degrees, and its sky was dim and brown.
        const bool ownSun = name == "rain_here" || name == "passing";
        if (ownSun && !m_presetSun) {
            m_elevationBefore = m_elevation;
            m_azimuthBefore = m_azimuth;
        } else if (!ownSun && m_presetSun) {
            m_elevation = m_elevationBefore;
            m_azimuth = m_azimuthBefore;
        }
        m_presetSun = ownSun;
        std::optional<wx::Cell> shower;
        const wx::Cell showerCell{.position = kShowerAt,
                                  .radius = 3200.0f,
                                  .type = wx::CloudType::Cumulonimbus,
                                  .coverage = 1.0f,
                                  .precipitation = 1.0f};
        if (name == "fair") {
            w.coverage = 0.32f;
            w.type = wx::CloudType::Cumulus;
            shower = showerCell;
        } else if (name == "shower") {
            w.coverage = 0.05f;
            w.type = wx::CloudType::Cumulus;
            shower = showerCell;
        } else if (name == "rain_here") {
            w.coverage = 0.03f;
            w.type = wx::CloudType::Cumulus;
            w.cirrus = 0.1f;
            shower = wx::Cell{.position = glm::vec2(kPlace.x, kPlace.z) + kRainHereAt,
                              .radius = 2000.0f,
                              .type = wx::CloudType::Cumulonimbus,
                              .coverage = 1.0f,
                              .precipitation = 1.0f,
                              .drifts = false};
            m_elevation = 12.0f;
            m_azimuth = 335.0f;
        } else if (name == "downpour") {
            w.coverage = 0.9f;
            w.type = wx::CloudType::Cumulonimbus;
            w.typeVariation = 0.2f;
            w.precipitation = 1.0f;
            w.cirrus = 0.0f;
        } else if (name == "passing") {
            w.coverage = 0.03f;
            w.type = wx::CloudType::Cumulus;
            w.cirrus = 0.1f;
            shower = wx::Cell{.position = glm::vec2(kPlace.x, kPlace.z) + kPassingAt,
                              .radius = 2400.0f,
                              .type = wx::CloudType::Cumulonimbus,
                              .coverage = 1.0f,
                              .precipitation = 1.0f};
            m_elevation = 30.0f;
            m_azimuth = 150.0f;
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
        } else if (name == "snowfall") {
            // A grey deck snowing steadily in a light wind: the cover builds over the place as long as it snows.
            w.coverage = 1.0f;
            w.type = wx::CloudType::Stratus;
            w.typeVariation = 0.1f;
            w.precipitation = 1.0f;
            w.cirrus = 0.0f;
            w.temperature = kSnowTemperature;
            w.wind.speed = kSnowWindSpeed;
        } else if (name == "thaw") {
            // Sun and a mild wind after the snow: the cover melts into wet ground and puddles.
            w.coverage = 0.25f;
            w.type = wx::CloudType::Cumulus;
            w.cirrus = 0.2f;
            w.temperature = 6.0f;
            w.wind.speed = kSnowWindSpeed;
        } else if (name == "snow_showers") {
            // Cumulus and towering cumulus snowing in a cold blue sky: white shafts under them from the air.
            w.coverage = 0.45f;
            w.type = wx::CloudType::Towering;
            w.typeVariation = 0.35f;
            w.precipitation = 0.6f;
            w.cirrus = 0.1f;
            w.temperature = -2.0f;
        } else if (name == "windy_storm") {
            // A gale under a thunderstorm standing over the meadow: gusts sweeping across the grass, the rain driven
            // through them, lightning and thunder; the weather's clock at its own pace, so the gusts are as fast as
            // they would be.
            w.coverage = 0.8f;
            w.type = wx::CloudType::Cumulus;
            w.typeVariation = 0.5f;
            w.precipitation = 0.6f;
            w.cirrus = 0.0f;
            w.wind.speed = kGaleSpeed;
            w.wind.gusts = 0.6f;
            w.wind.turbulence = 0.4f;
            w.lightning = kStormLightning;
            m_windFrom = kGaleFrom;
            w.wind.direction = windDirection(m_windFrom);
            shower = wx::Cell{.position = glm::vec2(kMeadow.x, kMeadow.z),
                              .radius = 3000.0f,
                              .type = wx::CloudType::Cumulonimbus,
                              .coverage = 1.0f,
                              .precipitation = 1.0f,
                              .drifts = false};
            m_timeScale = 1.0f;
            settings.lightning = true;
        } else {
            return false;
        }
        m_weather.setSettings(w);
        if (shower) m_showerCell = *shower;
        setShower(shower.has_value());
        applySun();
        return true;
    }

    // The single raining cloud of the last preset that had one (drifting with the wind from where it is placed,
    // unless it stays).
    void setShower(bool on) {
        if (m_shower != wx::kNoCell) m_weather.removeCell(m_shower);
        m_shower = wx::kNoCell;
        if (on) m_shower = m_weather.addCell(m_showerCell);
    }

    void setView(View view) {
        m_view = view;
        switch (view) {
        case View::Sky:
            // A few hundred metres up, looking a little above the horizon: the clouds, their shadows on the plain,
            // the lake.
            camera.target = glm::vec3(0.0f, 320.0f, -600.0f);
            camera.distance = 600.0f;
            camera.yaw = 0.0f;
            camera.pitch = glm::radians(-10.0f);
            camera.minDistance = 30.0f;
            camera.maxDistance = 3000.0f;
            settings.shadowDistance = 2500.0f;
            break;
        case View::Ground:
            // At eye height on the paving: the shelter on the right, the pond on the left, the block behind.
            camera.target = kPlace + glm::vec3(-1.0f, 1.6f, -14.0f);
            camera.distance = 10.0f;
            camera.yaw = glm::radians(-6.0f);
            camera.pitch = glm::radians(4.0f);
            camera.minDistance = 1.5f;
            camera.maxDistance = 400.0f;
            settings.shadowDistance = 120.0f;
            break;
        case View::Cloud:
            // Turned around toward the raining cloud, the sun behind: its rain shafts and the rainbow in them.
            camera.target = kPlace + glm::vec3(2.0f, 3.0f, -8.0f);
            camera.distance = 10.0f;
            camera.yaw = glm::radians(186.0f);
            camera.pitch = glm::radians(-8.0f);
            camera.minDistance = 1.5f;
            camera.maxDistance = 400.0f;
            settings.shadowDistance = 120.0f;
            break;
        case View::Shelter:
            // Under the roof, looking out over the paving toward the pond.
            camera.target = kShelter + glm::vec3(-3.0f, 0.4f, -0.8f);
            camera.distance = 3.0f;
            camera.yaw = glm::radians(75.0f);
            camera.pitch = glm::radians(20.0f);
            camera.minDistance = 1.0f;
            camera.maxDistance = 400.0f;
            settings.shadowDistance = 120.0f;
            break;
        case View::Above:
            // Above the place, looking down over the roof, the crates and the paving: what lies on top of them.
            camera.target = kShelter + glm::vec3(-3.5f, 0.5f, 0.5f);
            camera.distance = 11.0f;
            camera.yaw = glm::radians(-25.0f);
            camera.pitch = glm::radians(38.0f);
            camera.minDistance = 1.5f;
            camera.maxDistance = 400.0f;
            settings.shadowDistance = 120.0f;
            break;
        case View::Meadow:
            // At eye height at the meadow's edge, the wind from the left: the grass before the camera, bushes and
            // trees behind it.
            camera.target = kMeadow + glm::vec3(0.0f, 1.0f, 2.0f);
            camera.distance = 9.0f;
            camera.yaw = 0.0f;
            camera.pitch = glm::radians(8.0f);
            camera.minDistance = 1.5f;
            camera.maxDistance = 400.0f;
            settings.shadowDistance = 120.0f;
            break;
        }
        cameraCut = true;
    }

    // Precipitation at the camera (none above the cloud base).
    float rainHere() const { return m_weather.precipitation(camera.eye()); }

    // The rain sound from the weather at the camera: the distance to the nearest rain (not snow) is searched four times
    // a second (a search costs up to a few tenths of a millisecond), the cover is the renderer's; snow is quiet.
    void updateSound(double frameDt) {
        m_rainSearch -= frameDt;
        if (m_rainSearch <= 0.0) {
            m_rainDistance = m_weather.distanceToPrecipitation(cameraGround(), 0.1f, 8000.0f, wx::Precipitation::Rain);
            m_rainSearch = 0.25;
        }
        if (!m_sound) return;
        const bool drawn = m_stats.rain != Rain::Off || m_stats.snow != Snow::Off;
        m_sound->update({.precipitation = rainHere(),
                         .distanceToRain = m_rainDistance,
                         .cover = drawn ? std::max(m_stats.rainCover, 0.0f) : 0.0f,
                         .snow = m_weather.snowShare(camera.eye()),
                         .wind = windHere()},
                        frameDt);
    }

    // The wind at 10 m above the camera's ground (what weather reports give, and the sound's layers take), with its
    // gusts and without.
    float windHere() const {
        const glm::vec2 g = cameraGround();
        return glm::length(m_weather.windAt({g.x, 10.0f, g.y}));
    }
    float meanWindHere() const {
        const glm::vec2 g = cameraGround();
        return glm::length(m_weather.meanWindAt({g.x, 10.0f, g.y}));
    }

    // The flashes since the last frame, within earshot: to the sound, which plays their thunder when it arrives. A
    // jump of the weather's clock (a preset, a scenario) starts over from the new time.
    void passStrikes() {
        const double now = m_weather.time();
        if (now > m_strikesUntil && now - m_strikesUntil < 5.0 && settings.lightning) {
            m_weather.strikes(cameraGround(), kThunderRadius, m_strikesUntil, now, m_strikeList);
            for (const wx::Strike& strike : m_strikeList) {
                ++m_strikesHeard;
                if (m_sound) m_sound->strike(glm::length(strike.position - cameraGround()), strike.brightness);
            }
        }
        m_strikesUntil = now;
    }

    // Moves the weather's clock to 5 ms after the next flash within 3 km of the camera (in the next 10 minutes); its
    // thunder follows. The flashes are looked for in the weather setTime() makes, where they then are.
    bool toNextFlash() {
        const double now = m_weather.time();
        m_weather.setTime(now);
        if (m_shower != wx::kNoCell) setShower(true);
        for (double from = now; from < now + 600.0; from += 60.0) {
            m_weather.strikes(cameraGround(), 3000.0f, from, from + 60.0, m_strikeList);
            if (m_strikeList.empty()) continue;
            const double flash = m_strikeList.front().time;
            m_weather.setTime(flash + 0.005);
            if (m_shower != wx::kNoCell) setShower(true);
            m_strikesUntil = flash - 1e-3;
            return true;
        }
        return false;
    }

    // The meadow: patches of grass, each blade turning about the ground below it (custom.w 1), and bushes and trees,
    // each its own instance turning about its origin.
    Result<void> addMeadow(SurfaceId surface, Scene& s) {
        Random random{0x51ade5u};
        std::vector<Result<MeshId>> patches;
        for (int i = 0; i < kGrassPatches; ++i) patches.push_back(addMesh(s, grassPatch(random)));
        MeshData bushShape = lumps(random, 5, 0.45f, 0.55f, glm::vec3(0.0f, 0.55f, 0.0f), glm::vec3(0.5f, 0.25f, 0.5f));
        const Result<MeshId> bush = addMesh(s, bushShape);
        MeshData crownShape = lumps(random, 7, 0.9f, 1.3f, glm::vec3(0.0f, 4.2f, 0.0f), glm::vec3(1.2f, 0.7f, 1.2f));
        const Result<MeshId> crown = addMesh(s, crownShape);
        MeshData trunkShape = cylinderMesh(0.13f, 1.8f, 12);
        for (Vertex& v : trunkShape.vertices) v.position.y += 1.8f;
        const Result<MeshId> trunk = addMesh(s, trunkShape);
        for (const Result<MeshId>& patch : patches)
            if (!patch) return Error{patch.error()};
        for (const Result<MeshId>* m : {&bush, &crown, &trunk})
            if (!*m) return Error{m->error()};
        const MaterialId grass = s.addMaterial({.surface = surface,
                                                .baseColor = glm::vec4(0.3f, 0.42f, 0.16f, 1.0f),
                                                .roughness = 0.8f,
                                                .doubleSided = true,
                                                .name = "grass blades",
                                                .wetness = 0.6f,
                                                .snow = 0.5f});
        const MaterialId leaves = s.addMaterial({.surface = surface,
                                                 .baseColor = glm::vec4(0.16f, 0.3f, 0.1f, 1.0f),
                                                 .roughness = 0.75f,
                                                 .name = "leaves",
                                                 .wetness = 0.6f,
                                                 .snow = 0.5f});
        const MaterialId bark = s.addMaterial(
            {.surface = surface, .baseColor = glm::vec4(0.25f, 0.18f, 0.12f, 1.0f), .roughness = 0.9f, .name = "bark"});
        for (float z = -kMeadowHalf + kPatchSize / 2.0f; z < kMeadowHalf; z += kPatchSize)
            for (float x = -kMeadowHalf + kPatchSize / 2.0f; x < kMeadowHalf; x += kPatchSize) {
                const std::size_t which =
                    std::min(std::size_t(random.uniform(0.0f, float(kGrassPatches))), std::size_t(kGrassPatches - 1));
                const float turn = 1.5707963f * std::floor(random.uniform(0.0f, 4.0f));
                s.addInstance({.mesh = *patches[which],
                               .material = grass,
                               .transform = placed(kMeadow + glm::vec3(x, 0.0f, z), glm::vec3(1.0f), turn),
                               .color = glm::vec4(glm::vec3(random.uniform(0.92f, 1.08f)), 1.0f),
                               .custom = glm::vec4(0.6f, 0.5f, 0.01f, 1.0f)});
                ++m_plants;
            }
        for (int i = 0; i < 12; ++i) {
            const float x = random.uniform(-kMeadowHalf, kMeadowHalf);
            const glm::vec3 at = kMeadow + glm::vec3(x, 0.0f, random.uniform(-kMeadowHalf, -2.0f));
            const float size = random.uniform(0.8f, 1.4f);
            s.addInstance({.mesh = *bush,
                           .material = leaves,
                           .transform = placed(at, glm::vec3(size), random.uniform(0.0f, 6.2831853f)),
                           .custom = glm::vec4(1.1f * size, 2.0f, 0.03f, 0.0f)});
            ++m_plants;
        }
        for (const glm::vec2 xz : {glm::vec2(-7.0f, -11.0f), glm::vec2(3.0f, -14.0f), glm::vec2(9.0f, -9.0f)}) {
            const glm::mat4 root = placed(kMeadow + glm::vec3(xz.x, 0.0f, xz.y));
            const glm::vec4 tree(5.5f, 4.0f, 0.04f, 0.0f);
            s.addInstance({.mesh = *trunk, .material = bark, .transform = root, .custom = tree});
            s.addInstance({.mesh = *crown, .material = leaves, .transform = root, .custom = tree});
            ++m_plants;
        }
        return {};
    }

    // A tuft of grass about 0.6 m tall around its origin: blades leaning out a little, each a tapering strip of three
    // segments, darker at the root (the vertex color).
    static MeshData grassTuft(Random& random) {
        MeshData m;
        constexpr int kBlades = 11;
        constexpr int kSegments = 3;
        for (int b = 0; b < kBlades; ++b) {
            const float around = 6.2831853f * (float(b) + random.uniform(0.0f, 0.7f)) / float(kBlades);
            const glm::vec2 out(std::cos(around), std::sin(around));
            const glm::vec3 base = glm::vec3(out.x, 0.0f, out.y) * random.uniform(0.0f, 0.07f);
            const float lean = random.uniform(0.1f, 0.4f);
            const float height = random.uniform(0.55f, 1.0f) * 0.6f;
            const float facing = around + random.uniform(-0.6f, 0.6f) + 1.5707963f;
            const glm::vec3 across(std::cos(facing), 0.0f, std::sin(facing));
            const glm::vec3 axis =
                glm::normalize(glm::vec3(out.x * std::sin(lean), std::cos(lean), out.y * std::sin(lean)));
            const glm::vec3 normal =
                glm::normalize(glm::normalize(glm::cross(across, axis)) + glm::vec3(0.0f, 0.8f, 0.0f));
            const std::uint32_t first = std::uint32_t(m.vertices.size());
            for (int k = 0; k <= kSegments; ++k) {
                const float t = float(k) / float(kSegments);
                // A little more lean toward the tip: the blade arches.
                const glm::vec3 p =
                    base + axis * (height * t) + glm::vec3(out.x, 0.0f, out.y) * (0.12f * height * t * t);
                const float half = 0.022f * (1.0f - 0.85f * t);
                const glm::vec4 shade(glm::vec3(0.45f + 0.55f * t), 1.0f);
                m.vertices.push_back(
                    {.position = p - across * half, .normal = normal, .uv = {0.0f, 1.0f - t}, .color = shade});
                m.vertices.push_back(
                    {.position = p + across * half, .normal = normal, .uv = {1.0f, 1.0f - t}, .color = shade});
            }
            for (int k = 0; k < kSegments; ++k) {
                const std::uint32_t i = first + std::uint32_t(k) * 2u;
                m.indices.insert(m.indices.end(), {i, i + 1u, i + 3u, i, i + 3u, i + 2u});
            }
        }
        return m;
    }

    // A square of kPatchSize metres around its origin, tufts on a jittered grid, each turned, sized and shaded its way.
    static MeshData grassPatch(Random& random) {
        MeshData m;
        constexpr float kStep = 0.7f;
        for (float z = -kPatchSize / 2.0f + kStep / 2.0f; z < kPatchSize / 2.0f; z += kStep)
            for (float x = -kPatchSize / 2.0f + kStep / 2.0f; x < kPatchSize / 2.0f; x += kStep) {
                const float dx = random.uniform(-0.3f, 0.3f);
                const glm::vec3 at(x + dx, 0.0f, z + random.uniform(-0.3f, 0.3f));
                const float size = random.uniform(0.7f, 1.3f);
                const float yaw = random.uniform(0.0f, 6.2831853f);
                const float shade = random.uniform(0.8f, 1.15f);
                const glm::mat3 turn = glm::mat3(glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0.0f, 1.0f, 0.0f)));
                const MeshData tuft = grassTuft(random);
                const std::uint32_t first = std::uint32_t(m.vertices.size());
                for (Vertex v : tuft.vertices) {
                    v.position = at + turn * (v.position * size);
                    v.normal = turn * v.normal;
                    v.color = glm::vec4(glm::vec3(v.color) * shade, 1.0f);
                    m.vertices.push_back(v);
                }
                for (const std::uint32_t index : tuft.indices) m.indices.push_back(first + index);
            }
        return m;
    }

    // Overlapping spheres around `centre` (spread: how far from it each may lie): a bush or a tree's crown.
    static MeshData lumps(Random& random, int count, float smallest, float largest, glm::vec3 centre,
                          glm::vec3 spread) {
        MeshData m;
        for (int i = 0; i < count; ++i) {
            const MeshData ball = sphereMesh(random.uniform(smallest, largest), 10, 16);
            glm::vec3 offset;
            for (int axis = 0; axis < 3; ++axis) offset[axis] = random.uniform(-1.0f, 1.0f);
            const glm::vec3 at = centre + offset * spread;
            const std::uint32_t first = std::uint32_t(m.vertices.size());
            for (Vertex v : ball.vertices) {
                v.position += at;
                m.vertices.push_back(v);
            }
            for (const std::uint32_t index : ball.indices) m.indices.push_back(first + index);
        }
        return m;
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
        wx::Settings{.seed = 7, .wind = {.direction = {0.0f, 1.0f}, .speed = kWindSpeed}, .evolution = 1.0f}};
    wx::CellId m_shower = wx::kNoCell;
    wx::Cell m_showerCell{};
    View m_view = View::Sky;
    std::optional<audio::WeatherAudio> m_sound;
    bool m_soundOn = true;
    float m_soundVolume = 1.0f;
    double m_rainSearch = 0.0;
    float m_rainDistance = 1e9f;
    RenderStats m_stats{};
    Quality m_quality = Quality::Reduced;
    int m_steps = 64;
    int m_cubeFaces = 2;
    float m_windFrom = 0.0f;
    float m_timeScale = 10.0f;
    float m_elevation = 30.0f;
    float m_azimuth = 50.0f;
    // The sun is where a preset placed it, and where it was before that.
    bool m_presetSun = false;
    float m_elevationBefore = 30.0f;
    float m_azimuthBefore = 50.0f;
    bool m_runDay = false;
    bool m_limit60 = false;
    double m_outsideRate = 0.0;
    std::vector<wx::Strike> m_strikeList;
    double m_strikesUntil = 0.0; // the weather time the flashes were passed on up to
    std::uint32_t m_strikesHeard = 0;
    std::uint32_t m_plants = 0;
};

} // namespace

std::unique_ptr<Section> makeWeatherSection() { return std::make_unique<WeatherSection>(); }

} // namespace demo
