// Audio: the mixer on the engine's audio output. A listener (the head at the center, turned with the camera) hears an
// emitter circling in front of it, a fly-by passing at speed (Doppler) and a tone the demo computes on the audio thread
// (a Generator) at a position the sliders set. Music streams from a file and crossfades between two tracks; the bus
// sliders, mute and pause boxes and the meters show the bus tree. A click or tap on the ground plays a ping there.

#include "Demo.h"

#include <quench/audio/AudioSystem.h>
#include <quench/audio/Device.h>
#include <quench/audio/Spatial.h>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;

constexpr glm::vec3 kHead{0.0f, 1.2f, 0.0f};
constexpr glm::vec3 kOrbitCenter{0.0f, 1.2f, -4.0f};
constexpr float kOrbitRadius = 5.0f;
constexpr float kFlybyHalfLength = 40.0f;
constexpr float kFlybyZ = -3.0f;
constexpr float kEmitterPriority = 5.0f; // above the pings, so a burst of pings steals only pings
constexpr float kCrossfade = 2.0f;
constexpr float kMusicVolume = 0.3f;
constexpr const char* kTracks[] = {"sounds/music_a.wav", "sounds/music_b.wav"};

// A sine the demo computes on the audio thread; its frequency glides (about 10 ms) to the one the game thread sets.
class Tone final : public audio::Generator {
public:
    explicit Tone(float frequency) : Generator({1, 48000}), m_target(frequency), m_frequency(frequency) {}
    void setFrequency(float frequency) { m_target.store(frequency, std::memory_order_relaxed); }

    std::uint32_t generate(float* out, std::uint32_t frames) override {
        const double target = m_target.load(std::memory_order_relaxed);
        for (std::uint32_t i = 0; i < frames; ++i) {
            m_frequency += (target - m_frequency) * 0.002;
            m_phase += 2.0 * std::numbers::pi * m_frequency / 48000.0;
            if (m_phase > 2.0 * std::numbers::pi) m_phase -= 2.0 * std::numbers::pi;
            out[i] = float(0.25 * std::sin(m_phase));
        }
        return frames;
    }

private:
    std::atomic<float> m_target;
    double m_frequency;
    double m_phase = 0.0;
};

class AudioSection final : public Section {
public:
    AudioSection() {
        camera.target = glm::vec3(0.0f, 0.8f, -2.5f);
        camera.distance = 13.0f;
        camera.yaw = 0.0f;
        camera.pitch = glm::radians(34.0f);
        camera.maxDistance = 70.0f;
    }

    std::string_view id() const override { return "audio"; }
    std::string_view title() const override { return "Audio"; }
    std::string_view about() const override {
        return "3D sound around the head (it faces the way the camera looks): the orange emitter circles, the blue "
               "one passes at speed (Doppler), the green box is a tone the demo computes. Music streams and "
               "crossfades; the buses have volume, mute and pause. Tap the ground for a ping.";
    }

    Result<void> init(Context& c) override {
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> ground = addMesh(s, planeMesh(glm::vec2(50.0f), 50.0f));
        const Result<MeshId> sphere = addMesh(s, sphereMesh(0.5f, 24, 48));
        const Result<MeshId> box = addMesh(s, boxMesh(glm::vec3(0.5f)));
        const Result<MeshId> ring = addMesh(s, torusMesh(kOrbitRadius, 0.025f, 128, 8));
        const Result<MeshId> lane = addMesh(s, boxMesh(glm::vec3(kFlybyHalfLength, 0.025f, 0.025f)));
        for (const Result<MeshId>* m : {&ground, &sphere, &box, &ring, &lane})
            if (!*m) return Error{m->error()};

        s.addInstance({.mesh = *ground,
                       .material = addCheckerMaterial(c, s, glm::vec3(0.42f, 0.46f, 0.50f),
                                                      glm::vec3(0.32f, 0.35f, 0.38f), 2.0f)});
        const auto glowing = [&](const glm::vec3& color) {
            return s.addMaterial({.baseColor = glm::vec4(color, 1.0f), .emissive = color * 0.6f, .roughness = 0.4f});
        };
        const MaterialId skin = s.addMaterial({.baseColor = glm::vec4(0.85f, 0.78f, 0.70f, 1.0f), .roughness = 0.6f});
        const MaterialId paint = s.addMaterial({.baseColor = glm::vec4(0.2f, 0.2f, 0.22f, 1.0f), .roughness = 0.5f});
        m_head = s.addInstance({.mesh = *sphere, .material = skin, .transform = placed(kHead, glm::vec3(0.8f))});
        m_nose = s.addInstance({.mesh = *box, .material = skin});
        for (InstanceId& ear : m_ears) ear = s.addInstance({.mesh = *sphere, .material = paint});
        m_orbiter = s.addInstance({.mesh = *sphere, .material = glowing({1.0f, 0.45f, 0.1f})});
        m_flyby = s.addInstance({.mesh = *box, .material = glowing({0.15f, 0.45f, 1.0f})});
        m_tone = s.addInstance({.mesh = *box, .material = glowing({0.2f, 0.85f, 0.3f})});
        // The paths at ear height, where the emitters move.
        s.addInstance({.mesh = *ring, .material = paint, .transform = placed(kOrbitCenter), .castsShadows = false});
        s.addInstance(
            {.mesh = *lane, .material = paint, .transform = placed({0.0f, kHead.y, kFlybyZ}), .castsShadows = false});
        m_sun = s.addLight({.type = LightType::Directional});
        setDaylight(s, m_sun, 40.0f, 35.0f);

        audio::AudioSystem& a = c.app.audio();
        m_motor = a.load("sounds/motor.wav");
        m_siren = a.load("sounds/siren.wav");
        m_ping = a.load("sounds/ping.wav");
        m_toneSource = std::make_shared<Tone>(m_toneHz);
        // The buses are the app's: shutdown puts back what the controls change.
        for (std::uint32_t i = 0; i < a.busCount(); ++i) {
            const audio::BusInfo& info = a.busInfo(audio::BusId{std::uint8_t(i)});
            m_savedBuses.push_back({info.volume, info.muted});
        }
        place(c);
        return {};
    }

    void enter(Context& c) override {
        m_active = true;
        // The output meter of the panel (scripted runs keep the app's monitor: audio_measure reads it).
        if (!c.app.scripted()) c.app.audio().setMonitor(0.25);
        restart(c);
        // Also when the track of the last visit is still fading out: it crossfades into the new start.
        if (m_track > 0) c.app.audio().playMusic(kTracks[m_track - 1], {.crossfade = 0.5f, .volume = kMusicVolume});
    }

    void leave(Context& c) override {
        audio::AudioSystem& a = c.app.audio();
        for (audio::SoundHandle* h : {&m_orbitSound, &m_flybySound, &m_toneSound}) {
            a.stop(*h, 0.2f);
            *h = {};
        }
        a.stopMusic(0.5f);
        if (!c.app.scripted()) a.setMonitor(0.0);
        m_active = false;
    }

    void update(Context& c, double frameDt) override {
        const float dt = float(frameDt);
        m_orbitAngle = std::fmod(m_orbitAngle + m_orbitSpeed / kOrbitRadius * dt, 2.0f * std::numbers::pi_v<float>);
        m_flybyX += m_flybyDirection * m_flybySpeed * dt;
        if (std::abs(m_flybyX) > kFlybyHalfLength) {
            m_flybyX = std::copysign(2.0f * kFlybyHalfLength, m_flybyX) - m_flybyX;
            m_flybyDirection = -m_flybyDirection;
        }
        m_toneSource->setFrequency(m_toneHz);
        place(c);
    }

    void controls(Context& c) override {
        audio::AudioSystem& a = c.app.audio();
        ImGui::SeparatorText("Buses");
        for (std::uint32_t i = 0; i < a.busCount(); ++i) {
            const audio::BusId bus{std::uint8_t(i)};
            const audio::BusInfo& info = a.busInfo(bus);
            ImGui::PushID(int(i));
            float volume = info.volume;
            if (ImGui::SliderFloat(info.name.c_str(), &volume, 0.0f, 1.0f, "%.2f")) a.setBusVolume(bus, volume);
            bool muted = info.muted;
            if (ImGui::Checkbox("Mute", &muted)) a.setBusMuted(bus, muted);
            ImGui::SameLine();
            const float peak = std::max(info.peak[0], info.peak[1]);
            ImGui::ProgressBar(std::min(peak, 1.0f), ImVec2(-1.0f, 0.0f), std::format("{:.2f}", peak).c_str());
            ImGui::PopID();
        }
        if (ImGui::Checkbox("Pause effects", &m_pauseEffects)) a.setBusPaused(a.bus("effects"), m_pauseEffects);
        if (ImGui::Button("Ping")) ping(c);
        ImGui::SameLine();
        if (ImGui::Button("40 pings")) burst(c, 40);

        ImGui::SeparatorText("Music");
        int track = m_track;
        bool changed = ImGui::RadioButton("Off", &track, 0);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("A", &track, 1);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("B", &track, 2);
        if (changed) setTrack(c, track);

        ImGui::SeparatorText("Emitters");
        if (ImGui::Checkbox("Circling", &m_orbitOn)) restart(c);
        ImGui::SliderFloat("Speed##orbit", &m_orbitSpeed, 0.0f, 30.0f, "%.0f m/s");
        if (ImGui::Checkbox("Fly-by", &m_flybyOn)) restart(c);
        ImGui::SliderFloat("Speed##flyby", &m_flybySpeed, 5.0f, 80.0f, "%.0f m/s");
        ImGui::Text("Fly-by pitch %.3f", flybyPitch());
        if (ImGui::Checkbox("Tone", &m_toneOn)) restart(c);
        ImGui::SliderFloat("Frequency", &m_toneHz, 100.0f, 2000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat2("Position", &m_tonePosition.x, -15.0f, 15.0f, "%.1f");

        ImGui::SeparatorText("Distance");
        int model = int(m_attenuation);
        if (ImGui::Combo("Model", &model,
                         "none\0"
                         "inverse\0"
                         "linear\0"
                         "exponential\0"))
            m_attenuation = audio::Attenuation(model);
        ImGui::SliderFloat("Min (m)", &m_minDistance, 0.5f, 10.0f, "%.1f");
        ImGui::SliderFloat("Max (m)", &m_maxDistance, 5.0f, 100.0f, "%.0f");
        ImGui::SliderFloat("Doppler", &m_doppler, 0.0f, 2.0f, "%.2f");
    }

    void stats(Context& c) override {
        audio::AudioSystem& a = c.app.audio();
        const audio::AudioStats stats = a.stats();
        ImGui::Text("Voices %u of %u, sounds %u", stats.voices, stats.maxVoices, stats.sounds);
        ImGui::Text("Stolen %llu, rejected %llu", (unsigned long long)stats.stolen, (unsigned long long)stats.rejected);
        ImGui::Text("Underruns %llu, clipped %llu", (unsigned long long)stats.underruns,
                    (unsigned long long)stats.clipped);
        if (const audio::Device* device = c.app.audioDevice())
            ImGui::TextWrapped("Output: %s (%s), %u Hz", device->info().name.c_str(), device->info().backend.c_str(),
                               device->info().deviceSampleRate);
        else
            ImGui::TextWrapped("Output: none, offline mix at %u Hz", a.sampleRate());
        if (c.app.deterministic()) {
            ImGui::TextUnformatted("Mix time and meter hidden (fixed clock)");
            return;
        }
        ImGui::TextWrapped("Mix %.3f ms per call, %.2f%% of real time", stats.mixMs, stats.load * 100.0);
        // The output's level per side over the last 0.1 s: the circling emitter moves it from ear to ear.
        const std::vector<float> mix = a.monitored(0.1);
        double sum[2] = {0.0, 0.0};
        for (std::size_t i = 0; i + 1 < mix.size(); i += 2) {
            sum[0] += double(mix[i]) * double(mix[i]);
            sum[1] += double(mix[i + 1]) * double(mix[i + 1]);
        }
        const std::size_t frames = mix.size() / 2;
        for (int side = 0; side < 2; ++side) {
            const float rms = frames > 0 ? float(std::sqrt(sum[side] / double(frames))) : 0.0f;
            const float db = 20.0f * std::log10(std::max(rms, 1e-5f));
            const std::string label = std::format("{} {:.0f} dB", side == 0 ? "L" : "R", db);
            ImGui::ProgressBar(std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), label.c_str());
        }
    }

    void click(Context& c, glm::vec2 point) override {
        const std::optional<glm::vec3> hit = groundPoint(c, camera, point);
        if (!hit) return;
        c.app.audio().play(
            m_ping, {.bus = c.app.audio().bus("effects"), .spatial = true, .emitter = emitter(*hit, glm::vec3(0.0f))});
        ++m_pings;
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        if (name == "sounds_ready") return m_motor.ready() && m_siren.ready() && m_ping.ready() ? 1.0 : 0.0;
        if (name == "music_track") return double(m_track);
        if (name == "flyby_x") return double(m_flybyX);
        if (name == "flyby_pitch") return double(flybyPitch());
        if (name == "pings") return double(m_pings);
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        const std::optional<float> number = parseFloat(value);
        const std::optional<bool> flag = parseBool(value);
        audio::AudioSystem& a = c.app.audio();
        if (name == "orbit" && flag) {
            m_orbitOn = *flag;
            m_orbitAngle = 0.0f;
        } else if (name == "flyby" && flag) {
            m_flybyOn = *flag;
            m_flybyX = -kFlybyHalfLength;
            m_flybyDirection = 1.0f;
        } else if (name == "tone" && flag) {
            m_toneOn = *flag;
        } else if (name == "tone_hz" && number) {
            m_toneHz = std::clamp(*number, 20.0f, 20000.0f);
        } else if (name == "tone_x" && number) {
            m_tonePosition.x = *number;
        } else if (name == "tone_z" && number) {
            m_tonePosition.y = *number;
        } else if (name == "orbit_speed" && number) {
            m_orbitSpeed = std::max(*number, 0.0f);
        } else if (name == "flyby_speed" && number) {
            m_flybySpeed = std::max(*number, 0.0f);
        } else if (name == "attenuation") {
            const std::optional<audio::Attenuation> model = audio::parseAttenuation(value);
            if (!model) return false;
            m_attenuation = *model;
        } else if (name == "min_distance" && number && *number > 0.0f) {
            m_minDistance = *number;
        } else if (name == "max_distance" && number && *number > 0.0f) {
            m_maxDistance = *number;
        } else if (name == "doppler" && number) {
            m_doppler = std::max(*number, 0.0f);
        } else if (name == "music" && number && *number >= 0.0f && *number <= 2.0f) {
            setTrack(c, int(*number));
            return true;
        } else if (name.starts_with("bus_") && number && a.findBus(name.substr(4))) {
            a.setBusVolume(*a.findBus(name.substr(4)), std::clamp(*number, 0.0f, 1.0f));
            return true;
        } else if (name.starts_with("mute_") && flag && a.findBus(name.substr(5))) {
            a.setBusMuted(*a.findBus(name.substr(5)), *flag);
            return true;
        } else if (name == "pause_effects" && flag) {
            m_pauseEffects = *flag;
            a.setBusPaused(a.bus("effects"), m_pauseEffects);
            return true;
        } else if (name == "ping" && flag) {
            if (*flag) ping(c);
            return true;
        } else if (name == "burst" && number && *number >= 1.0f) {
            burst(c, int(*number));
            return true;
        } else {
            return false;
        }
        restart(c);
        place(c);
        return true;
    }

    void shutdown(Context& c) override {
        audio::AudioSystem& a = c.app.audio();
        for (std::size_t i = 0; i < m_savedBuses.size(); ++i) {
            const audio::BusId bus{std::uint8_t(i)};
            a.setBusVolume(bus, m_savedBuses[i].first);
            a.setBusMuted(bus, m_savedBuses[i].second);
        }
        if (m_pauseEffects) a.setBusPaused(a.bus("effects"), false);
        m_motor = {};
        m_siren = {};
        m_ping = {};
        m_scene.reset();
    }

private:
    audio::Emitter emitter(const glm::vec3& position, const glm::vec3& velocity) const {
        return {.position = position,
                .velocity = velocity,
                .attenuation = m_attenuation,
                .minDistance = m_minDistance,
                .maxDistance = std::max(m_maxDistance, m_minDistance + 0.1f),
                .doppler = m_doppler};
    }

    audio::Listener listener() const {
        return {.position = kHead, .forward = glm::vec3(-std::sin(camera.yaw), 0.0f, -std::cos(camera.yaw))};
    }

    glm::vec3 orbitPosition() const {
        return kOrbitCenter + kOrbitRadius * glm::vec3(std::sin(m_orbitAngle), 0.0f, std::cos(m_orbitAngle));
    }
    glm::vec3 orbitVelocity() const {
        return m_orbitSpeed * glm::vec3(std::cos(m_orbitAngle), 0.0f, -std::sin(m_orbitAngle));
    }
    glm::vec3 flybyPosition() const { return {m_flybyX, kHead.y, kFlybyZ}; }
    glm::vec3 flybyVelocity() const { return {m_flybyDirection * m_flybySpeed, 0.0f, 0.0f}; }
    glm::vec3 tonePosition() const { return {m_tonePosition.x, kHead.y, m_tonePosition.y}; }

    float flybyPitch() const { return audio::spatialize(listener(), emitter(flybyPosition(), flybyVelocity())).pitch; }

    // Starts the sounds that are switched on and not playing, stops the ones switched off.
    void restart(Context& c) {
        if (!m_active) return;
        audio::AudioSystem& a = c.app.audio();
        const audio::BusId effects = a.bus("effects");
        const auto keep = [&](audio::SoundHandle& handle, bool on, auto start) {
            if (on && !a.alive(handle)) handle = start();
            if (!on && handle.valid()) {
                a.stop(handle, 0.1f);
                handle = {};
            }
        };
        const audio::PlayParams loop{
            .bus = effects, .loop = true, .priority = kEmitterPriority, .fadeIn = 0.05f, .spatial = true};
        keep(m_orbitSound, m_orbitOn, [&] {
            audio::PlayParams p = loop;
            p.volume = 0.4f;
            p.emitter = emitter(orbitPosition(), orbitVelocity());
            return a.play(m_motor, p);
        });
        keep(m_flybySound, m_flybyOn, [&] {
            audio::PlayParams p = loop;
            p.volume = 0.6f;
            p.emitter = emitter(flybyPosition(), flybyVelocity());
            return a.play(m_siren, p);
        });
        keep(m_toneSound, m_toneOn, [&] {
            audio::PlayParams p = loop;
            p.emitter = emitter(tonePosition(), glm::vec3(0.0f));
            return a.playGenerator(m_toneSource, p);
        });
    }

    void setTrack(Context& c, int track) {
        m_track = track;
        if (!m_active) return;
        audio::AudioSystem& a = c.app.audio();
        if (track == 0)
            a.stopMusic(kCrossfade);
        else
            a.playMusic(kTracks[track - 1], {.crossfade = kCrossfade, .volume = kMusicVolume});
    }

    void ping(Context& c) {
        c.app.audio().play(m_ping, {.bus = c.app.audio().bus("ui"), .volume = 0.6f});
        ++m_pings;
    }

    // One-shots spread around the head, more than there are voices: the newest steal the oldest.
    void burst(Context& c, int count) {
        audio::AudioSystem& a = c.app.audio();
        for (int i = 0; i < count; ++i) {
            const float angle = 2.0f * std::numbers::pi_v<float> * float(i) / float(count);
            const glm::vec3 at = kHead + 3.0f * glm::vec3(std::sin(angle), 0.0f, -std::cos(angle));
            a.play(m_ping, {.bus = a.bus("effects"),
                            .volume = 0.1f,
                            .pitch = 0.75f + 0.5f * float(i % 5) / 4.0f,
                            .spatial = true,
                            .emitter = emitter(at, glm::vec3(0.0f))});
        }
        m_pings += count;
    }

    void place(Context& c) {
        Scene& s = *m_scene;
        const glm::vec3 forward = listener().forward;
        const glm::vec3 right(-forward.z, 0.0f, forward.x);
        s.setTransform(m_nose, placed(kHead + 0.42f * forward, glm::vec3(0.12f, 0.12f, 0.25f), camera.yaw));
        s.setTransform(m_ears[0], placed(kHead - 0.4f * right, glm::vec3(0.22f)));
        s.setTransform(m_ears[1], placed(kHead + 0.4f * right, glm::vec3(0.22f)));
        s.setTransform(m_orbiter, placed(orbitPosition(), glm::vec3(0.7f)));
        s.setTransform(m_flyby, placed(flybyPosition(), glm::vec3(1.4f, 0.5f, 0.6f)));
        s.setTransform(m_tone, placed(tonePosition(), glm::vec3(0.6f)));
        s.setVisible(m_orbiter, m_orbitOn);
        s.setVisible(m_flyby, m_flybyOn);
        s.setVisible(m_tone, m_toneOn);
        if (!m_active) return;
        audio::AudioSystem& a = c.app.audio();
        a.setListener(listener());
        a.setEmitter(m_orbitSound, emitter(orbitPosition(), orbitVelocity()));
        a.setEmitter(m_flybySound, emitter(flybyPosition(), flybyVelocity()));
        a.setEmitter(m_toneSound, emitter(tonePosition(), glm::vec3(0.0f)));
    }

    std::optional<Scene> m_scene;
    InstanceId m_head, m_nose, m_orbiter, m_flyby, m_tone;
    std::array<InstanceId, 2> m_ears;
    LightId m_sun;
    assets::AssetRef<audio::SoundAsset> m_motor, m_siren, m_ping;
    std::shared_ptr<Tone> m_toneSource;
    audio::SoundHandle m_orbitSound, m_flybySound, m_toneSound;
    bool m_active = false;
    bool m_orbitOn = true, m_flybyOn = true, m_toneOn = false, m_pauseEffects = false;
    float m_orbitAngle = 0.0f, m_orbitSpeed = 8.0f;
    float m_flybyX = -kFlybyHalfLength, m_flybyDirection = 1.0f, m_flybySpeed = 30.0f;
    float m_toneHz = 660.0f;
    glm::vec2 m_tonePosition{4.0f, -1.0f}; // x, z
    audio::Attenuation m_attenuation = audio::Attenuation::Inverse;
    float m_minDistance = 1.0f, m_maxDistance = 40.0f, m_doppler = 1.0f;
    int m_track = 1;
    int m_pings = 0;
    std::vector<std::pair<float, bool>> m_savedBuses; // volume, muted
};

} // namespace

std::unique_ptr<Section> makeAudioSection() { return std::make_unique<AudioSection>(); }

} // namespace demo
