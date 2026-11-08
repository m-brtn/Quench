#pragma once

// The engine's weather sounds (docs/audio.md, "Weather sounds"): looping layers of rain and wind mixed from what the
// weather does around the listener, and thunder after lightning. Light and heavy rain crossfade by the precipitation
// there, distant rain grows as the nearest rain comes closer, rain on a roof replaces the open-air layers as the
// listener goes under cover, and a light and a strong wind crossfade by the wind's speed (its gusts swell them). A
// flash of lightning the game passes on is heard as thunder when its sound arrives, later and duller the farther it
// struck. A game sets the numbers once per frame (from quench_weather and the renderer's rain cover, or its own), names
// its own sound for any layer, or switches the object off and plays its own sounds from the same numbers
// (targetGains and thunderGains give the engine's mix rules for that).
//
// Game thread only: it plays, fades and restarts sounds through the AudioSystem, whose audio thread keeps its rules
// (nothing here runs there). The sounds are assets: a reloaded file (hot reload) crossfades into the new data.

#include <quench/audio/AudioSystem.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace quench::audio {

enum class WeatherLayer : std::uint8_t {
    RainLight,  // rain at the listener, light: a soft hiss of fine drops
    RainHeavy,  // heavy rain at the listener: a dense roar
    RainFar,    // rain heard from afar, low and dull, while the nearest rain is within WeatherSoundDesc::farDistance
    RainCover,  // rain drumming on the roof over the listener (under cover)
    WindLight,  // a light wind: a soft rush that rises with its speed
    WindStrong, // a strong wind: a roar with a howl, from about 6 m/s
};
inline constexpr std::size_t kWeatherLayers = 6;

// The thunder of a flash (WeatherAudio::strike): a near clap with its crack and a far, low rumble.
enum class Thunder : std::uint8_t { Near, Far };
inline constexpr std::size_t kThunderSounds = 2;

const char* weatherLayerName(WeatherLayer layer);
// The engine's default sound of a layer: "quench/weather/rain_light.wav" and so on, cooked by the CMake target
// quench_weather_sounds (procedural, tools/assets/make_weather_sounds.py), which a game lists in quench_add_game's
// COOKED_ASSETS.
const char* defaultWeatherSound(WeatherLayer layer);
// "quench/weather/thunder_near.wav" and "quench/weather/thunder_far.wav", cooked the same way (one-shots).
const char* defaultThunderSound(Thunder thunder);

// What the listener hears of the weather.
struct WeatherSoundState {
    float precipitation = 0.0f; // at the listener in the open, 0..1, rain and snow (weather::Weather::precipitation)
    // m to the nearest rain, 0 inside it (weather::Weather::distanceToPrecipitation with Precipitation::Rain)
    float distanceToRain = 1e9f;
    float cover = 0.0f; // 0 open sky .. 1 under a roof (render::RenderStats::rainCover, or the game's own)
    // The share of the precipitation that falls as snow (weather::Weather::snowShare): snow falls without a sound,
    // so only the rest plays as rain, and falling snow muffles the distant rain.
    float snow = 0.0f;
    // The wind's speed at the listener (m/s), its gusts included: |weather::Weather::windAt|.
    float wind = 0.0f;
};

struct WeatherSoundDesc {
    // A sound per layer (WeatherLayer order); an empty path leaves the layer silent.
    std::array<std::string, kWeatherLayers> sounds{
        defaultWeatherSound(WeatherLayer::RainLight), defaultWeatherSound(WeatherLayer::RainHeavy),
        defaultWeatherSound(WeatherLayer::RainFar),   defaultWeatherSound(WeatherLayer::RainCover),
        defaultWeatherSound(WeatherLayer::WindLight), defaultWeatherSound(WeatherLayer::WindStrong)};
    // The thunder's sounds (Thunder order); an empty path leaves that one silent.
    std::array<std::string, kThunderSounds> thunder{defaultThunderSound(Thunder::Near),
                                                    defaultThunderSound(Thunder::Far)};
    std::string bus = "effects"; // AudioSystem::bus: the master bus (and a warning) when none has this name
    float volume = 1.0f;
    float farDistance = 4000.0f;   // m: distant rain is heard from this close, louder as it comes nearer
    float smoothing = 0.5f;        // s: how fast the layers follow a change of the state
    float thunderReach = 25000.0f; // m: the thunder of farther flashes is not heard
};

class WeatherAudio {
public:
    // Loads the sounds (through the AudioSystem's asset manager); nothing plays before the first update().
    explicit WeatherAudio(AudioSystem& audio, WeatherSoundDesc desc = {});
    // Fades its sounds out. It holds sound assets: destroy it before the AudioSystem.
    ~WeatherAudio();
    WeatherAudio(const WeatherAudio&) = delete;
    WeatherAudio& operator=(const WeatherAudio&) = delete;

    // A game's own sound for a layer (a playing layer crossfades into it); "" silences the layer.
    void setSound(WeatherLayer layer, std::string_view path);
    const std::string& soundPath(WeatherLayer layer) const;
    void setBus(std::string_view name);
    void setVolume(float volume);
    float volume() const { return m_desc.volume; }
    // Off: its sounds fade out and update() plays nothing until it is on again.
    void setEnabled(bool enabled);
    bool enabled() const { return m_enabled; }

    // Once per frame: the layers move toward the state's gains (WeatherSoundDesc::smoothing); a layer starts when it
    // becomes audible and stops after two seconds of silence.
    void update(const WeatherSoundState& state, double dt);

    // A layer's gain now (smoothed, before the volume) and its sound (none while it is silent).
    float gain(WeatherLayer layer) const { return m_layers[std::size_t(layer)].gain; }
    SoundHandle sound(WeatherLayer layer) const { return m_layers[std::size_t(layer)].handle; }

    // A flash of lightning `distance` m from the listener (weather::Weather::strikes; a game passes each flash once,
    // when it appears): its thunder plays distance / kSpeedOfSound seconds later by update()'s clock, by
    // thunderGains. At most kMaxThunder wait; beyond that the farthest is dropped. Nothing while disabled.
    static constexpr float kSpeedOfSound = 343.0f;
    static constexpr std::size_t kMaxThunder = 16;
    void strike(float distance, float brightness = 1.0f);
    void setThunderSound(Thunder thunder, std::string_view path);
    const std::string& thunderPath(Thunder thunder) const;
    std::size_t thunderWaiting() const { return m_thunder.size(); }
    std::uint32_t thunderPlayed() const { return m_thunderPlayed; }

    // The engine's mix rules: the gain of each layer for a state, without smoothing. The rain at the listener is the
    // precipitation times 1 - snow. The open-air layers (light, heavy) scale by 1 - cover and the roof layer by cover,
    // all by a loudness that rises with the square root of the rain; light and heavy crossfade at equal power between
    // rain 0.25 and 0.75; the distant layer rises to 1 as the nearest rain comes from farDistance to 0 m, fades out as
    // rain starts at the listener and is muffled by up to 60 % in falling snow. The wind's layers rise with the
    // square root of its speed from 1 to 20 m/s, crossfade at equal power from light to strong between 6 and 16 m/s
    // and are halved under cover.
    static std::array<float, kWeatherLayers> targetGains(const WeatherSoundState& state, float farDistance);
    // The thunder's rule: a flash's gain falls as 1 / (1 + distance / 1.5 km) up to `reach` (0 beyond), times its
    // brightness and 1 - 0.3 cover; the near clap gives way to the far rumble at equal power from 1 to 5 km.
    static std::array<float, kThunderSounds> thunderGains(float distance, float brightness, float cover, float reach);

private:
    struct Layer {
        std::string path;
        assets::AssetRef<SoundAsset> asset;
        SoundHandle handle{};
        std::uint32_t version = 0; // of the asset when the sound started
        float gain = 0.0f;
        double silent = 0.0; // seconds the layer has been inaudible while its sound plays
    };

    struct Waiting {
        double at = 0.0; // update()'s clock
        float distance = 0.0f;
        float brightness = 1.0f;
    };

    void start(Layer& layer, float volume, float fadeIn);

    AudioSystem& m_audio;
    WeatherSoundDesc m_desc;
    BusId m_bus{};
    std::array<Layer, kWeatherLayers> m_layers{};
    std::array<std::string, kThunderSounds> m_thunderPaths{};
    std::array<assets::AssetRef<SoundAsset>, kThunderSounds> m_thunderSounds{};
    std::vector<Waiting> m_thunder;
    std::uint32_t m_thunderPlayed = 0;
    double m_clock = 0.0;
    float m_cover = 0.0f; // the last state's
    bool m_enabled = true;
};

} // namespace quench::audio
