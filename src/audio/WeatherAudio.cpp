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
constexpr float kWindHeard = 1.0f;      // m/s: a wind below this is not heard
constexpr float kWindFull = 20.0f;      // m/s: the wind's layers at full loudness

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
    case WeatherLayer::WindLight: return "wind_light";
    case WeatherLayer::WindStrong: return "wind_strong";
    }
    return "?";
}

const char* defaultWeatherSound(WeatherLayer layer) {
    switch (layer) {
    case WeatherLayer::RainLight: return "quench/weather/rain_light.wav";
    case WeatherLayer::RainHeavy: return "quench/weather/rain_heavy.wav";
    case WeatherLayer::RainFar: return "quench/weather/rain_far.wav";
    case WeatherLayer::RainCover: return "quench/weather/rain_cover.wav";
    case WeatherLayer::WindLight: return "quench/weather/wind_light.wav";
    case WeatherLayer::WindStrong: return "quench/weather/wind_strong.wav";
    }
    return "";
}

const char* defaultThunderSound(Thunder thunder) {
    return thunder == Thunder::Near ? "quench/weather/thunder_near.wav" : "quench/weather/thunder_far.wav";
}

std::array<float, kWeatherLayers> WeatherAudio::targetGains(const WeatherSoundState& state, float farDistance) {
    const float total = std::clamp(state.precipitation, 0.0f, 1.0f);
    const float share = std::clamp(state.snow, 0.0f, 1.0f);
    const float p = total * (1.0f - share);
    const float cover = std::clamp(state.cover, 0.0f, 1.0f);
    const float level = p > kRainThreshold ? std::sqrt((p - kRainThreshold) / (1.0f - kRainThreshold)) : 0.0f;
    const float heavy = smoothstep(0.25f, 0.75f, p);
    const float near = std::clamp(1.0f - state.distanceToRain / std::max(farDistance, 1.0f), 0.0f, 1.0f);
    // Falling snow muffles what is heard from afar.
    const float muffled = 1.0f - 0.6f * total * share;
    std::array<float, kWeatherLayers> g{};
    g[std::size_t(WeatherLayer::RainLight)] = (1.0f - cover) * level * std::sqrt(1.0f - heavy);
    g[std::size_t(WeatherLayer::RainHeavy)] = (1.0f - cover) * level * std::sqrt(heavy);
    g[std::size_t(WeatherLayer::RainFar)] =
        near * near * (1.0f - std::clamp(p / 0.1f, 0.0f, 1.0f)) * (1.0f - 0.5f * cover) * muffled;
    g[std::size_t(WeatherLayer::RainCover)] = cover * level;
    const float wind = std::max(state.wind, 0.0f);
    const float blowing = std::sqrt(std::clamp((wind - kWindHeard) / (kWindFull - kWindHeard), 0.0f, 1.0f));
    const float strong = smoothstep(6.0f, 16.0f, wind);
    const float sheltered = 1.0f - 0.5f * cover;
    g[std::size_t(WeatherLayer::WindLight)] = sheltered * blowing * std::sqrt(1.0f - strong);
    g[std::size_t(WeatherLayer::WindStrong)] = sheltered * blowing * std::sqrt(strong);
    return g;
}

std::array<float, kThunderSounds> WeatherAudio::thunderGains(float distance, float brightness, float cover,
                                                             float reach) {
    distance = std::max(distance, 0.0f);
    if (distance > reach) return {};
    const float loud =
        std::max(brightness, 0.0f) / (1.0f + distance / 1500.0f) * (1.0f - 0.3f * std::clamp(cover, 0.0f, 1.0f));
    const float near = 1.0f - smoothstep(1000.0f, 5000.0f, distance);
    return {loud * std::sqrt(near), loud * std::sqrt(1.0f - near)};
}

WeatherAudio::WeatherAudio(AudioSystem& audio, WeatherSoundDesc desc) : m_audio(audio), m_desc(std::move(desc)) {
    m_bus = m_audio.bus(m_desc.bus);
    for (std::size_t i = 0; i < kWeatherLayers; ++i) {
        Layer& layer = m_layers[i];
        layer.path = m_desc.sounds[i];
        if (!layer.path.empty()) layer.asset = m_audio.load(layer.path, kPriority);
    }
    for (std::size_t i = 0; i < kThunderSounds; ++i) setThunderSound(Thunder(i), m_desc.thunder[i]);
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

void WeatherAudio::setThunderSound(Thunder thunder, std::string_view path) {
    const std::size_t i = std::size_t(thunder);
    if (m_thunderSounds[i] && m_thunderPaths[i] == path) return;
    m_thunderPaths[i] = std::string(path);
    m_desc.thunder[i] = m_thunderPaths[i];
    m_thunderSounds[i] = path.empty() ? assets::AssetRef<SoundAsset>{} : m_audio.load(m_thunderPaths[i], kPriority);
}

const std::string& WeatherAudio::thunderPath(Thunder thunder) const { return m_thunderPaths[std::size_t(thunder)]; }

void WeatherAudio::strike(float distance, float brightness) {
    if (!m_enabled || !(distance >= 0.0f) || distance > m_desc.thunderReach) return;
    if (m_thunder.size() >= kMaxThunder) {
        const auto farthest =
            std::max_element(m_thunder.begin(), m_thunder.end(),
                             [](const Waiting& a, const Waiting& b) { return a.distance < b.distance; });
        if (farthest->distance <= distance) return;
        m_thunder.erase(farthest);
    }
    m_thunder.push_back({m_clock + double(distance / kSpeedOfSound), distance, brightness});
}

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
    m_thunder.clear();
}

void WeatherAudio::start(Layer& layer, float volume, float fadeIn) {
    PlayParams params{.bus = m_bus, .volume = volume, .loop = true, .priority = kPriority, .fadeIn = fadeIn};
    layer.handle = m_audio.play(layer.asset, params);
    layer.version = layer.asset->version();
    layer.silent = 0.0;
}

void WeatherAudio::update(const WeatherSoundState& state, double dt) {
    m_clock += std::max(dt, 0.0);
    if (!m_enabled) return;
    m_cover = state.cover;
    // The thunder whose sound has arrived: one-shots at the gains of their distance.
    for (std::size_t i = 0; i < m_thunder.size();) {
        const Waiting w = m_thunder[i];
        if (w.at > m_clock) {
            ++i;
            continue;
        }
        m_thunder.erase(m_thunder.begin() + std::ptrdiff_t(i));
        const std::array<float, kThunderSounds> g =
            thunderGains(w.distance, w.brightness, m_cover, m_desc.thunderReach);
        bool played = false;
        for (std::size_t k = 0; k < kThunderSounds; ++k) {
            const float volume = g[k] * m_desc.volume;
            if (volume <= kAudible || !m_thunderSounds[k] || !m_thunderSounds[k].ready()) continue;
            m_audio.play(m_thunderSounds[k], {.bus = m_bus, .volume = volume, .priority = kPriority});
            played = true;
        }
        if (played) ++m_thunderPlayed;
    }
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
