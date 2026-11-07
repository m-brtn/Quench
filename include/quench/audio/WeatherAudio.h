#pragma once

// The engine's weather sounds (docs/audio.md, "Weather sounds"): looping layers of rain mixed from what the weather
// does around the listener. Light and heavy rain crossfade by the precipitation there, distant rain grows as the
// nearest rain comes closer, and rain on a roof replaces the open-air layers as the listener goes under cover. A game
// sets the numbers once per frame (from quench_weather and the renderer's rain cover, or its own), names its own sound
// for any layer, or switches the object off and plays its own sounds from the same numbers (targetGains gives the
// engine's mix rules for that).
//
// Game thread only: it plays, fades and restarts sounds through the AudioSystem, whose audio thread keeps its rules
// (nothing here runs there). The sounds are assets: a reloaded file (hot reload) crossfades into the new data.

#include <quench/audio/AudioSystem.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace quench::audio {

enum class WeatherLayer : std::uint8_t {
    RainLight, // rain at the listener, light: a soft hiss of fine drops
    RainHeavy, // heavy rain at the listener: a dense roar
    RainFar,   // rain heard from afar, low and dull, while the nearest rain is within WeatherSoundDesc::farDistance
    RainCover, // rain drumming on the roof over the listener (under cover)
};
inline constexpr std::size_t kWeatherLayers = 4;

const char* weatherLayerName(WeatherLayer layer);
// The engine's default sound of a layer: "quench/weather/rain_light.wav" and so on, cooked by the CMake target
// quench_weather_sounds (procedural, tools/assets/make_weather_sounds.py), which a game lists in quench_add_game's
// COOKED_ASSETS.
const char* defaultWeatherSound(WeatherLayer layer);

// What the listener hears of the weather.
struct WeatherSoundState {
    float precipitation = 0.0f;  // at the listener in the open, 0..1 (weather::Weather::precipitation)
    float distanceToRain = 1e9f; // m to the nearest rain, 0 inside it (weather::Weather::distanceToPrecipitation)
    float cover = 0.0f;          // 0 open sky .. 1 under a roof (render::RenderStats::rainCover, or the game's own)
};

struct WeatherSoundDesc {
    // A sound per layer (WeatherLayer order); an empty path leaves the layer silent.
    std::array<std::string, kWeatherLayers> sounds{
        defaultWeatherSound(WeatherLayer::RainLight), defaultWeatherSound(WeatherLayer::RainHeavy),
        defaultWeatherSound(WeatherLayer::RainFar), defaultWeatherSound(WeatherLayer::RainCover)};
    std::string bus = "effects"; // AudioSystem::bus: the master bus (and a warning) when none has this name
    float volume = 1.0f;
    float farDistance = 4000.0f; // m: distant rain is heard from this close, louder as it comes nearer
    float smoothing = 0.5f;      // s: how fast the layers follow a change of the state
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

    // The engine's mix rules: the gain of each layer for a state, without smoothing. The open-air layers (light,
    // heavy) scale by 1 - cover and the roof layer by cover, all by a loudness that rises with the square root of the
    // precipitation; light and heavy crossfade at equal power between precipitation 0.25 and 0.75; the distant layer
    // rises to 1 as the nearest rain comes from farDistance to 0 m and fades out as rain starts at the listener.
    static std::array<float, kWeatherLayers> targetGains(const WeatherSoundState& state, float farDistance);

private:
    struct Layer {
        std::string path;
        assets::AssetRef<SoundAsset> asset;
        SoundHandle handle{};
        std::uint32_t version = 0; // of the asset when the sound started
        float gain = 0.0f;
        double silent = 0.0; // seconds the layer has been inaudible while its sound plays
    };

    void start(Layer& layer, float volume, float fadeIn);

    AudioSystem& m_audio;
    WeatherSoundDesc m_desc;
    BusId m_bus{};
    std::array<Layer, kWeatherLayers> m_layers{};
    bool m_enabled = true;
};

} // namespace quench::audio
