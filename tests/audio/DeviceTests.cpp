#include "Test.h"

#include "core/Platform.h"

#if QUENCH_PLATFORM_IOS
#include "audio/AudioSession.h"
#endif

#include <quench/audio/Analysis.h>
#include <quench/audio/AudioSystem.h>
#include <quench/audio/Device.h>
#include <quench/core/Jobs.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <string>
#include <thread>

using namespace quench;
using namespace quench::audio;

namespace {

class Tone final : public Generator {
public:
    Tone(float frequency, float amplitude)
        : Generator({1, 48000}), m_step(2.0 * std::numbers::pi * frequency / 48000.0), m_amplitude(amplitude) {}
    std::uint32_t generate(float* out, std::uint32_t frames) override {
        for (std::uint32_t f = 0; f < frames; ++f) {
            out[f] = float(m_amplitude * std::sin(m_phase));
            m_phase = std::fmod(m_phase + m_step, 2.0 * std::numbers::pi);
        }
        return frames;
    }

private:
    double m_phase = 0.0, m_step, m_amplitude;
};

// Runs the frame loop for up to `seconds` until `done` is true.
template <class Done>
bool pump(AudioSystem& audio, Device& device, double seconds, Done done) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        audio.update();
        device.update();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

// miniaudio's null backend: a device thread pulls the mix without any sound.
QUENCH_TEST(null_device) {
    JobSystem jobs;
    AudioSystem audio(jobs, nullptr);
    audio.setMonitor(1.0);
    Result<std::unique_ptr<Device>> device = Device::open(audio, {.name = "null"});
    REQUIRE(device, "open: %s", device.error().c_str());
    CHECK((*device)->running() && (*device)->info().backend == "Null", "backend '%s'",
          (*device)->info().backend.c_str());
    audio.playGenerator(std::make_shared<Tone>(1000.0f, 0.5f));
    const bool mixed = pump(audio, **device, 3.0, [&] { return audio.monitored(0.3).size() >= 0.3 * 48000 * 2; });
    REQUIRE(mixed, "the device thread mixed %llu frames", (unsigned long long)audio.stats().framesMixed);
    const Peak peak = dominantFrequency(audio.monitored(0.3), 2, 48000);
    CHECK(std::abs(peak.frequency - 1000.0f) < 2.0f, "monitored %f Hz", peak.frequency);

    (*device)->suspend();
    CHECK(!(*device)->running(), "suspended");
    const std::uint64_t frames = audio.stats().framesMixed;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(audio.stats().framesMixed == frames, "no mixing while suspended");
    (*device)->resume();
    CHECK(pump(audio, **device, 3.0, [&] { return audio.stats().framesMixed > frames + 4800; }), "mixing again");
}

// Opt-in: a short quiet tone on the default output (QUENCH_TEST_AUDIO_DEVICE=1), the one test that makes a sound.
QUENCH_TEST(default_output) {
    if (!std::getenv("QUENCH_TEST_AUDIO_DEVICE")) QUENCH_SKIP("set QUENCH_TEST_AUDIO_DEVICE=1 to play a tone");
    JobSystem jobs;
    AudioSystem audio(jobs, nullptr);
    Result<std::unique_ptr<Device>> device = Device::open(audio);
    REQUIRE(device, "open: %s", device.error().c_str());
    std::printf("output '%s' (%s), %u Hz\n", (*device)->info().name.c_str(), (*device)->info().backend.c_str(),
                (*device)->info().deviceSampleRate);
    const SoundHandle tone = audio.playGenerator(std::make_shared<Tone>(440.0f, 0.1f), {.fadeIn = 0.05f});
    pump(audio, **device, 0.5, [] { return false; });
    audio.stop(tone, 0.05f);
    pump(audio, **device, 0.2, [] { return false; });
    CHECK(audio.stats().framesMixed > 0, "mixed");
}

// The iOS audio session's category (no sound: the session is configured, not used).
QUENCH_TEST(audio_session_category) {
#if !QUENCH_PLATFORM_IOS
    QUENCH_SKIP("iOS only");
#else
    char text[256] = {};
    REQUIRE(quench_audio_session_configure(2, text, sizeof(text)), "playback: %s", text);
    quench_audio_session_category(text, sizeof(text));
    CHECK(std::string(text) == "AVAudioSessionCategoryPlayback", "category %s", text);
    REQUIRE(quench_audio_session_configure(0, text, sizeof(text)), "ambient: %s", text);
    quench_audio_session_category(text, sizeof(text));
    CHECK(std::string(text) == "AVAudioSessionCategoryAmbient", "category %s", text);
    quench_audio_session_route(text, sizeof(text));
    CHECK(text[0] != '\0', "route");
    std::printf("route: %s\n", text);
#endif
}
