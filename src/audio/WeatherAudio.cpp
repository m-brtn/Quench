#include <quench/audio/WeatherAudio.h>

#include <algorithm>
#include <cmath>

namespace quench::audio {

namespace {

constexpr float kAudible = 1e-4f;       // a layer below this gain is silent
constexpr double kStopAfter = 2.0;      // s of silence before a layer's sound stops
constexpr float kStartFade = 0.2f;      // s: a layer fades in when it starts
constexpr float kCrossfade = 0.5f;      // s: a new or reloaded sound replaces the old one
constexpr float kRainThreshold = 0.02f; // precipitation below this is not heard (weather::Weather::kWettingThreshold)
constexpr float kPriority = 100.0f;     // weather layers keep their voices against effects

float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

const char* weatherLayerName(WeatherLayer layer) {
    switch (layer) {
    case WeatherLayer::RainLight: return "rain_light";
    case WeatherLayer::RainHeavy: return "rain_heavy";
    case WeatherLayer::RainFar: return "rain_far";
    case WeatherLayer::RainCover: return "rain_cover";
    }
    return "?";
}

const char* defaultWeatherSound(WeatherLayer layer) {
    switch (layer) {
    case WeatherLayer::RainLight: return "quench/weather/rain_light.wav";
    case WeatherLayer::RainHeavy: return "quench/weather/rain_heavy.wav";
    case WeatherLayer::RainFar: return "quench/weather/rain_far.wav";
    case WeatherLayer::RainCover: return "quench/weather/rain_cover.wav";
    }
    return "";
}

std::array<float, kWeatherLayers> WeatherAudio::targetGains(const WeatherSoundState& state, float farDistance) {
    const float p = std::clamp(state.precipitation, 0.0f, 1.0f);
    const float cover = std::clamp(state.cover, 0.0f, 1.0f);
    const float level = p > kRainThreshold ? std::sqrt((p - kRainThreshold) / (1.0f - kRainThreshold)) : 0.0f;
    const float heavy = smoothstep(0.25f, 0.75f, p);
    const float near = std::clamp(1.0f - state.distanceToRain / std::max(farDistance, 1.0f), 0.0f, 1.0f);
    std::array<float, kWeatherLayers> g{};
    g[std::size_t(WeatherLayer::RainLight)] = (1.0f - cover) * level * std::sqrt(1.0f - heavy);
    g[std::size_t(WeatherLayer::RainHeavy)] = (1.0f - cover) * level * std::sqrt(heavy);
    g[std::size_t(WeatherLayer::RainFar)] =
        near * near * (1.0f - std::clamp(p / 0.1f, 0.0f, 1.0f)) * (1.0f - 0.5f * cover);
    g[std::size_t(WeatherLayer::RainCover)] = cover * level;
    return g;
}

WeatherAudio::WeatherAudio(AudioSystem& audio, WeatherSoundDesc desc) : m_audio(audio), m_desc(std::move(desc)) {
    m_bus = m_audio.bus(m_desc.bus);
    for (std::size_t i = 0; i < kWeatherLayers; ++i) {
        Layer& layer = m_layers[i];
        layer.path = m_desc.sounds[i];
        if (!layer.path.empty()) layer.asset = m_audio.load(layer.path, kPriority);
    }
}

WeatherAudio::~WeatherAudio() {
    for (Layer& layer : m_layers)
        if (layer.handle.valid()) m_audio.stop(layer.handle, kStartFade);
}

void WeatherAudio::setSound(WeatherLayer which, std::string_view path) {
    Layer& layer = m_layers[std::size_t(which)];
    if (layer.path == path) return;
    layer.path = std::string(path);
    m_desc.sounds[std::size_t(which)] = layer.path;
    layer.asset = layer.path.empty() ? assets::AssetRef<SoundAsset>{} : m_audio.load(layer.path, kPriority);
    // The playing sound crossfades into the new one once it is loaded (update()); a silenced layer fades out now.
    if (layer.path.empty() && layer.handle.valid()) {
        m_audio.stop(layer.handle, kCrossfade);
        layer.handle = {};
    }
    layer.version = ~0u;
}

const std::string& WeatherAudio::soundPath(WeatherLayer layer) const { return m_layers[std::size_t(layer)].path; }

void WeatherAudio::setBus(std::string_view name) {
    m_desc.bus = std::string(name);
    m_bus = m_audio.bus(m_desc.bus);
    // Sounds keep the bus they started on: restart the playing ones on the new bus.
    for (Layer& layer : m_layers)
        if (layer.handle.valid()) layer.version = ~0u;
}

void WeatherAudio::setVolume(float volume) { m_desc.volume = std::max(volume, 0.0f); }

void WeatherAudio::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (enabled) return;
    for (Layer& layer : m_layers) {
        if (layer.handle.valid()) m_audio.stop(layer.handle, kCrossfade);
        layer.handle = {};
        layer.gain = 0.0f;
        layer.silent = 0.0;
    }
}

void WeatherAudio::start(Layer& layer, float volume, float fadeIn) {
    PlayParams params{.bus = m_bus, .volume = volume, .loop = true, .priority = kPriority, .fadeIn = fadeIn};
    layer.handle = m_audio.play(layer.asset, params);
    layer.version = layer.asset->version();
    layer.silent = 0.0;
}

void WeatherAudio::update(const WeatherSoundState& state, double dt) {
    if (!m_enabled) return;
    const std::array<float, kWeatherLayers> target = targetGains(state, m_desc.farDistance);
    const float follow =
        m_desc.smoothing > 0.0f ? float(1.0 - std::exp(-std::max(dt, 0.0) / double(m_desc.smoothing))) : 1.0f;
    for (std::size_t i = 0; i < kWeatherLayers; ++i) {
        Layer& layer = m_layers[i];
        layer.gain += (target[i] - layer.gain) * follow;
        if (layer.gain < kAudible * 0.01f && target[i] == 0.0f) layer.gain = 0.0f;
        const float volume = layer.gain * m_desc.volume;
        if (layer.handle.valid() && !m_audio.alive(layer.handle)) layer.handle = {};
        if (!layer.asset || !layer.asset.ready()) continue;
        if (volume <= kAudible) {
            if (!layer.handle.valid()) continue;
            layer.silent += dt;
            m_audio.setVolume(layer.handle, 0.0f);
            if (layer.silent >= kStopAfter) {
                m_audio.stop(layer.handle, kStartFade);
                layer.handle = {};
            }
            continue;
        }
        if (!layer.handle.valid()) {
            start(layer, volume, kStartFade);
        } else if (layer.version != layer.asset->version()) {
            // A reloaded file, another sound or another bus: the new sound fades in over the old one.
            m_audio.stop(layer.handle, kCrossfade);
            start(layer, volume, kCrossfade);
        } else {
            m_audio.setVolume(layer.handle, volume);
            layer.silent = 0.0;
        }
    }
}

} // namespace quench::audio
