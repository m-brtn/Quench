#include "Test.h"

#include <quench/assets/AssetManager.h>
#include <quench/audio/Analysis.h>
#include <quench/audio/AudioSystem.h>
#include <quench/audio/Wav.h>
#include <quench/audio/WeatherAudio.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Jobs.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <numbers>
#include <span>
#include <string>
#include <thread>
#include <vector>

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#define QUENCH_TEST_THREADS 1
#else
#define QUENCH_TEST_THREADS 0
#endif

// The global operator new and delete of this test program, counting the calls of a thread while it has an
// AllocationCounter (the audio thread's rule: render() allocates and frees nothing).
namespace {
thread_local bool g_countAllocations = false;
thread_local std::uint64_t g_allocations = 0;
} // namespace

void* operator new(std::size_t size) {
    if (g_countAllocations) ++g_allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept {
    if (p && g_countAllocations) ++g_allocations;
    std::free(p);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

using namespace quench;
using namespace quench::audio;

namespace quench::test {
// tests/audio/data, embedded by tests/CMakeLists.txt.
std::span<const std::uint8_t> toneFlac();
std::span<const std::uint8_t> toneMp3();
std::span<const std::uint8_t> loopOgg();
} // namespace quench::test

namespace {

constexpr std::uint32_t kRate = 48000;

class Sine final : public Generator {
public:
    Sine(float frequency, float amplitude = 0.5f, std::uint32_t rate = kRate, std::uint32_t channels = 1)
        : Generator({channels, rate}), m_step(2.0 * std::numbers::pi * frequency / rate), m_amplitude(amplitude) {}

    std::uint32_t generate(float* out, std::uint32_t frames) override {
        const std::uint32_t channels = format().channels;
        for (std::uint32_t f = 0; f < frames; ++f) {
            const auto value = float(m_amplitude * std::sin(m_phase));
            m_phase = std::fmod(m_phase + m_step, 2.0 * std::numbers::pi);
            for (std::uint32_t c = 0; c < channels; ++c) out[f * channels + c] = value;
        }
        return frames;
    }

private:
    double m_phase = 0.0, m_step;
    double m_amplitude;
};

// A constant per channel (stereo, so the balance leaves it unchanged at pan 0).
class Constant final : public Generator {
public:
    explicit Constant(float value, std::uint32_t channels = 2) : Generator({channels, kRate}), m_value(value) {}
    std::uint32_t generate(float* out, std::uint32_t frames) override {
        std::fill(out, out + std::size_t(frames) * format().channels, m_value);
        return frames;
    }

private:
    float m_value;
};

// Mono white noise, uniform in [-amplitude, amplitude], the same sequence every run.
class Noise final : public Generator {
public:
    explicit Noise(float amplitude, std::uint32_t rate = kRate) : Generator({1, rate}), m_amplitude(amplitude) {}
    std::uint32_t generate(float* out, std::uint32_t frames) override {
        for (std::uint32_t f = 0; f < frames; ++f) {
            m_state = m_state * 1664525u + 1013904223u;
            out[f] = m_amplitude * (float(m_state >> 8) / float(1u << 23) - 1.0f);
        }
        return frames;
    }

private:
    float m_amplitude;
    std::uint32_t m_state = 12345;
};

// An AudioSystem mixing offline on the test thread.
struct Rig {
    explicit Rig(const AudioSettings& settings = {}, int workers = 0)
        : jobs(JobSystemDesc{.workerThreads = workers}), audio(jobs, nullptr, settings) {
        audio.setOffline(true);
    }
    // `seconds` of mix in steps of `step` with an update() before each.
    std::vector<float> run(double seconds, double step = 0.01) {
        std::vector<float> out;
        for (double t = 0.0; t < seconds - 1e-9; t += step) {
            audio.update();
            audio.mixOffline(step, &out);
        }
        return out;
    }

    JobSystem jobs;
    AudioSystem audio;
};

// The last `seconds` of interleaved stereo samples.
std::span<const float> last(const std::vector<float>& samples, double seconds) {
    const auto count = std::min(samples.size(), std::size_t(seconds * kRate) * 2);
    return std::span(samples).subspan(samples.size() - count);
}

// Largest change between neighbouring frames of either channel.
float maxStep(std::span<const float> samples) {
    float step = 0.0f;
    for (std::size_t i = 2; i < samples.size(); ++i) step = std::max(step, std::abs(samples[i] - samples[i - 2]));
    return step;
}

bool near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }

// One channel of interleaved stereo.
std::vector<float> channelOf(std::span<const float> stereo, std::uint32_t channel) {
    std::vector<float> mono(stereo.size() / 2);
    for (std::size_t i = 0; i < mono.size(); ++i) mono[i] = stereo[i * 2 + channel];
    return mono;
}

// tests/audio/data/loop.ogg: 1 s at 48 kHz, 0.5 * sin of 440 Hz on the left and 880 Hz on the right.
constexpr float kLoopOggHz[2] = {440.0f, 880.0f};

// Allocations and frees of this thread while it lives.
class AllocationCounter {
public:
    AllocationCounter() : m_start(g_allocations) { g_countAllocations = true; }
    ~AllocationCounter() { g_countAllocations = false; }
    std::uint64_t count() const { return g_allocations - m_start; }

private:
    std::uint64_t m_start;
};

// One stereo frame of 1, then the sound ends.
class Impulse final : public Generator {
public:
    Impulse() : Generator({2, kRate}) {}
    std::uint32_t generate(float* out, std::uint32_t frames) override {
        if (m_done || frames == 0) return 0;
        out[0] = out[1] = 1.0f;
        m_done = true;
        return 1;
    }

private:
    bool m_done = false;
};

// The reverberation time of an impulse response (interleaved stereo): the time from -5 to -35 dB of its energy decay
// curve (Schroeder's backward integration of both channels), doubled (T30). 0 if it does not fall that far.
double reverberationTime(std::span<const float> stereo) {
    const std::size_t frames = stereo.size() / 2;
    std::vector<double> remaining(frames + 1, 0.0);
    for (std::size_t f = frames; f-- > 0;)
        remaining[f] =
            remaining[f + 1] + double(stereo[2 * f]) * stereo[2 * f] + double(stereo[2 * f + 1]) * stereo[2 * f + 1];
    if (remaining[0] <= 0.0) return 0.0;
    std::size_t at5 = 0, at35 = 0;
    for (std::size_t f = 0; f < frames && at35 == 0; ++f) {
        const double db = 10.0 * std::log10(std::max(remaining[f], 1e-300) / remaining[0]);
        if (at5 == 0 && db <= -5.0) at5 = f;
        if (db <= -35.0) at35 = f;
    }
    return at35 > at5 ? 2.0 * double(at35 - at5) / kRate : 0.0;
}

// Asset files for one case, read through quench::fs; restores the default backend at the end.
struct AssetDir {
    explicit AssetDir(const char* name) : dir(std::filesystem::path(quench::test::tmpDir()) / name) {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        fs::setBackend(fs::makeStdBackend(dir, {}));
    }
    ~AssetDir() { fs::setBackend(nullptr); }
    void write(const std::string& path, std::span<const std::uint8_t> bytes) const {
        std::filesystem::create_directories((dir / path).parent_path());
        std::ofstream(dir / path, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }

    std::filesystem::path dir;
};

// A stereo PCM16 WAV whose samples are distinct integers (exact after decoding: k / 32768).
std::vector<float> patternSamples(std::uint32_t frames) {
    std::vector<float> samples(std::size_t(frames) * 2);
    for (std::uint32_t i = 0; i < frames; ++i) {
        samples[2 * i] = float(int(i * 37 % 20000) - 10000) / 32767.0f;
        samples[2 * i + 1] = float(int(i * 101 % 16000) - 8000) / 32767.0f;
    }
    return samples;
}

std::vector<float> sineSamples(float frequency, float amplitude, double seconds, std::uint32_t channels = 2) {
    const auto frames = std::size_t(seconds * kRate);
    std::vector<float> samples(frames * channels);
    for (std::size_t i = 0; i < frames; ++i)
        for (std::uint32_t c = 0; c < channels; ++c)
            samples[i * channels + c] =
                amplitude * float(std::sin(2.0 * std::numbers::pi * frequency * double(i) / kRate));
    return samples;
}

} // namespace

// ---------------------------------------------------------------------------------------------------- spatial

QUENCH_TEST(spatial_attenuation_models) {
    Emitter e{.minDistance = 2.0f, .maxDistance = 20.0f, .rolloff = 1.0f};
    e.attenuation = Attenuation::None;
    CHECK(distanceGain(e, 50.0f) == 1.0f, "none");
    e.attenuation = Attenuation::Inverse;
    CHECK(distanceGain(e, 1.0f) == 1.0f && near(distanceGain(e, 4.0f), 0.5f, 1e-6f) &&
              near(distanceGain(e, 8.0f), 0.25f, 1e-6f) && near(distanceGain(e, 100.0f), 0.1f, 1e-6f),
          "inverse: %f %f %f", distanceGain(e, 4.0f), distanceGain(e, 8.0f), distanceGain(e, 100.0f));
    e.attenuation = Attenuation::Linear;
    CHECK(near(distanceGain(e, 11.0f), 0.5f, 1e-6f) && distanceGain(e, 20.0f) == 0.0f && distanceGain(e, 2.0f) == 1.0f,
          "linear: %f", distanceGain(e, 11.0f));
    e.attenuation = Attenuation::Exponential;
    e.rolloff = 2.0f;
    CHECK(near(distanceGain(e, 4.0f), 0.25f, 1e-6f) && near(distanceGain(e, 40.0f), 0.01f, 1e-6f), "exponential: %f %f",
          distanceGain(e, 4.0f), distanceGain(e, 40.0f));
    e.attenuation = Attenuation::Inverse;
    e.rolloff = 0.5f;
    CHECK(near(distanceGain(e, 6.0f), 2.0f / (2.0f + 0.5f * 4.0f), 1e-6f), "inverse rolloff");
    CHECK(parseAttenuation("linear") == Attenuation::Linear && !parseAttenuation("loud"), "names");
}

QUENCH_TEST(spatial_pan_law) {
    for (float pan = -1.0f; pan <= 1.0f; pan += 0.125f) {
        const glm::vec2 g = panGains(pan);
        CHECK(near(g.x * g.x + g.y * g.y, 1.0f, 1e-5f), "constant power at %f", pan);
    }
    CHECK(near(panGains(-1.0f).x, 1.0f, 1e-6f) && near(panGains(-1.0f).y, 0.0f, 1e-6f), "left");
    CHECK(near(panGains(0.0f).x, 0.70710678f, 1e-6f) && near(panGains(0.0f).y, 0.70710678f, 1e-6f), "center");
    CHECK(balanceGains(0.0f) == glm::vec2(1.0f) && balanceGains(0.5f) == glm::vec2(0.5f, 1.0f), "balance");

    Listener listener; // at the origin, looking down -Z
    Emitter right{.position = {3.0f, 0.0f, 0.0f}};
    CHECK(near(spatialize(listener, right).pan, 1.0f, 1e-6f), "right: %f", spatialize(listener, right).pan);
    Emitter front{.position = {0.0f, 0.0f, -3.0f}};
    CHECK(near(spatialize(listener, front).pan, 0.0f, 1e-6f), "front");
    Emitter leftFront{.position = {-2.0f, 0.0f, -2.0f}};
    CHECK(near(spatialize(listener, leftFront).pan, -0.70710678f, 1e-5f), "left front");
    Emitter close{.position = {0.5f, 0.0f, 0.0f}, .minDistance = 1.0f};
    CHECK(near(spatialize(listener, close).pan, 0.5f, 1e-6f), "inside minDistance: %f",
          spatialize(listener, close).pan);
    // Turned to face +X, the emitter on +X is in front.
    listener.forward = {1.0f, 0.0f, 0.0f};
    CHECK(near(spatialize(listener, right).pan, 0.0f, 1e-6f), "turned listener");
}

QUENCH_TEST(spatial_doppler) {
    Listener listener;
    Emitter approaching{.position = {0.0f, 0.0f, -100.0f}, .velocity = {0.0f, 0.0f, 34.3f}};
    CHECK(near(spatialize(listener, approaching).pitch, 343.0f / (343.0f - 34.3f), 1e-5f), "approaching: %f",
          spatialize(listener, approaching).pitch);
    Emitter receding{.position = {0.0f, 0.0f, -100.0f}, .velocity = {0.0f, 0.0f, -34.3f}};
    CHECK(near(spatialize(listener, receding).pitch, 343.0f / (343.0f + 34.3f), 1e-5f), "receding");
    Emitter passing{.position = {0.0f, 0.0f, -100.0f}, .velocity = {34.3f, 0.0f, 0.0f}};
    CHECK(near(spatialize(listener, passing).pitch, 1.0f, 1e-6f), "sideways: no shift");
    Listener moving{.velocity = {0.0f, 0.0f, -34.3f}};
    Emitter still{.position = {0.0f, 0.0f, -100.0f}};
    CHECK(near(spatialize(moving, still).pitch, (343.0f + 34.3f) / 343.0f, 1e-5f), "listener toward the emitter");
    CHECK(spatialize(listener, approaching, 343.0f, 0.0f).pitch == 1.0f, "scale 0");
    // 5000 m/s is limited to 0.9 c: factor 10, clamped to 4.
    Emitter fast{.position = {0.0f, 0.0f, -100.0f}, .velocity = {0.0f, 0.0f, 5000.0f}};
    CHECK(near(spatialize(listener, fast).pitch, 4.0f, 1e-5f), "clamped: %f", spatialize(listener, fast).pitch);
}

// ---------------------------------------------------------------------------------------------------- mixer

QUENCH_TEST(mixer_bus_gain) {
    AudioSettings settings;
    settings.buses = {{"music"}, {"effects"}, {.name = "steps", .parent = "effects", .volume = 0.5f}};
    Rig rig(settings);
    AudioSystem& a = rig.audio;
    REQUIRE(a.busCount() == 4 && a.busInfo(a.bus("steps")).parent == a.bus("effects"), "bus tree");
    a.playGenerator(std::make_shared<Constant>(0.25f), {.bus = a.bus("steps")});
    a.setBusVolume(a.bus("effects"), 0.5f);
    a.setBusVolume(BusId{}, 0.8f);
    std::vector<float> out = rig.run(0.3);
    Levels levels = measureLevels(last(out, 0.01), 2);
    CHECK(near(levels.rms[0], 0.25f * 0.5f * 0.5f * 0.8f, 1e-5f) && near(levels.rms[1], 0.05f, 1e-5f), "level %f %f",
          levels.rms[0], levels.rms[1]);
    CHECK(near(a.busInfo(a.bus("steps")).peak[0], 0.125f, 0.01f) && near(a.busInfo(BusId{}).peak[1], 0.05f, 0.01f),
          "meters %f %f", a.busInfo(a.bus("steps")).peak[0], a.busInfo(BusId{}).peak[1]);
    CHECK(a.busInfo(a.bus("music")).peak[0] == 0.0f, "music bus meter %f", a.busInfo(a.bus("music")).peak[0]);

    a.setBusMuted(a.bus("effects"), true);
    out = rig.run(0.2);
    CHECK(measureLevels(last(out, 0.01), 2).peak[0] < 1e-5f, "muted parent: %f",
          measureLevels(last(out, 0.01), 2).peak[0]);
    a.setBusMuted(a.bus("effects"), false);
    a.setBusVolume(a.bus("music"), 0.0f); // another branch: no effect
    out = rig.run(0.2);
    CHECK(near(measureLevels(last(out, 0.01), 2).rms[0], 0.05f, 1e-5f), "unmuted");
    CHECK(a.bus("none") == BusId{} && !a.findBus("none"), "unknown bus");
}

QUENCH_TEST(mixer_click_free_ramps) {
    Rig rig;
    AudioSystem& a = rig.audio;
    const SoundHandle h = a.playGenerator(std::make_shared<Constant>(0.5f));
    std::vector<float> out = rig.run(0.1);
    CHECK(near(out[0], 0.5f, 1e-6f), "starts at once: %f", out[0]);
    constexpr float kClick = 0.01f; // a jump of 2% of full scale between two samples
    struct Change {
        const char* name;
        std::function<void()> apply;
        float left, right;
    };
    const Change changes[] = {
        {"volume", [&] { a.setVolume(h, 0.1f); }, 0.05f, 0.05f},
        {"pan", [&] { a.setPan(h, 1.0f); }, 0.0f, 0.05f},
        {"pan back", [&] { a.setPan(h, 0.0f); }, 0.05f, 0.05f},
        {"volume up", [&] { a.setVolume(h, 1.0f); }, 0.5f, 0.5f},
        {"bus volume", [&] { a.setBusVolume(BusId{}, 0.0f); }, 0.0f, 0.0f},
        {"bus volume back", [&] { a.setBusVolume(BusId{}, 1.0f); }, 0.5f, 0.5f},
        {"pause", [&] { a.setPaused(h, true); }, 0.0f, 0.0f},
        {"resume", [&] { a.setPaused(h, false); }, 0.5f, 0.5f},
    };
    for (const Change& change : changes) {
        change.apply();
        out = rig.run(0.15);
        const Levels end = measureLevels(last(out, 0.005), 2);
        CHECK(maxStep(out) < kClick, "%s: step %f", change.name, maxStep(out));
        CHECK(near(end.rms[0], change.left, 1e-4f) && near(end.rms[1], change.right, 1e-4f), "%s: ends at %f %f",
              change.name, end.rms[0], end.rms[1]);
    }
    a.stop(h); // no fade asked: the shortest one (3 ms)
    out = rig.run(0.02, 0.001);
    CHECK(maxStep(out) < kClick, "stop: step %f", maxStep(out));
    CHECK(measureLevels(std::span(out).subspan(std::size_t(0.004 * kRate) * 2), 2).peak[0] == 0.0f,
          "silent 4 ms after the stop");
    CHECK(a.state(h) == SoundState::Ended, "ended");
}

QUENCH_TEST(mixer_resample_and_pitch) {
    Rig rig;
    AudioSystem& a = rig.audio;
    const SoundHandle h = a.playGenerator(std::make_shared<Sine>(1000.0f, 0.5f, 32000));
    std::vector<float> out = rig.run(0.5);
    Peak peak = dominantFrequency(last(out, 0.4), 2, kRate);
    CHECK(near(peak.frequency, 1000.0f, 1.0f), "32 kHz source at 48 kHz: %f Hz", peak.frequency);
    a.setPitch(h, 1.5f);
    out = rig.run(0.5);
    peak = dominantFrequency(last(out, 0.4), 2, kRate);
    CHECK(near(peak.frequency, 1500.0f, 1.5f), "pitch 1.5: %f Hz", peak.frequency);
    const Levels levels = measureLevels(last(out, 0.4), 2);
    CHECK(near(levels.rms[0], 0.5f * 0.70710678f / std::numbers::sqrt2_v<float>, 0.01f), "level %f", levels.rms[0]);
}

QUENCH_TEST(mixer_doppler_pitch) {
    struct Case {
        const char* name;
        glm::vec3 emitterVelocity, listenerVelocity;
        float expected;
    };
    const Case cases[] = {
        {"approaching", {0.0f, 0.0f, 34.3f}, {0.0f, 0.0f, 0.0f}, 1000.0f * 343.0f / (343.0f - 34.3f)},
        {"receding", {0.0f, 0.0f, -34.3f}, {0.0f, 0.0f, 0.0f}, 1000.0f * 343.0f / (343.0f + 34.3f)},
        {"listener approaching", {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -34.3f}, 1000.0f * (343.0f + 34.3f) / 343.0f},
    };
    for (const Case& c : cases) {
        Rig rig;
        AudioSystem& a = rig.audio;
        a.setListener({.velocity = c.listenerVelocity});
        a.playGenerator(std::make_shared<Sine>(1000.0f), {.spatial = true,
                                                          .emitter = {.position = {0.0f, 0.0f, -50.0f},
                                                                      .velocity = c.emitterVelocity,
                                                                      .attenuation = Attenuation::None}});
        const std::vector<float> out = rig.run(0.6);
        const Peak peak = dominantFrequency(last(out, 0.5), 2, kRate);
        CHECK(near(peak.frequency, c.expected, 1.5f), "%s: %f Hz, expected %f", c.name, peak.frequency, c.expected);
    }
}

QUENCH_TEST(mixer_spatial_pan_and_attenuation) {
    Rig rig;
    AudioSystem& a = rig.audio;
    const Emitter base{.attenuation = Attenuation::Inverse, .minDistance = 1.0f, .rolloff = 1.0f};
    Emitter emitter = base;
    emitter.position = {2.0f, 0.0f, 0.0f};
    const SoundHandle h = a.playGenerator(std::make_shared<Sine>(500.0f, 0.5f), {.spatial = true, .emitter = emitter});
    std::vector<float> out = rig.run(0.3);
    Levels levels = measureLevels(last(out, 0.2), 2);
    const float sineRms = 0.5f / std::numbers::sqrt2_v<float>;
    CHECK(levels.rms[0] < 1e-4f && near(levels.rms[1], sineRms * 0.5f, 2e-3f), "2 m to the right: %f %f", levels.rms[0],
          levels.rms[1]);

    a.setPosition(h, {-2.0f, 0.0f, -2.0f}); // left front, 2.83 m
    out = rig.run(0.3);
    levels = measureLevels(last(out, 0.2), 2);
    const glm::vec2 pan = panGains(-0.70710678f);
    const float gain = 1.0f / (1.0f + std::sqrt(8.0f) - 1.0f);
    CHECK(near(levels.rms[0], sineRms * gain * pan.x, 2e-3f) && near(levels.rms[1], sineRms * gain * pan.y, 2e-3f),
          "left front: %f %f, expected %f %f", levels.rms[0], levels.rms[1], sineRms * gain * pan.x,
          sineRms * gain * pan.y);

    a.setListener({.forward = {-1.0f, 0.0f, 0.0f}}); // now the emitter is to the right front
    out = rig.run(0.3);
    levels = measureLevels(last(out, 0.2), 2);
    CHECK(levels.rms[1] > levels.rms[0] * 2.0f, "turned listener: %f %f", levels.rms[0], levels.rms[1]);
}

QUENCH_TEST(mixer_voice_stealing) {
    AudioSettings settings;
    settings.maxVoices = 4;
    Rig rig(settings);
    AudioSystem& a = rig.audio;
    std::vector<std::pair<SoundHandle, EndReason>> ended;
    a.setEndCallback([&](SoundHandle h, EndReason r) { ended.emplace_back(h, r); });
    // Volumes 0.4, 0.3, 0.2, 0.1: the newest is the quietest.
    std::vector<SoundHandle> low;
    for (int i = 0; i < 4; ++i)
        low.push_back(
            a.playGenerator(std::make_shared<Constant>(0.1f), {.volume = 0.1f * float(4 - i), .priority = 1.0f}));
    rig.run(0.05);
    CHECK(a.stats().voices == 4 && ended.empty(), "four voices: %u", a.stats().voices);

    const SoundHandle high = a.playGenerator(std::make_shared<Constant>(0.01f), {.priority = 2.0f});
    rig.run(0.05);
    REQUIRE(ended.size() == 1, "one sound ended: %zu", ended.size());
    CHECK(ended[0].first == low[3] && ended[0].second == EndReason::Stolen, "the quietest lowest-priority voice: %s",
          endReasonName(ended[0].second));
    CHECK(a.alive(high) && a.stats().voices == 4 && a.stats().stolen == 1, "the new one plays: %u voices",
          a.stats().voices);

    const SoundHandle lowest = a.playGenerator(std::make_shared<Constant>(1.0f), {.priority = 0.0f});
    rig.run(0.05);
    REQUIRE(ended.size() == 2, "two sounds ended: %zu", ended.size());
    CHECK(ended[1].first == lowest && ended[1].second == EndReason::Rejected, "lower priority rejected: %s",
          endReasonName(ended[1].second));
    CHECK(a.stats().rejected == 1, "rejected count");

    // Equal priority: the new sound wins over the quietest (volume 0.2).
    const SoundHandle equal = a.playGenerator(std::make_shared<Constant>(0.5f), {.priority = 1.0f});
    rig.run(0.05);
    REQUIRE(ended.size() == 3, "three sounds ended: %zu", ended.size());
    CHECK(ended[2].first == low[2] && a.alive(equal), "equal priority steals the quietest");
}

QUENCH_TEST(mixer_one_shot_and_loop) {
    auto data = std::make_shared<SoundData>();
    data->format = {2, kRate};
    for (int i = 0; i < 1000; ++i) {
        data->samples.push_back(float(i) / 1000.0f);
        data->samples.push_back(-float(i) / 2000.0f);
    }
    {
        Rig rig;
        std::vector<std::pair<SoundHandle, EndReason>> ended;
        rig.audio.setEndCallback([&](SoundHandle h, EndReason r) { ended.emplace_back(h, r); });
        const SoundHandle h = rig.audio.play(std::shared_ptr<const SoundData>(data));
        CHECK(rig.audio.state(h) == SoundState::Pending, "pending until update()");
        const std::vector<float> out = rig.run(0.05);
        bool exact = true;
        for (std::size_t i = 0; i < data->samples.size(); ++i) exact &= out[i] == data->samples[i];
        CHECK(exact, "one-shot plays the data unchanged");
        CHECK(measureLevels(std::span(out).subspan(data->samples.size()), 2).peak[0] == 0.0f, "then silence");
        rig.audio.update();
        CHECK(ended.size() == 1 && ended[0].second == EndReason::Finished && rig.audio.state(h) == SoundState::Ended,
              "finished");
    }
    {
        Rig rig;
        rig.audio.play(std::shared_ptr<const SoundData>(data), {.loop = true});
        const std::vector<float> out = rig.run(0.1);
        bool exact = true;
        for (std::size_t i = 0; i < out.size(); ++i) exact &= out[i] == data->samples[i % data->samples.size()];
        CHECK(exact, "the loop repeats the data without a gap");
        CHECK(rig.audio.stats().voices == 1, "still playing");
    }
}

QUENCH_TEST(mixer_pause_freezes_position) {
    // A one-second ramp: the value says how far the sound got.
    auto ramp = std::make_shared<SoundData>();
    ramp->format = {2, kRate};
    for (std::uint32_t i = 0; i < kRate; ++i) {
        ramp->samples.push_back(float(i) / kRate);
        ramp->samples.push_back(float(i) / kRate);
    }
    for (bool bus : {false, true}) {
        Rig rig;
        AudioSystem& a = rig.audio;
        const SoundHandle h = a.play(std::shared_ptr<const SoundData>(ramp), {.bus = a.bus("effects")});
        rig.run(0.1);
        if (bus)
            a.setBusPaused(a.bus("effects"), true);
        else
            a.setPaused(h, true);
        std::vector<float> out = rig.run(0.3);
        CHECK(measureLevels(last(out, 0.25), 2).peak[0] == 0.0f, "silent while paused (bus %d)", int(bus));
        if (bus)
            a.setBusPaused(a.bus("effects"), false);
        else
            a.setPaused(h, false);
        out = rig.run(0.02);
        // Paused at about 0.1 s plus the 5 ms fade; 20 ms after the resume it is near 0.125 (0.425 if it ran on).
        const float value = last(out, 0.001)[0];
        CHECK(value > 0.1f && value < 0.15f, "resumed where it paused (bus %d): %f", int(bus), value);
    }
}

QUENCH_TEST(mixer_handles_and_states) {
    Rig rig;
    AudioSystem& a = rig.audio;
    CHECK(!SoundHandle{}.valid() && a.state(SoundHandle{}) == SoundState::Ended, "default handle");
    const SoundHandle h = a.playGenerator(std::make_shared<Constant>(0.1f));
    CHECK(h.valid() && a.state(h) == SoundState::Pending, "pending");
    rig.run(0.01);
    CHECK(a.state(h) == SoundState::Playing && a.stats().sounds == 1, "playing");
    a.stop(h, 0.01f);
    rig.run(0.03);
    CHECK(a.state(h) == SoundState::Ended && a.stats().sounds == 0, "ended");
    // A stale handle after the slot is reused: calls are ignored.
    const SoundHandle next = a.playGenerator(std::make_shared<Constant>(0.2f));
    a.setVolume(h, 0.0f);
    a.stop(h);
    std::vector<float> out = rig.run(0.05);
    CHECK(a.alive(next) && near(last(out, 0.01)[0], 0.2f, 1e-6f), "stale handle had no effect");
    const SoundHandle pending = a.playGenerator(std::make_shared<Constant>(0.3f));
    a.stop(pending);
    CHECK(a.state(pending) == SoundState::Ended, "a pending sound stops at once");
    CHECK(std::string(endReasonName(EndReason::Dropped)) == "dropped", "names");

    AudioSettings few;
    few.maxVoices = 1;
    few.maxSounds = 2;
    Rig small(few);
    CHECK(small.audio.playGenerator(std::make_shared<Constant>(0.1f)).valid() &&
              small.audio.playGenerator(std::make_shared<Constant>(0.1f)).valid() &&
              !small.audio.playGenerator(std::make_shared<Constant>(0.1f)).valid() && small.audio.stats().refused == 1,
          "refused beyond maxSounds");
}

QUENCH_TEST(mixer_offline_carry_and_monitor) {
    Rig rig;
    AudioSystem& a = rig.audio;
    a.setMonitor(0.5);
    a.playGenerator(std::make_shared<Sine>(300.0f));
    std::vector<float> out;
    std::uint32_t frames = 0;
    for (int i = 0; i < 90; ++i) {
        a.update();
        frames += a.mixOffline(1.0 / 90.0, &out);
    }
    CHECK(frames >= kRate - 1 && frames <= kRate && out.size() == std::size_t(frames) * 2, "one second: %u frames",
          frames);
    a.update();
    const std::vector<float> monitored = a.monitored(2.0);
    CHECK(monitored.size() == std::size_t(kRate / 2) * 2, "half a second monitored: %zu", monitored.size());
    const std::span<const float> tail = last(out, 0.5);
    CHECK(monitored.size() == tail.size() && std::equal(monitored.begin(), monitored.end(), tail.begin()),
          "the monitor holds the newest output");
    CHECK(a.stats().framesMixed == frames, "frames mixed");
}

QUENCH_TEST(mixer_pcm_stream) {
    Rig rig;
    AudioSystem& a = rig.audio;
    auto pcm = std::make_shared<PcmStream>(Format{2, kRate}, 0.25);
    const std::vector<float> input = sineSamples(440.0f, 0.3f, 0.1);
    CHECK(pcm->push(input) == input.size() / 2 && pcm->queued() == input.size() / 2, "pushed");
    std::vector<std::pair<SoundHandle, EndReason>> ended;
    a.setEndCallback([&](SoundHandle h, EndReason r) { ended.emplace_back(h, r); });
    const SoundHandle h = a.playGenerator(pcm);
    std::vector<float> out = rig.run(0.1);
    CHECK(std::equal(input.begin(), input.end(), out.begin()), "plays what was pushed");
    out = rig.run(0.05);
    CHECK(pcm->underruns() > 0 && a.alive(h) && measureLevels(out, 2).peak[0] == 0.0f, "empty: silence, an underrun");
    pcm->push(input);
    pcm->finish();
    rig.run(0.15);
    CHECK(ended.size() == 1 && ended[0].second == EndReason::Finished, "finish() ends it after the queued frames");
}

QUENCH_TEST(mixer_thread_stress) {
#if !QUENCH_TEST_THREADS
    QUENCH_SKIP("no threads in this build");
#else
    AssetDir assets("audio_mixer_thread_stress");
    const std::vector<float> music = sineSamples(220.0f, 0.2f, 0.5);
    assets.write("loop.wav", encodeWav(music, 2, kRate));
    JobSystem jobs(JobSystemDesc{.workerThreads = 2});
    AudioSettings settings;
    settings.maxVoices = 8;
    settings.streamBufferSeconds = 0.1;
    settings.buses.push_back({.name = "reverb", .reverb = ReverbDesc{}});
    settings.hrtf = true;
    settings.hrtfVoices = 3;
    AudioSystem audio(jobs, nullptr, settings);
    const BusId reverb = audio.bus("reverb");
    auto data = std::make_shared<const SoundData>(SoundData{{1, 22050}, sineSamples(880.0f, 0.1f, 0.05, 1)});
    std::atomic<bool> stop{false};
    // The "device": renders as fast as it can, like a callback that never sleeps.
    std::thread device([&] {
        std::vector<float> buffer(256 * 2);
        while (!stop.load()) audio.render(buffer.data(), 256);
    });
    const auto start = std::chrono::steady_clock::now();
    std::vector<SoundHandle> handles;
    int i = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(300)) {
        if (i % 5 == 0) handles.push_back(audio.play(data, {.spatial = true, .sendBus = reverb, .sendLevel = 0.5f}));
        if (i % 17 == 0) handles.push_back(audio.playGenerator(std::make_shared<Sine>(500.0f + float(i))));
        if (i % 41 == 0) handles.push_back(audio.playStream("loop.wav", {.loop = true}));
        for (SoundHandle h : handles) audio.setPosition(h, {float(i % 7), 0.0f, -2.0f}, {1.0f, 0.0f, 0.0f});
        for (SoundHandle h : handles) audio.setSend(h, reverb, float(i % 3) / 3.0f);
        if (handles.size() > 12) {
            audio.stop(handles.front(), 0.01f);
            handles.erase(handles.begin());
        }
        audio.setBusVolume(audio.bus("effects"), float(i % 10) / 10.0f);
        if (i % 7 == 0) audio.setReverb(reverb, {.roomSize = float(i % 10) / 10.0f, .damping = 0.5f});
        if (i % 23 == 0) audio.setHrtf(!audio.hrtf());
        audio.update();
        ++i;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    audio.stopAll();
    for (int wait = 0; wait < 500 && audio.stats().sounds > 0; ++wait) {
        audio.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    stop.store(true);
    device.join();
    CHECK(audio.stats().sounds == 0, "every sound ended: %u left", audio.stats().sounds);
    CHECK(audio.stats().played > 10, "played %llu", (unsigned long long)audio.stats().played);
#endif
}

// render() as the device calls it, between the game thread's updates, with sounds, a stream, generators, 3D sound
// (panned and through the HRTF, switched on and off), sends into a reverb and changes of every kind in flight: no
// allocation and no free on its thread.
QUENCH_TEST(mixer_render_allocates_nothing) {
    {
        AllocationCounter counter;
        auto probe = std::make_unique<int>(1);
        probe.reset();
        REQUIRE(counter.count() == 2, "the counter sees new and delete: %llu", (unsigned long long)counter.count());
    }
    AssetDir assets("audio_mixer_render_allocates_nothing");
    assets.write("loop.ogg", quench::test::loopOgg());
    JobSystem jobs(JobSystemDesc{.workerThreads = 0});
    AudioSettings settings;
    settings.streamBufferSeconds = 0.05;
    settings.buses.push_back({.name = "reverb", .reverb = ReverbDesc{}});
    settings.hrtf = true;
    settings.hrtfVoices = 3;
    AudioSystem audio(jobs, nullptr, settings);
    const BusId reverb = audio.bus("reverb");
    auto data = std::make_shared<const SoundData>(SoundData{{1, 22050}, sineSamples(880.0f, 0.3f, 0.05, 1)});
    std::uint32_t hrtfMixed = 0;
    std::vector<SoundHandle> handles;
    handles.push_back(audio.playStream("loop.ogg", {.loop = true, .sendBus = reverb, .sendLevel = 0.3f}));
    std::vector<float> buffer(256 * 2);
    std::uint64_t allocations = 0, mixed = 0;
    for (int i = 0; i < 600; ++i) {
        if (i % 9 == 0) handles.push_back(audio.play(data, {.pitch = 1.3f, .spatial = true, .sendBus = reverb}));
        if (i % 50 == 0) handles.push_back(audio.playGenerator(std::make_shared<Sine>(300.0f), {.fadeIn = 0.05f}));
        for (SoundHandle h : handles) {
            audio.setPosition(h, {float(i % 7) - 3.0f, 0.0f, -2.0f}, {2.0f, 0.0f, 0.0f});
            audio.setSend(h, reverb, float(i % 4) / 4.0f);
        }
        if (handles.size() > 10) {
            audio.stop(handles[1], 0.01f);
            handles.erase(handles.begin() + 1);
        }
        if (i % 25 == 0) audio.setReverb(reverb, {.roomSize = float(i % 100) / 100.0f});
        if (i % 30 == 0) audio.setBusPaused(audio.bus("effects"), i % 60 == 0);
        if (i % 40 == 0) audio.setHrtf(i % 80 == 0);
        audio.setBusVolume(BusId{}, 0.5f + float(i % 5) / 10.0f);
        audio.update();
        jobs.waitAll();
        AllocationCounter counter;
        audio.render(buffer.data(), 256);
        allocations += counter.count();
        mixed += audio.stats().voices;
        hrtfMixed += audio.stats().hrtfVoices;
    }
    CHECK(allocations == 0, "render() allocated or freed %llu times", (unsigned long long)allocations);
    CHECK(mixed > 600 * 3, "voices mixed per render: %.1f", double(mixed) / 600.0);
    CHECK(hrtfMixed > 300, "HRTF voices per render: %.1f", double(hrtfMixed) / 600.0);
}

// Opt-in (QUENCH_TEST_AUDIO_COST=1): the time render() takes on this machine for 32 looping 3D voices, without and
// with effects, called as the device calls it (512 frames at 48 kHz). Prints the numbers; checks none of them.
QUENCH_TEST(mix_cost) {
    if (!std::getenv("QUENCH_TEST_AUDIO_COST")) QUENCH_SKIP("set QUENCH_TEST_AUDIO_COST=1 to time the mix");
    struct Case {
        const char* name;
        bool reverbBus = false;
        float send = 0.0f;
        std::uint32_t hrtfVoices = 0; // with the HRTF on
    };
    const Case cases[] = {{"32 voices"},
                          {"32 voices, a reverb bus, no sends", true},
                          {"32 voices, sends 0.3 into a reverb", true, 0.3f},
                          {"32 voices, 8 of them through the HRTF", false, 0.0f, 8},
                          {"32 voices, all through the HRTF", false, 0.0f, 32}};
    auto data = std::make_shared<const SoundData>(SoundData{{1, kRate}, sineSamples(440.0f, 0.02f, 1.0, 1)});
    constexpr std::uint32_t kFrames = 512;
    constexpr int kCalls = int(20 * kRate / kFrames);
    for (const Case& c : cases) {
        JobSystem jobs(JobSystemDesc{.workerThreads = 0});
        AudioSettings settings;
        if (c.reverbBus) settings.buses.push_back({.name = "reverb", .reverb = ReverbDesc{}});
        settings.hrtf = c.hrtfVoices > 0;
        settings.hrtfVoices = c.hrtfVoices;
        AudioSystem audio(jobs, nullptr, settings);
        const BusId reverb = c.reverbBus ? audio.bus("reverb") : BusId{};
        for (int i = 0; i < 32; ++i) {
            const float angle = float(i) * 0.2f;
            audio.play(data, {.pitch = 0.8f + 0.01f * float(i),
                              .loop = true,
                              .spatial = true,
                              .emitter = {.position = {5.0f * std::cos(angle), 0.0f, 5.0f * std::sin(angle)}},
                              .sendBus = reverb,
                              .sendLevel = c.send});
        }
        std::vector<float> buffer(kFrames * 2);
        double seconds = 0.0;
        for (int i = 0; i < kCalls; ++i) {
            if (i % 2 == 0) audio.update();
            const auto start = std::chrono::steady_clock::now();
            audio.render(buffer.data(), kFrames);
            seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }
        const double audioSeconds = double(kCalls) * kFrames / kRate;
        std::printf("mix cost, %s: %.2f us per ms of audio (%.3f%% of one core)\n", c.name,
                    seconds * 1e3 / audioSeconds, 100.0 * seconds / audioSeconds);
        CHECK(audio.stats().voices == 32 && audio.stats().hrtfVoices == c.hrtfVoices, "%s: %u voices mixed, %u HRTF",
              c.name, audio.stats().voices, audio.stats().hrtfVoices);
    }
}

// ---------------------------------------------------------------------------------------------------- reverb

// A reverb bus adds nothing when nothing reaches it: the mix is bit for bit the same as without the bus, with sends
// at 0, and with sends into a reverb bus at volume 0.
QUENCH_TEST(reverb_wet_zero_is_dry) {
    auto mix = [](bool reverbBus, float sendLevel, float reverbVolume) {
        AudioSettings settings;
        if (reverbBus) settings.buses.push_back({.name = "reverb", .volume = reverbVolume, .reverb = ReverbDesc{}});
        Rig rig(settings);
        AudioSystem& a = rig.audio;
        const BusId send = reverbBus ? a.bus("reverb") : BusId{};
        a.playGenerator(std::make_shared<Sine>(440.0f, 0.3f),
                        {.bus = a.bus("effects"), .sendBus = send, .sendLevel = sendLevel});
        a.playGenerator(
            std::make_shared<Sine>(660.0f, 0.2f),
            {.spatial = true, .emitter = {.position = {2.0f, 0.0f, -1.0f}}, .sendBus = send, .sendLevel = sendLevel});
        auto data = std::make_shared<const SoundData>(SoundData{{2, 44100}, sineSamples(220.0f, 0.2f, 0.1)});
        a.play(data, {.pitch = 0.9f, .loop = true, .sendBus = send, .sendLevel = sendLevel});
        return rig.run(1.0);
    };
    const std::vector<float> dry = mix(false, 0.0f, 1.0f);
    const auto same = [&](const std::vector<float>& other) {
        return other.size() == dry.size() && std::memcmp(other.data(), dry.data(), dry.size() * sizeof(float)) == 0;
    };
    CHECK(same(mix(true, 0.0f, 1.0f)), "a reverb bus without sends changed the mix");
    CHECK(same(mix(true, 0.7f, 0.0f)), "sends into a reverb bus at volume 0 changed the mix");
    const std::vector<float> wet = mix(true, 0.7f, 1.0f);
    std::vector<float> difference(dry.size());
    for (std::size_t i = 0; i < dry.size(); ++i) difference[i] = wet[i] - dry[i];
    const float reverbRms = measureLevels(difference, 2).rmsMono();
    std::printf("reverb: %.4f RMS of reverb on %.4f RMS of dry signal (sends 0.7)\n", reverbRms,
                measureLevels(dry, 2).rmsMono());
    CHECK(reverbRms > 0.01f, "the reverb is audible with sends: %f RMS", reverbRms);
}

// An impulse on a reverb bus: the reverberation time (T30) grows with the room size, and the tail ends in exact
// zeros (after which the reverb is skipped).
QUENCH_TEST(reverb_decay_grows_with_room_size) {
    double previous = 0.0;
    for (float room : {0.2f, 0.5f, 0.8f}) {
        AudioSettings settings;
        settings.buses.push_back({.name = "reverb", .reverb = ReverbDesc{.roomSize = room}});
        Rig rig(settings);
        rig.audio.playGenerator(std::make_shared<Impulse>(), {.bus = rig.audio.bus("reverb")});
        const std::vector<float> out = rig.run(10.0, 0.1);
        const double rt60 = reverberationTime(out);
        std::size_t lastSound = 0;
        for (std::size_t i = 0; i < out.size(); ++i)
            if (out[i] != 0.0f) lastSound = i / 2;
        const std::size_t cutFrom = lastSound > kRate / 10 ? lastSound - kRate / 10 : 0;
        const auto peakOf = [](std::span<const float> samples) {
            const Levels levels = measureLevels(samples, 2);
            return std::max(levels.peak[0], levels.peak[1]);
        };
        const float beforeCut = peakOf(std::span(out).subspan(cutFrom * 2, (lastSound + 1 - cutFrom) * 2));
        std::printf("reverb room size %.1f: RT60 %.3f s, peak %.4f, zeros from %.2f s (peak %.2g in the 100 ms "
                    "before)\n",
                    room, rt60, peakOf(out), double(lastSound + 1) / kRate, beforeCut);
        CHECK(rt60 > previous * 1.2, "room size %.1f: RT60 %.3f s after %.3f s", room, rt60, previous);
        CHECK(lastSound + kRate / 2 < out.size() / 2, "room size %.1f: the tail lasts to %.2f s", room,
              double(lastSound) / kRate);
        CHECK(beforeCut < 1e-4f, "room size %.1f: %g before the reverb goes idle", room, beforeCut);
        previous = rt60;
    }
    CHECK(previous > 1.0, "a large room rings for more than a second: %.3f s", previous);
}

// ---------------------------------------------------------------------------------------------------- HRTF

namespace {

// The lag (in frames) at which the right channel best matches the left one, within +-maxLag: positive when the right
// ear hears the sound later.
int interauralLag(std::span<const float> stereo, int maxLag) {
    const auto frames = int(stereo.size() / 2);
    int best = 0;
    double bestValue = -1e300;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        double sum = 0.0;
        for (int f = std::max(0, -lag); f < frames - std::max(0, lag); ++f)
            sum += double(stereo[2 * f]) * stereo[2 * (f + lag) + 1];
        if (sum > bestValue) {
            bestValue = sum;
            best = lag;
        }
    }
    return best;
}

} // namespace

// A mono source to the left, to the right and in front of the listener (2 m away, at ear height): the near ear
// louder and earlier by the head's delay, the two sides mirrored, the front even; at 48 kHz (the table's rate) and at
// 44.1 kHz (the table resampled).
QUENCH_TEST(hrtf_left_right) {
    for (std::uint32_t rate : {48000u, 44100u}) {
        struct Heard {
            float levelDb; // left over right
            int lag;       // right after left, in frames
        };
        const auto hear = [rate](glm::vec3 position) {
            AudioSettings settings;
            settings.sampleRate = rate;
            settings.hrtf = true;
            Rig rig(settings);
            rig.audio.playGenerator(std::make_shared<Noise>(0.3f, rate),
                                    {.spatial = true, .emitter = {.position = position}});
            const std::vector<float> out = rig.run(0.5);
            const auto steady = std::span(out).subspan(out.size() / 2);
            const Levels levels = measureLevels(steady, 2);
            CHECK(rig.audio.stats().hrtfVoices == 1, "%u Hz: %u HRTF voices", rate, rig.audio.stats().hrtfVoices);
            return Heard{20.0f * std::log10(levels.rms[0] / levels.rms[1]), interauralLag(steady, 60)};
        };
        const Heard left = hear({-2.0f, 0.0f, 0.0f});
        const Heard right = hear({2.0f, 0.0f, 0.0f});
        const Heard front = hear({0.0f, 0.0f, -2.0f});
        // The same lags of SADIE's own responses (256 taps at 48 kHz) at azimuth 90 and 270: 39 and -38 frames.
        const float scale = float(rate) / 48000.0f;
        const float expected[2] = {39.0f * scale, -38.0f * scale};
        std::printf("HRTF at %u Hz: left %+.1f dB, right ear %d frames later; right %+.1f dB, %d; front %+.1f dB, %d\n",
                    rate, left.levelDb, left.lag, right.levelDb, right.lag, front.levelDb, front.lag);
        CHECK(left.levelDb > 10.0f && right.levelDb < -10.0f, "%u Hz: levels %+.1f and %+.1f dB", rate, left.levelDb,
              right.levelDb);
        CHECK(std::abs(float(left.lag) - expected[0]) <= 2.0f && std::abs(float(right.lag) - expected[1]) <= 2.0f,
              "%u Hz: lags %d and %d frames, %.1f and %.1f expected", rate, left.lag, right.lag, expected[0],
              expected[1]);
        CHECK(std::abs(front.levelDb) < 1.5f && std::abs(front.lag) <= 2, "%u Hz: front %+.1f dB, lag %d", rate,
              front.levelDb, front.lag);
    }
}

// HRTF off (or without HRTF voices) mixes bit for bit like the mixer without an HRTF; with it on, sounds that are not
// spatial are not filtered either.
QUENCH_TEST(hrtf_off_is_panned) {
    const auto mix = [](bool hrtf, std::uint32_t hrtfVoices, bool spatialSounds) {
        AudioSettings settings;
        settings.hrtf = hrtf;
        settings.hrtfVoices = hrtfVoices;
        Rig rig(settings);
        AudioSystem& a = rig.audio;
        a.playGenerator(std::make_shared<Sine>(440.0f, 0.3f), {.bus = a.bus("effects"), .pan = -0.4f});
        auto data = std::make_shared<const SoundData>(SoundData{{2, 44100}, sineSamples(220.0f, 0.2f, 0.1)});
        a.play(data, {.pitch = 0.9f, .loop = true});
        if (spatialSounds) {
            a.playGenerator(std::make_shared<Sine>(660.0f, 0.2f),
                            {.spatial = true, .emitter = {.position = {2.0f, 0.0f, -1.0f}}});
            a.play(data, {.loop = true, .spatial = true, .emitter = {.position = {-1.0f, 1.0f, 3.0f}}});
        }
        return rig.run(0.5);
    };
    const auto same = [](const std::vector<float>& a, const std::vector<float>& b) {
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    };
    const std::vector<float> panned = mix(false, 0, true);
    CHECK(same(mix(false, 8, true), panned), "HRTF voices with the HRTF off changed the mix");
    CHECK(same(mix(true, 0, true), panned), "the HRTF on without HRTF voices changed the mix");
    CHECK(!same(mix(true, 8, true), panned), "the HRTF on did not change a mix of spatial sounds");
    CHECK(same(mix(true, 8, false), mix(false, 0, false)), "the HRTF on changed a mix without spatial sounds");
}

// At most hrtfVoices spatial sounds are filtered at once (the others are panned); switching off gives every filter
// back, switching on takes them again.
QUENCH_TEST(hrtf_voice_cap) {
    AudioSettings settings;
    settings.hrtf = true;
    settings.hrtfVoices = 2;
    Rig rig(settings);
    AudioSystem& a = rig.audio;
    for (int i = 0; i < 5; ++i)
        a.playGenerator(std::make_shared<Sine>(300.0f + 100.0f * float(i), 0.1f),
                        {.spatial = true, .emitter = {.position = {float(i) - 2.0f, 0.0f, -2.0f}}});
    std::vector<float> out = rig.run(0.1);
    CHECK(a.stats().voices == 5 && a.stats().hrtfVoices == 2, "%u voices, %u with the HRTF", a.stats().voices,
          a.stats().hrtfVoices);
    a.setHrtf(false);
    out = rig.run(0.1);
    CHECK(!a.hrtf() && a.stats().hrtfVoices == 0, "switched off: %u HRTF voices", a.stats().hrtfVoices);
    a.setHrtf(true);
    out = rig.run(0.1);
    CHECK(a.hrtf() && a.stats().hrtfVoices == 2, "switched on again: %u HRTF voices", a.stats().hrtfVoices);
    CHECK(measureLevels(out, 2).rmsMono() > 0.02f, "the sounds play: %f RMS", measureLevels(out, 2).rmsMono());

    AudioSettings none;
    none.hrtfVoices = 0;
    Rig without(none);
    without.audio.setHrtf(true);
    CHECK(!without.audio.hrtf(), "setHrtf(true) without HRTF voices stays off");
}

// A 1 kHz tone circling the head once a second through the HRTF's directions, and the HRTF switched on and off under
// it: no step between frames larger than the tone's own (a click would be one).
QUENCH_TEST(hrtf_moving_and_switching_is_smooth) {
    constexpr float kHz = 1000.0f;
    AudioSettings settings;
    settings.hrtf = true;
    Rig rig(settings);
    AudioSystem& a = rig.audio;
    const SoundHandle h = a.playGenerator(std::make_shared<Sine>(kHz, 0.3f),
                                          {.spatial = true, .emitter = {.position = {0.0f, 0.0f, -2.0f}}});
    std::vector<float> out;
    constexpr double kStep = 0.005;
    for (int i = 0; i < 600; ++i) {
        const double t = double(i) * kStep;
        const auto angle = float(2.0 * std::numbers::pi * t);
        a.setPosition(h, {2.0f * std::sin(angle), 0.8f * std::sin(0.5f * angle), -2.0f * std::cos(angle)});
        if (i % 100 == 50) a.setHrtf(!a.hrtf());
        a.update();
        a.mixOffline(kStep, &out);
    }
    const auto steady = std::span<const float>(out).subspan(std::size_t(0.05 * kRate) * 2);
    const Levels levels = measureLevels(steady, 2);
    const float tone = 2.0f * std::numbers::pi_v<float> * kHz / float(kRate); // largest step of a unit sine
    for (std::uint32_t c = 0; c < 2; ++c) {
        const std::vector<float> channel = channelOf(steady, c);
        float step = 0.0f;
        for (std::size_t i = 1; i < channel.size(); ++i) step = std::max(step, std::abs(channel[i] - channel[i - 1]));
        std::printf("HRTF moving and switched: channel %u peak %.4f, largest step %.4f (a sine of that peak: %.4f)\n",
                    c, levels.peak[c], step, levels.peak[c] * tone);
        CHECK(step <= 1.1f * levels.peak[c] * tone, "channel %u: a step of %f at a peak of %f", c, step,
              levels.peak[c]);
    }
}

// ---------------------------------------------------------------------------------------------------- streams

QUENCH_TEST(stream_chunk_boundaries) {
    AssetDir assets("audio_stream_chunk_boundaries");
    const std::vector<float> pattern = patternSamples(kRate * 2 + 777);
    const fs::Bytes wav = encodeWav(pattern, 2, kRate);
    assets.write("music/pattern.wav", wav);
    const Result<SoundData> expected = decodeSound(wav);
    REQUIRE(expected, "decode: %s", expected.error().c_str());

    AudioSettings settings;
    settings.streamBufferSeconds = 0.05; // the smallest ring (4096 frames): many refills
    Rig rig(settings);
    std::vector<std::pair<SoundHandle, EndReason>> ended;
    rig.audio.setEndCallback([&](SoundHandle h, EndReason r) { ended.emplace_back(h, r); });
    rig.audio.playStream("music/pattern.wav");
    const std::vector<float> out = rig.run(2.2, 0.0123);
    REQUIRE(out.size() >= expected->samples.size(), "mixed %zu", out.size());
    std::size_t mismatch = expected->samples.size();
    for (std::size_t i = 0; i < expected->samples.size() && mismatch == expected->samples.size(); ++i)
        if (out[i] != expected->samples[i]) mismatch = i;
    CHECK(mismatch == expected->samples.size(), "streamed output differs at sample %zu", mismatch);
    CHECK(rig.audio.stats().underruns == 0, "underruns %llu", (unsigned long long)rig.audio.stats().underruns);
    rig.audio.update();
    CHECK(ended.size() == 1 && ended[0].second == EndReason::Finished, "the stream finished");
}

QUENCH_TEST(stream_refills_on_jobs) {
    AssetDir assets("audio_stream_refills_on_jobs");
    const std::vector<float> pattern = patternSamples(kRate + 1234);
    const fs::Bytes wav = encodeWav(pattern, 2, kRate);
    assets.write("pattern.wav", wav);
    const Result<SoundData> expected = decodeSound(wav);
    REQUIRE(expected, "decode");

    JobSystem jobs(JobSystemDesc{.workerThreads = QUENCH_TEST_THREADS ? 2 : 0});
    AudioSettings settings;
    settings.streamBufferSeconds = 0.05;
    AudioSystem audio(jobs, nullptr, settings);
    const SoundHandle h = audio.playStream("pattern.wav");
    std::vector<float> out, buffer(480 * 2);
    for (int step = 0; step < 300 && out.size() < expected->samples.size(); ++step) {
        audio.update(); // opens the stream and refills it on jobs
        jobs.waitAll(); // the "device" renders only after they are done
        if (audio.state(h) != SoundState::Playing) continue;
        audio.update(); // sends the play
        audio.render(buffer.data(), 480);
        out.insert(out.end(), buffer.begin(), buffer.end());
    }
    REQUIRE(out.size() >= expected->samples.size(), "mixed %zu of %zu", out.size(), expected->samples.size());
    CHECK(std::equal(expected->samples.begin(), expected->samples.end(), out.begin()), "streamed output matches");
    CHECK(audio.stats().underruns == 0, "underruns");
}

QUENCH_TEST(stream_loop_is_gapless) {
    AssetDir assets("audio_stream_loop_is_gapless");
    const std::vector<float> pattern = patternSamples(12000);
    const fs::Bytes wav = encodeWav(pattern, 2, kRate);
    assets.write("loop.wav", wav);
    const Result<SoundData> expected = decodeSound(wav);
    REQUIRE(expected, "decode");
    AudioSettings settings;
    settings.streamBufferSeconds = 0.05;
    Rig rig(settings);
    rig.audio.playStream("loop.wav", {.loop = true});
    const std::vector<float> out = rig.run(1.0);
    std::size_t mismatch = out.size();
    for (std::size_t i = 0; i < out.size() && mismatch == out.size(); ++i)
        if (out[i] != expected->samples[i % expected->samples.size()]) mismatch = i;
    CHECK(mismatch == out.size(), "the looped stream differs at sample %zu", mismatch);
}

QUENCH_TEST(stream_music_crossfade) {
    AssetDir assets("audio_stream_music_crossfade");
    assets.write("a.wav", encodeWav(sineSamples(440.0f, 0.4f, 1.0), 2, kRate));
    assets.write("b.wav", encodeWav(sineSamples(660.0f, 0.4f, 1.0), 2, kRate));
    Rig rig;
    AudioSystem& a = rig.audio;
    std::vector<std::pair<SoundHandle, EndReason>> ended;
    a.setEndCallback([&](SoundHandle h, EndReason r) { ended.emplace_back(h, r); });
    const SoundHandle first = a.playMusic("a.wav", {.crossfade = 0.0f});
    std::vector<float> out = rig.run(0.5);
    CHECK(near(dominantFrequency(last(out, 0.3), 2, kRate).frequency, 440.0f, 1.0f), "first track");
    const float before = measureLevels(last(out, 0.3), 2).rms[0];

    const SoundHandle second = a.playMusic("b.wav", {.crossfade = 0.5f});
    out = rig.run(0.5);
    // Equal-power crossfade of two unrelated tones: the level stays about the same throughout.
    float lowest = 10.0f, highest = 0.0f;
    for (double t = 0.0; t + 0.05 <= 0.5; t += 0.05) {
        const float rms =
            measureLevels(std::span(out).subspan(std::size_t(t * kRate) * 2, std::size_t(0.05 * kRate) * 2), 2).rms[0];
        lowest = std::min(lowest, rms);
        highest = std::max(highest, rms);
    }
    CHECK(lowest > before * 0.85f && highest < before * 1.15f, "level during the crossfade %f..%f (before %f)", lowest,
          highest, before);
    out = rig.run(0.3);
    const Peak peak = dominantFrequency(last(out, 0.25), 2, kRate, 300.0f, 1000.0f);
    CHECK(near(peak.frequency, 660.0f, 1.0f), "second track: %f Hz", peak.frequency);
    CHECK(near(dominantFrequency(last(out, 0.25), 2, kRate, 400.0f, 500.0f).level, 0.0f, 0.01f), "first track gone");
    CHECK(ended.size() == 1 && ended[0].first == first && ended[0].second == EndReason::Stopped, "first stopped");
    CHECK(a.music() == second && a.alive(second), "the second is the music");
    a.stopMusic(0.1f);
    rig.run(0.2);
    CHECK(!a.alive(second) && !a.music().valid(), "music stopped");
}

// A streamed Vorbis file is decoded in miniaudio's push mode, which reports no length: the stream gives the frames of
// the file decoded in memory (pull mode) and starts over where the decoder runs out, without a gap or a click.
QUENCH_TEST(stream_vorbis_loop) {
    AssetDir assets("audio_stream_vorbis_loop");
    assets.write("loop.ogg", quench::test::loopOgg());
    const Result<SoundData> expected = decodeSound(quench::test::loopOgg());
    REQUIRE(expected && expected->format.channels == 2, "decode");
    const std::vector<float>& once = expected->samples;

    AudioSettings settings;
    settings.streamBufferSeconds = 0.05; // the smallest ring: many refills, several of them across the loop point
    Rig rig(settings);
    const SoundHandle h = rig.audio.playStream("loop.ogg", {.loop = true});
    const std::vector<float> out = rig.run(3.5);
    REQUIRE(out.size() == std::size_t(3.5 * kRate) * 2, "mixed %zu", out.size());
    CHECK(rig.audio.alive(h), "still looping");
    float worst = 0.0f;
    for (std::size_t i = 0; i < out.size(); ++i) worst = std::max(worst, std::abs(out[i] - once[i % once.size()]));
    CHECK(worst == 0.0f, "the loop differs from the decoded file by up to %g", worst);

    // By measurement: the largest step between neighbouring frames within 5 ms of the loop points against the
    // largest elsewhere (a click), and the level of 25 ms windows (whole periods of both tones) every 12.5 ms, on and
    // across the loop points, against the whole (a gap).
    const std::size_t period = expected->frames(), near5ms = kRate / 200, window = kRate / 40;
    float stepAtLoops = 0.0f, stepElsewhere = 0.0f;
    for (std::size_t f = 1; f < out.size() / 2; ++f) {
        const std::size_t fromLoop = std::min(f % period, period - f % period);
        float& step = fromLoop <= near5ms ? stepAtLoops : stepElsewhere;
        for (std::size_t c = 0; c < 2; ++c) step = std::max(step, std::abs(out[f * 2 + c] - out[(f - 1) * 2 + c]));
    }
    CHECK(stepAtLoops <= stepElsewhere * 1.05f, "a step of %f at the loop points, %f elsewhere", stepAtLoops,
          stepElsewhere);
    // The lowest and highest window level relative to the whole.
    auto levelRange = [&](std::span<const float> samples) {
        const float whole = measureLevels(samples, 2).rmsMono();
        float lowest = 10.0f, highest = 0.0f;
        for (std::size_t start = 0; start + window <= samples.size() / 2; start += window / 2) {
            const float rms = measureLevels(samples.subspan(start * 2, window * 2), 2).rmsMono();
            lowest = std::min(lowest, rms / whole);
            highest = std::max(highest, rms / whole);
        }
        return std::pair(lowest, highest);
    };
    const auto [lowest, highest] = levelRange(out);
    CHECK(lowest >= 0.98f && highest <= 1.02f, "25 ms levels %f..%f of the whole", lowest, highest);
    // The measurement sees a gap: 2.5 ms of silence at the first loop point.
    std::vector<float> gapped = out;
    gapped.insert(gapped.begin() + std::ptrdiff_t(period * 2), kRate / 400 * 2, 0.0f);
    const float gappedLowest = levelRange(gapped).first;
    CHECK(gappedLowest < 0.98f, "a gap of 2.5 ms measured %f of the whole", gappedLowest);
    std::printf("vorbis loop: largest step %.4f at the loop points, %.4f elsewhere; 25 ms levels %.4f..%.4f of the "
                "whole (%.4f with a 2.5 ms gap)\n",
                stepAtLoops, stepElsewhere, lowest, highest, gappedLowest);
    const std::vector<float> left = channelOf(last(out, 1.0), 0), right = channelOf(last(out, 1.0), 1);
    CHECK(near(dominantFrequency(left, 1, kRate).frequency, kLoopOggHz[0], 1.0f) &&
              near(dominantFrequency(right, 1, kRate).frequency, kLoopOggHz[1], 1.0f),
          "440 Hz left, 880 Hz right");
    CHECK(rig.audio.stats().underruns == 0, "underruns %llu", (unsigned long long)rig.audio.stats().underruns);
}

// Vorbis music loops past its end and crossfades into the next track like any other file.
QUENCH_TEST(stream_vorbis_music_crossfade) {
    AssetDir assets("audio_stream_vorbis_music_crossfade");
    assets.write("loop.ogg", quench::test::loopOgg());
    assets.write("next.wav", encodeWav(sineSamples(660.0f, 0.5f, 1.0), 2, kRate));
    Rig rig;
    AudioSystem& a = rig.audio;
    std::vector<std::pair<SoundHandle, EndReason>> ended;
    a.setEndCallback([&](SoundHandle h, EndReason r) { ended.emplace_back(h, r); });
    const SoundHandle first = a.playMusic("loop.ogg", {.crossfade = 0.0f});
    std::vector<float> out = rig.run(1.6); // past the end of the 1 s file
    CHECK(a.alive(first) && ended.empty(), "the Vorbis music loops");
    const std::vector<float> left = channelOf(last(out, 0.5), 0);
    CHECK(near(dominantFrequency(left, 1, kRate).frequency, kLoopOggHz[0], 1.0f), "first track after its loop point");
    const float before = measureLevels(last(out, 0.5), 2).rms[0];

    const SoundHandle second = a.playMusic("next.wav", {.crossfade = 0.5f});
    out = rig.run(0.5);
    float lowest = 10.0f, highest = 0.0f;
    for (double t = 0.0; t + 0.05 <= 0.5; t += 0.05) {
        const float rms =
            measureLevels(std::span(out).subspan(std::size_t(t * kRate) * 2, std::size_t(0.05 * kRate) * 2), 2).rms[0];
        lowest = std::min(lowest, rms);
        highest = std::max(highest, rms);
    }
    CHECK(lowest > before * 0.85f && highest < before * 1.15f, "level during the crossfade %f..%f (before %f)", lowest,
          highest, before);
    out = rig.run(0.3);
    const std::vector<float> after = channelOf(last(out, 0.25), 0);
    CHECK(near(dominantFrequency(after, 1, kRate, 300.0f, 1000.0f).frequency, 660.0f, 1.0f), "second track");
    CHECK(near(dominantFrequency(after, 1, kRate, 400.0f, 500.0f).level, 0.0f, 0.01f), "first track gone");
    CHECK(ended.size() == 1 && ended[0].first == first && ended[0].second == EndReason::Stopped, "first stopped");
    CHECK(a.music() == second, "the second is the music");
}

QUENCH_TEST(stream_missing_file) {
    AssetDir assets("audio_stream_missing_file");
    assets.write("broken.mp3", std::vector<std::uint8_t>(5000, 0x42));
    assets.write("truncated.ogg", quench::test::loopOgg().first(60));
    for (bool offline : {true, false}) {
        JobSystem jobs(JobSystemDesc{.workerThreads = 0});
        AudioSystem audio(jobs, nullptr);
        audio.setOffline(offline);
        std::vector<EndReason> reasons;
        audio.setEndCallback([&](SoundHandle, EndReason r) { reasons.push_back(r); });
        audio.playStream("missing.ogg");
        audio.playStream("broken.mp3");
        audio.playStream("truncated.ogg");
        for (int i = 0; i < 10 && reasons.size() < 3; ++i) {
            audio.update();
            jobs.waitAll();
        }
        CHECK(reasons.size() == 3 && std::ranges::all_of(reasons, [](EndReason r) { return r == EndReason::Failed; }),
              "all fail (offline %d): %zu", int(offline), reasons.size());
    }
}

// ---------------------------------------------------------------------------------------------------- files

QUENCH_TEST(decode_wav_flac_mp3) {
    const std::vector<float> samples = sineSamples(440.0f, 0.5f, 0.5);
    for (WavSampleFormat format : {WavSampleFormat::Pcm16, WavSampleFormat::Float32}) {
        const Result<SoundData> wav = decodeSound(encodeWav(samples, 2, kRate, format));
        REQUIRE(wav, "wav: %s", wav.error().c_str());
        CHECK(wav->format.channels == 2 && wav->format.sampleRate == kRate && wav->frames() == samples.size() / 2,
              "wav format");
        float worst = 0.0f;
        for (std::size_t i = 0; i < samples.size(); ++i)
            worst = std::max(worst, std::abs(wav->samples[i] - samples[i]));
        CHECK(worst <= (format == WavSampleFormat::Pcm16 ? 1.0f / 16384.0f : 0.0f), "wav round trip error %g", worst);
    }
    struct File {
        const char* name;
        std::span<const std::uint8_t> bytes;
    };
    for (const File& file : {File{"flac", quench::test::toneFlac()}, File{"mp3", quench::test::toneMp3()}}) {
        const Result<SoundData> sound = decodeSound(file.bytes);
        REQUIRE(sound, "%s: %s", file.name, sound.error().c_str());
        CHECK(sound->format.channels == 1 && sound->format.sampleRate == 22050, "%s format %u ch %u Hz", file.name,
              sound->format.channels, sound->format.sampleRate);
        // 11025 frames encoded; the MP3 decodes with its encoder delay and padding (22 frames of 576 = 12672).
        CHECK(sound->frames() >= 11025 && sound->frames() <= 11025 + 3 * 576, "%s: %llu frames", file.name,
              (unsigned long long)sound->frames());
        const Peak peak = dominantFrequency(sound->samples, 1, 22050);
        CHECK(near(peak.frequency, 440.0f, 1.0f), "%s: %f Hz", file.name, peak.frequency);
        CHECK(near(measureLevels(sound->samples, 1).peak[0], 0.5f, 0.05f), "%s peak", file.name);
    }
}

// The Ogg Vorbis loop against the signal it was encoded from: its exact length (the last page's granule position),
// and the error of each channel against the sine.
QUENCH_TEST(decode_vorbis) {
    const Result<SoundData> sound = decodeSound(quench::test::loopOgg());
    REQUIRE(sound, "ogg: %s", sound.error().c_str());
    CHECK(sound->format.channels == 2 && sound->format.sampleRate == kRate, "format %u ch %u Hz",
          sound->format.channels, sound->format.sampleRate);
    CHECK(sound->frames() == kRate, "%llu frames", (unsigned long long)sound->frames());
    for (std::uint32_t c = 0; c < 2; ++c) {
        double signal = 0.0, error = 0.0;
        for (std::uint64_t f = 0; f < sound->frames(); ++f) {
            const double expected = 0.5 * std::sin(2.0 * std::numbers::pi * kLoopOggHz[c] * double(f) / kRate);
            signal += expected * expected;
            error += (sound->samples[f * 2 + c] - expected) * (sound->samples[f * 2 + c] - expected);
        }
        const double snr = 10.0 * std::log10(signal / std::max(error, 1e-30));
        std::printf("vorbis channel %u: %.2f dB signal to error\n", c, snr);
        CHECK(snr >= 30.0, "channel %u: %.2f dB signal to error", c, snr);
        const Peak peak = dominantFrequency(channelOf(sound->samples, c), 1, kRate);
        CHECK(near(peak.frequency, kLoopOggHz[c], 0.5f), "channel %u: %f Hz", c, peak.frequency);
    }
}

QUENCH_TEST(decode_errors) {
    std::vector<std::uint8_t> text;
    for (int i = 0; i < 200; ++i)
        for (char c : std::string("not a sound file ")) text.push_back(std::uint8_t(c));
    CHECK(!decodeSound(text), "text decoded");
    CHECK(!decodeSound({}), "nothing decoded");
    const fs::Bytes empty = encodeWav({}, 2, kRate);
    const Result<SoundData> noFrames = decodeSound(empty);
    CHECK(!noFrames, "a WAV without frames decoded");
    const std::span<const std::uint8_t> flac = quench::test::toneFlac();
    CHECK(!decodeSound(flac.first(60)), "a truncated FLAC header decoded");
    const std::span<const std::uint8_t> ogg = quench::test::loopOgg();
    CHECK(!decodeSound(ogg.first(60)), "a truncated Ogg Vorbis header decoded");
    std::vector<std::uint8_t> notVorbis(3000, 0x42);
    std::copy(ogg.begin(), ogg.begin() + 4, notVorbis.begin()); // "OggS", then no Ogg pages
    CHECK(!decodeSound(notVorbis), "an Ogg signature without pages decoded");
}

QUENCH_TEST(sound_assets) {
    AssetDir dir("audio_sound_assets");
    dir.write("tone.wav", encodeWav(sineSamples(440.0f, 0.5f, 0.2, 1), 1, kRate));
    dir.write("broken.wav", std::vector<std::uint8_t>(3000, 0x7f));
    JobSystem jobs(JobSystemDesc{.workerThreads = QUENCH_TEST_THREADS ? 2 : 0});
    assets::AssetManager manager(jobs);
    {
        AudioSettings settings;
        settings.maxStartDelay = 0.05;
        AudioSystem audio(jobs, &manager, settings);
        audio.setOffline(true);
        std::vector<std::pair<SoundHandle, EndReason>> ended;
        audio.setEndCallback([&](SoundHandle h, EndReason r) { ended.emplace_back(h, r); });

        assets::AssetRef<SoundAsset> tone = audio.load("tone.wav");
        const SoundHandle loop = audio.play(tone, {.loop = true});
        const SoundHandle missing = audio.play("missing.wav");
        const SoundHandle broken = audio.play("broken.wav", {.loop = true});
        CHECK(audio.state(loop) == SoundState::Pending, "pending while loading");
        std::vector<float> out;
        for (int i = 0; i < 400 && (audio.state(loop) != SoundState::Playing || audio.alive(broken)); ++i) {
            manager.update();
            audio.update();
            audio.mixOffline(0.001, &out);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(tone.ready() && tone->data() && tone->data()->frames() == std::size_t(0.2 * kRate), "loaded");
        CHECK(audio.state(loop) == SoundState::Playing, "the loop started once loaded");
        CHECK(!audio.alive(missing) && !audio.alive(broken), "missing and broken sounds end");
        int failed = 0;
        for (const auto& [handle, reason] : ended) failed += reason == EndReason::Failed;
        CHECK(failed == 2, "two failed: %d of %zu", failed, ended.size());
        const assets::AssetManager::Stats stats = manager.stats();
        bool counted = false;
        for (const auto& type : stats.types)
            if (type.name == "sound") counted = type.memory.cpu == std::uint64_t(0.2 * kRate) * sizeof(float);
        CHECK(counted, "the decoded sound's memory is counted");

        // A one-shot of a sound that takes longer than maxStartDelay to load is dropped.
        manager.setSimulatedLatency(0.5);
        const SoundHandle late = audio.play("tone_late.wav");
        dir.write("tone_late.wav", encodeWav(sineSamples(440.0f, 0.5f, 0.2, 1), 1, kRate));
        for (int i = 0; i < 20; ++i) {
            manager.update();
            audio.update();
            audio.mixOffline(0.01, &out);
        }
        REQUIRE(!ended.empty(), "nothing ended");
        CHECK(!audio.alive(late) && ended.back().first == late && ended.back().second == EndReason::Dropped,
              "late one-shot dropped: %s", endReasonName(ended.back().second));
        manager.setSimulatedLatency(0.0);
        audio.stopAll();
        audio.update();
    }
    // The AudioSystem unregistered its type: loading again needs a new one.
    CHECK(manager.stats().types.empty() || manager.stats().types[0].name != "sound", "type unregistered");
}

// ---------------------------------------------------------------------------------------------------- weather

QUENCH_TEST(weather_audio_gains) {
    const auto gains = [](float precipitation, float distance, float cover) {
        return WeatherAudio::targetGains({precipitation, distance, cover}, 4000.0f);
    };
    const auto at = [](const std::array<float, kWeatherLayers>& g, WeatherLayer layer) {
        return g[std::size_t(layer)];
    };
    for (float g : gains(0.0f, 1e9f, 0.0f)) CHECK(g == 0.0f, "dry and no rain near: silent");
    const auto trace = gains(0.01f, 0.0f, 0.0f);
    CHECK(at(trace, WeatherLayer::RainLight) == 0.0f && at(trace, WeatherLayer::RainHeavy) == 0.0f,
          "a trace of rain is not heard at the listener");
    const auto light = gains(0.15f, 0.0f, 0.0f);
    CHECK(at(light, WeatherLayer::RainLight) > 0.2f && at(light, WeatherLayer::RainHeavy) == 0.0f, "light rain");
    const auto heavy = gains(0.9f, 0.0f, 0.0f);
    CHECK(at(heavy, WeatherLayer::RainHeavy) > 0.9f && at(heavy, WeatherLayer::RainLight) == 0.0f, "heavy rain");
    // Equal power between light and heavy: their squares sum to the loudness at every precipitation.
    for (float p = 0.05f; p <= 1.0f; p += 0.05f) {
        const auto g = gains(p, 0.0f, 0.0f);
        const float power = at(g, WeatherLayer::RainLight) * at(g, WeatherLayer::RainLight) +
                            at(g, WeatherLayer::RainHeavy) * at(g, WeatherLayer::RainHeavy);
        CHECK(near(power, (p - 0.02f) / 0.98f, 1e-4f), "power %f at %f", power, p);
    }
    const auto covered = gains(0.6f, 0.0f, 1.0f);
    CHECK(at(covered, WeatherLayer::RainLight) == 0.0f && at(covered, WeatherLayer::RainHeavy) == 0.0f &&
              at(covered, WeatherLayer::RainCover) > 0.7f,
          "under a roof only the roof is heard");
    const auto near_ = gains(0.0f, 500.0f, 0.0f), far = gains(0.0f, 3500.0f, 0.0f), beyond = gains(0.0f, 5000.0f, 0.0f);
    CHECK(at(near_, WeatherLayer::RainFar) > at(far, WeatherLayer::RainFar) && at(far, WeatherLayer::RainFar) > 0.0f &&
              at(beyond, WeatherLayer::RainFar) == 0.0f,
          "distant rain grows as it comes nearer: %f %f %f", at(near_, WeatherLayer::RainFar),
          at(far, WeatherLayer::RainFar), at(beyond, WeatherLayer::RainFar));
    CHECK(at(gains(0.5f, 0.0f, 0.0f), WeatherLayer::RainFar) == 0.0f, "no distant layer inside the rain");
}

// The engine's default weather sounds (tools/assets/make_weather_sounds.py, written at build time where the build has
// Python and the cook): loops without a seam, below full scale, heavy rain louder than light, the light rain's
// energy the highest, the distant rain's the lowest and the roof's below the open-air rain's. The share of a band is
// the energy of the sound through a one-pole low-pass (or what it leaves: the high-pass) against the whole; one pole
// is a gentle slope, so the shares overlap.
QUENCH_TEST(weather_default_sounds) {
#ifndef QUENCH_TEST_WEATHER_SOUNDS_DIR
    QUENCH_SKIP("the default weather sounds are written in host builds with Python only");
#else
    struct Measured {
        float rms = 0.0f, peak = 0.0f, below700 = 0.0f, above2k = 0.0f, seam = 0.0f, step = 0.0f;
    };
    const auto lowShare = [](const std::vector<float>& mono, float hz, std::uint32_t rate, bool high) {
        const float a = std::exp(-2.0f * std::numbers::pi_v<float> * hz / float(rate));
        double total = 0.0, kept = 0.0;
        float y = 0.0f;
        for (int pass = 0; pass < 2; ++pass) // twice around the loop, the second kept
            for (float x : mono) {
                y = (1.0f - a) * x + a * y;
                if (pass == 0) continue;
                const float out = high ? x - y : y;
                kept += double(out) * out;
                total += double(x) * x;
            }
        return total > 0.0 ? float(kept / total) : 0.0f;
    };
    const auto measure = [&](const char* name) {
        Measured m;
        std::ifstream file(std::filesystem::path(QUENCH_TEST_WEATHER_SOUNDS_DIR) / name, std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
        Result<SoundData> sound = decodeSound(bytes);
        CHECK(sound, "%s: %s", name, sound ? "" : sound.error().c_str());
        if (!sound) return m;
        const std::uint32_t channels = sound->format.channels, rate = sound->format.sampleRate;
        std::vector<float> mono(sound->frames());
        for (std::size_t f = 0; f < mono.size(); ++f) {
            float sum = 0.0f;
            for (std::uint32_t c = 0; c < channels; ++c) sum += sound->samples[f * channels + c];
            mono[f] = sum / float(channels);
        }
        const Levels levels = measureLevels(sound->samples, channels);
        m.rms = levels.rmsMono();
        m.peak = std::max(levels.peak[0], levels.peak[1]);
        m.below700 = lowShare(mono, 700.0f, rate, false);
        m.above2k = lowShare(mono, 2000.0f, rate, true);
        // The jump from the last frame to the first against the loop's largest change over two frames.
        m.seam = std::abs(mono.front() - mono.back());
        m.step = maxStep(mono);
        std::printf("  %s: %.1f s, rms %.4f, peak %.3f, energy below 700 Hz %.2f, above 2 kHz %.2f, seam %.4f "
                    "(largest step %.4f)\n",
                    name, sound->duration(), double(m.rms), double(m.peak), double(m.below700), double(m.above2k),
                    double(m.seam), double(m.step));
        CHECK(m.peak < 0.99f && m.rms > 0.01f, "%s: rms %f, peak %f", name, double(m.rms), double(m.peak));
        CHECK(m.seam <= m.step, "%s: the loop's seam %f is no larger than its steps (%f)", name, double(m.seam),
              double(m.step));
        return m;
    };
    const Measured light = measure("rain_light.wav"), heavy = measure("rain_heavy.wav");
    const Measured far = measure("rain_far.wav"), cover = measure("rain_cover.wav");
    CHECK(heavy.rms > light.rms * 1.5f, "heavy rain louder: %f against %f", double(heavy.rms), double(light.rms));
    CHECK(light.above2k > 0.45f && light.above2k > heavy.above2k, "light rain is high: %.2f above 2 kHz (heavy %.2f)",
          double(light.above2k), double(heavy.above2k));
    CHECK(far.below700 > 0.6f && far.below700 > cover.below700 && far.above2k < 0.1f,
          "distant rain is low: %.2f below 700 Hz, %.2f above 2 kHz", double(far.below700), double(far.above2k));
    CHECK(cover.below700 > heavy.below700 && cover.above2k < light.above2k,
          "the roof is duller than the open air: %.2f below 700 Hz (heavy %.2f)", double(cover.below700),
          double(heavy.below700));
#endif
}

// The layers mixed offline from sine loops of their own frequency: the strongest frequency of the mix says which
// layer plays. A changed sound and a reloaded file crossfade in; a disabled object goes silent and stops its sounds.
QUENCH_TEST(weather_audio_mix) {
    AssetDir dir("audio_weather_audio_mix");
    const std::pair<const char*, float> files[] = {{"light.wav", 3000.0f},
                                                   {"heavy.wav", 1000.0f},
                                                   {"far.wav", 200.0f},
                                                   {"cover.wav", 500.0f},
                                                   {"other.wav", 1500.0f}};
    for (const auto& [name, hz] : files) dir.write(name, encodeWav(sineSamples(hz, 0.5f, 1.0, 1), 1, kRate));
    JobSystem jobs(JobSystemDesc{.workerThreads = 0});
    assets::AssetManager manager(jobs);
    AudioSystem audio(jobs, &manager);
    audio.setOffline(true);
    std::vector<float> out;
    {
        WeatherAudio weather(
            audio, {.sounds = {"light.wav", "heavy.wav", "far.wav", "cover.wav"}, .bus = "effects", .smoothing = 0.1f});
        const auto run = [&](const WeatherSoundState& state, double seconds) {
            out.clear();
            for (double t = 0.0; t < seconds - 1e-9; t += 0.01) {
                manager.update();
                weather.update(state, 0.01);
                audio.update();
                audio.mixOffline(0.01, &out);
            }
        };
        const auto strongest = [&] { return dominantFrequency(last(out, 0.25), 2, kRate, 100.0f, 4000.0f); };

        run({.precipitation = 0.15f}, 1.0);
        Peak peak = strongest();
        CHECK(near(peak.frequency, 3000.0f, 2.0f), "light rain: %f Hz", peak.frequency);
        const float lightRms = measureLevels(last(out, 0.25), 2).rmsMono();
        run({.precipitation = 0.95f}, 1.0);
        peak = strongest();
        CHECK(near(peak.frequency, 1000.0f, 2.0f), "heavy rain: %f Hz", peak.frequency);
        CHECK(dominantFrequency(last(out, 0.25), 2, kRate, 2500.0f, 3500.0f).level < 0.01f, "light layer gone");
        CHECK(measureLevels(last(out, 0.25), 2).rmsMono() > lightRms * 1.5f, "heavy rain is louder");
        run({.precipitation = 0.95f, .cover = 1.0f}, 1.0);
        peak = strongest();
        CHECK(near(peak.frequency, 500.0f, 2.0f), "under cover: %f Hz", peak.frequency);
        CHECK(dominantFrequency(last(out, 0.25), 2, kRate, 800.0f, 1200.0f).level < 0.01f, "open-air layers gone");
        run({.precipitation = 0.0f, .distanceToRain = 300.0f}, 1.0);
        peak = strongest();
        CHECK(near(peak.frequency, 200.0f, 2.0f), "distant rain: %f Hz", peak.frequency);
        const float nearRms = measureLevels(last(out, 0.25), 2).rmsMono();
        run({.precipitation = 0.0f, .distanceToRain = 3000.0f}, 1.0);
        const float farRms = measureLevels(last(out, 0.25), 2).rmsMono();
        CHECK(farRms > 0.0f && farRms < nearRms * 0.5f, "farther rain is quieter: %f < %f", farRms, nearRms);

        // Another sound for a playing layer, then a hot reload of its file: each crossfades into the new data.
        run({.precipitation = 0.95f}, 0.5);
        weather.setSound(WeatherLayer::RainHeavy, "other.wav");
        run({.precipitation = 0.95f}, 1.0);
        peak = strongest();
        CHECK(near(peak.frequency, 1500.0f, 2.0f), "the game's sound: %f Hz", peak.frequency);
        CHECK(maxStep(out) < 0.2f, "no click in the crossfade: %f", maxStep(out));
        dir.write("other.wav", encodeWav(sineSamples(1800.0f, 0.5f, 1.0, 1), 1, kRate));
        manager.reload(audio.load("other.wav")->id());
        run({.precipitation = 0.95f}, 1.0);
        peak = strongest();
        CHECK(near(peak.frequency, 1800.0f, 2.0f), "reloaded: %f Hz", peak.frequency);

        // The bus a game names: muting it silences the rain.
        weather.setBus("ui");
        run({.precipitation = 0.95f}, 1.0);
        audio.setBusMuted(audio.bus("ui"), true);
        run({.precipitation = 0.95f}, 0.5);
        CHECK(measureLevels(last(out, 0.25), 2).rmsMono() < 1e-4f, "on the bus it was given");
        audio.setBusMuted(audio.bus("ui"), false);

        // Dry weather: silent at once, the sounds stopped after two seconds.
        run({.precipitation = 0.0f}, 1.0);
        CHECK(measureLevels(last(out, 0.25), 2).rmsMono() < 1e-4f, "dry: silent");
        run({.precipitation = 0.0f}, 2.5);
        CHECK(audio.stats().sounds == 0, "dry: sounds stopped (%u alive)", audio.stats().sounds);

        run({.precipitation = 0.95f}, 0.5);
        CHECK(audio.stats().sounds > 0, "rain again");
        weather.setEnabled(false);
        run({.precipitation = 0.95f}, 1.0);
        CHECK(measureLevels(last(out, 0.25), 2).rmsMono() < 1e-4f && audio.stats().sounds == 0, "disabled: silent");
    }
    audio.update();
}

// ---------------------------------------------------------------------------------------------------- analysis

QUENCH_TEST(analysis_levels_and_frequency) {
    const std::vector<float> sine = sineSamples(1234.5f, 0.8f, 1.0);
    const Levels levels = measureLevels(sine, 2);
    CHECK(near(levels.rms[0], 0.8f / std::numbers::sqrt2_v<float>, 1e-3f) && near(levels.peak[1], 0.8f, 1e-3f),
          "rms %f peak %f", levels.rms[0], levels.peak[1]);
    const Peak peak = dominantFrequency(sine, 2, kRate);
    CHECK(near(peak.frequency, 1234.5f, 0.5f) && near(peak.level, 0.8f, 0.08f), "peak %f Hz level %f", peak.frequency,
          peak.level);
    const Peak limited = dominantFrequency(sine, 2, kRate, 20.0f, 1000.0f);
    CHECK(limited.frequency < 1000.0f && limited.level < 0.05f, "outside the band: %f Hz %f", limited.frequency,
          limited.level);
    CHECK(dominantFrequency(std::vector<float>(4096, 0.0f), 2, kRate).frequency == 0.0f, "silence");
    CHECK(measureLevels({}, 2).frames == 0, "empty");
}
