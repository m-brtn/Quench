// The microphone's recording side (quench/audio/Microphone.h) with the fake source: headless, no device, no prompt.
// The ring buffer the input fills, the resampling to 16 kHz, the level meter, the length limit, the WAV encoder and
// the permission's states. The device itself (miniaudio's null backend) is in DeviceTests.cpp.

#include "Test.h"

#include "audio/Queue.h"

#include <quench/audio/Analysis.h>
#include <quench/audio/Microphone.h>
#include <quench/audio/Sound.h>
#include <quench/audio/Wav.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#define QUENCH_TEST_THREADS 1
#else
#define QUENCH_TEST_THREADS 0
#endif

using namespace quench;
using namespace quench::audio;

namespace quench::test {
std::span<const std::uint8_t> toneFlac(); // tests/audio/data/tone.flac: 0.5 s 440 Hz, 22050 Hz, amplitude 0.5
} // namespace quench::test

namespace {

constexpr double kFrame = 1.0 / 60.0;

std::vector<float> sine(double hz, double seconds, std::uint32_t rate, double amplitude) {
    std::vector<float> out(std::size_t(seconds * rate));
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = float(amplitude * std::sin(2.0 * std::numbers::pi * hz * double(i) / rate));
    return out;
}

// The usage declared and the fake microphone's permission granted.
void grant(Microphone& mic) {
    declareMicrophoneUsage("tests");
    mic.setFakePermission(MicPermission::Granted);
}

// Runs frames of kFrame until the recording has `seconds` (or 1000 frames passed).
void recordFor(Microphone& mic, double seconds) {
    for (int i = 0; i < 1000 && mic.active() && mic.seconds() < seconds; ++i) mic.update(kFrame);
}

} // namespace

// The ring the device's callback thread fills: wrap-around, a full ring refuses what does not fit.
QUENCH_TEST(mic_ring_buffer) {
    audio::detail::SampleRing ring(1, 1000);
    std::vector<float> in(1500), out(1500);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = float(i);
    CHECK(ring.write(in.data(), 700) == 700, "first write");
    CHECK(ring.read(out.data(), 500) == 500 && out[0] == 0.0f && out[499] == 499.0f, "first read");
    CHECK(ring.write(in.data() + 700, 700) == 700, "a write across the end");
    CHECK(ring.available() == 900, "available %u", ring.available());
    CHECK(ring.write(in.data() + 1400, 100) == 100 && ring.writable() == 0, "full");
    CHECK(ring.write(in.data(), 10) == 0, "nothing fits into a full ring");
    CHECK(ring.read(out.data(), 1500) == 1000, "read all");
    bool ordered = true;
    for (std::size_t i = 0; i < 1000; ++i) ordered = ordered && out[i] == float(500 + i);
    CHECK(ordered, "frames in order across the wrap");
}

// One producer thread (as a device's callback) and the consumer: every frame arrives once, in order.
QUENCH_TEST(mic_ring_buffer_threads) {
#if !QUENCH_TEST_THREADS
    QUENCH_SKIP("no threads in this build");
#else
    audio::detail::SampleRing ring(1, 4096);
    constexpr std::uint32_t kTotal = 2'000'000;
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        std::vector<float> block(480);
        std::uint32_t next = 0;
        while (next < kTotal && !stop.load()) {
            const std::uint32_t n = std::min<std::uint32_t>(480, kTotal - next);
            for (std::uint32_t i = 0; i < n; ++i) block[i] = float(next + i);
            std::uint32_t done = 0;
            while (done < n && !stop.load()) done += ring.write(block.data() + done, n - done);
            next += n;
        }
    });
    std::vector<float> out(1000);
    std::uint32_t expected = 0;
    bool ordered = true;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (expected < kTotal && std::chrono::steady_clock::now() < end) {
        const std::uint32_t n = ring.read(out.data(), 1000);
        for (std::uint32_t i = 0; i < n; ++i) ordered = ordered && out[i] == float(expected + i);
        expected += n;
    }
    stop = true;
    producer.join();
    CHECK(expected == kTotal, "%u of %u frames", expected, kTotal);
    CHECK(ordered, "in order");
#endif
}

// A sine in at 48 kHz and at 44.1 kHz, the same frequency out at 16 kHz, with the length and the level kept.
QUENCH_TEST(mic_resample_to_16k) {
    for (const std::uint32_t rate : {48000u, 44100u}) {
        Microphone mic;
        grant(mic);
        mic.feed(sine(1000.0, 1.5, rate, 0.5), 1, rate);
        REQUIRE(mic.start(), "start");
        recordFor(mic, 1.0);
        mic.stop();
        std::optional<MicRecording> r = mic.takeRecording();
        REQUIRE(r, "a recording at %u Hz", rate);
        CHECK(r->sampleRate == 16000, "rate %u", r->sampleRate);
        CHECK(r->seconds() >= 1.0 && r->seconds() < 1.03, "%f s", r->seconds());
        const Peak peak = dominantFrequency(r->samples, 1, 16000);
        CHECK(std::abs(peak.frequency - 1000.0f) < 1.0f, "%u Hz in: %f Hz out", rate, double(peak.frequency));
        // The middle, away from the filter's start.
        const Levels levels = measureLevels(std::span(r->samples).subspan(1600, 12800), 1);
        CHECK(std::abs(levels.rmsMono() - 0.5f / std::numbers::sqrt2_v<float>) < 0.01f, "rms %f",
              double(levels.rmsMono()));
        CHECK(r->droppedFrames == 0 && !r->limitReached, "nothing lost");
    }
}

// What is above the new Nyquist frequency (8 kHz) does not fold back into the recording.
QUENCH_TEST(mic_resample_filters_aliases) {
    Microphone mic;
    grant(mic);
    mic.feed(sine(14000.0, 1.0, 48000, 0.5), 1, 48000); // would fold to 2 kHz
    REQUIRE(mic.start(), "start");
    recordFor(mic, 0.8);
    mic.stop();
    std::optional<MicRecording> r = mic.takeRecording();
    REQUIRE(r, "a recording");
    const Levels levels = measureLevels(std::span(r->samples).subspan(1600), 1);
    CHECK(levels.rmsMono() < 0.01f, "rms %f of an input rms of 0.354", double(levels.rmsMono()));
}

// The meter: the peak while the tone plays, falling after it, 0 once the recording ended.
QUENCH_TEST(mic_level_meter) {
    Microphone mic;
    grant(mic);
    mic.feed(sine(500.0, 0.5, 48000, 0.6), 1, 48000);
    REQUIRE(mic.start(), "start");
    CHECK(mic.state() == MicState::Opening && mic.level() == 0.0f, "opening");
    recordFor(mic, 0.3);
    CHECK(mic.state() == MicState::Recording, "recording");
    CHECK(mic.level() > 0.55f && mic.level() <= 0.61f, "level %f during a 0.6 tone", double(mic.level()));
    recordFor(mic, 0.55);
    const float justAfter = mic.level();
    recordFor(mic, 0.75);
    CHECK(mic.level() < justAfter * 0.4f, "falls: %f then %f", double(justAfter), double(mic.level()));
    recordFor(mic, 1.7);
    CHECK(mic.level() < 0.001f, "silence: %f", double(mic.level()));
    mic.stop();
    CHECK(mic.level() == 0.0f, "0 after the end");
}

// A recording that runs into its limit ends there, with exactly the frames of the limit, and says so.
QUENCH_TEST(mic_length_limit) {
    Microphone mic;
    grant(mic);
    mic.feed(sine(300.0, 3.0, 48000, 0.3), 1, 48000);
    REQUIRE(mic.start({.sampleRate = 16000, .maxSeconds = 0.75}), "start");
    for (int i = 0; i < 200 && mic.state() != MicState::Finished; ++i) mic.update(kFrame);
    CHECK(mic.state() == MicState::Finished && mic.limitReached(), "ended by the limit (%s)",
          micStateName(mic.state()));
    std::optional<MicRecording> r = mic.takeRecording();
    REQUIRE(r, "a recording");
    CHECK(r->samples.size() == 12000 && r->limitReached, "%zu samples", r->samples.size());
    CHECK(mic.state() == MicState::Idle && !mic.takeRecording(), "taken once");
    CHECK(!mic.start({.sampleRate = 4000}) && !mic.start({.maxSeconds = 0.0}) && !mic.start({.maxSeconds = 601.0}),
          "out of range");
}

// The WAV of a recording: 16-bit mono at its rate, read back by the decoder within one step of 16 bits.
QUENCH_TEST(mic_wav_round_trip) {
    MicRecording r{.sampleRate = 16000, .samples = sine(440.0, 0.25, 16000, 0.7)};
    r.samples[10] = 1.5f; // clamped
    const fs::Bytes wav = r.wav();
    REQUIRE(wav.size() == 44 + r.samples.size() * 2, "%zu bytes", wav.size());
    CHECK(std::string(wav.begin(), wav.begin() + 4) == "RIFF" &&
              std::string(wav.begin() + 8, wav.begin() + 12) == "WAVE",
          "RIFF WAVE");
    const auto u16 = [&](std::size_t at) { return unsigned(wav[at] | (wav[at + 1] << 8)); };
    const auto u32 = [&](std::size_t at) { return u16(at) | (u16(at + 2) << 16); };
    CHECK(u16(20) == 1 && u16(22) == 1 && u32(24) == 16000 && u16(34) == 16, "PCM, mono, 16000 Hz, 16-bit");
    Result<SoundData> back = decodeSound(wav);
    REQUIRE(back, "decode: %s", back.error().c_str());
    REQUIRE(back->format.channels == 1 && back->format.sampleRate == 16000 && back->samples.size() == r.samples.size(),
            "%u channels, %u Hz, %zu samples", back->format.channels, back->format.sampleRate, back->samples.size());
    float worst = 0.0f;
    for (std::size_t i = 0; i < r.samples.size(); ++i)
        worst = std::max(worst, std::abs(back->samples[i] - std::clamp(r.samples[i], -1.0f, 1.0f)));
    CHECK(worst < 1.0f / 16384.0f, "largest difference %g", double(worst));
    // The 16-bit encoder writes the same bytes from pcm16().
    const std::vector<std::int16_t> pcm = r.pcm16();
    CHECK(pcm[10] == 32767, "clamped to %d", pcm[10]);
    CHECK(encodeWavPcm16(pcm, 1, 16000) == wav, "encodeWavPcm16 == MicRecording::wav()");
}

// The fake source fed from a sound file (FLAC at 22050 Hz): the file's tone in the recording.
QUENCH_TEST(mic_fake_source_from_sound) {
    Result<SoundData> sound = decodeSound(quench::test::toneFlac());
    REQUIRE(sound, "decode: %s", sound.error().c_str());
    Microphone mic;
    grant(mic);
    CHECK(mic.fake() && mic.info().device.empty(), "fake, not open yet");
    mic.feed(*sound);
    CHECK(std::abs(mic.queuedSeconds() - 0.5) < 0.002, "queued %f s", mic.queuedSeconds());
    REQUIRE(mic.start(), "start");
    recordFor(mic, 0.45);
    CHECK(mic.info().backend == "fake" && mic.info().deviceSampleRate == 48000, "info %s %u",
          mic.info().backend.c_str(), mic.info().deviceSampleRate);
    mic.stop();
    std::optional<MicRecording> r = mic.takeRecording();
    REQUIRE(r, "a recording");
    const Peak peak = dominantFrequency(r->samples, 1, 16000);
    CHECK(std::abs(peak.frequency - 440.0f) < 1.0f, "%f Hz", double(peak.frequency));
    // The pace follows update()'s time: 0.45 s within a frame of 1/60 s.
    CHECK(r->seconds() >= 0.45 && r->seconds() <= 0.45 + kFrame + 0.001, "%f s", r->seconds());
    mic.clearFeed();
    CHECK(mic.queuedSeconds() == 0.0, "cleared");
}

// The permission's states with the fake source, and what start() refuses.
QUENCH_TEST(mic_permission_states) {
    declareMicrophoneUsage("");
    Microphone mic;
    CHECK(mic.permission() == MicPermission::NotAsked, "not asked: %s", micPermissionName(mic.permission()));
    Result<void> undeclared = mic.start();
    CHECK(!undeclared && undeclared.error().find("MICROPHONE") != std::string::npos, "undeclared: %s",
          undeclared ? "" : undeclared.error().c_str());
    mic.requestPermission();
    CHECK(mic.permission() == MicPermission::Unavailable && mic.error().find("MICROPHONE") != std::string::npos,
          "a request without the usage: %s", micPermissionName(mic.permission()));

    declareMicrophoneUsage("tests");
    CHECK(microphoneUsage() == "tests", "declared");
    mic.setFakePermission(MicPermission::NotAsked);
    Result<void> notAsked = mic.start();
    CHECK(!notAsked && notAsked.error().find("requestPermission") != std::string::npos, "not asked: %s",
          notAsked ? "" : notAsked.error().c_str());
    mic.requestPermission();
    CHECK(mic.permission() == MicPermission::Asking, "asking");
    mic.update(kFrame);
    CHECK(mic.permission() == MicPermission::Granted, "granted: %s", micPermissionName(mic.permission()));
    REQUIRE(mic.start(), "start when granted");
    CHECK(!mic.start(), "one recording at a time");
    mic.cancel();
    CHECK(mic.state() == MicState::Idle && !mic.takeRecording(), "cancelled: nothing kept");

    for (const MicPermission p : {MicPermission::Denied, MicPermission::Unavailable}) {
        mic.setFakePermission(p);
        CHECK(!mic.start() && mic.state() == MicState::Idle, "refused when %s", micPermissionName(p));
    }
    mic.setFakePermission(MicPermission::Unknown);
    CHECK(mic.start(), "unknown: tried");
    mic.stop(); // before the input opened: a finished recording of nothing
    std::optional<MicRecording> r = mic.takeRecording();
    CHECK(r && r->samples.empty(), "stopped while opening");
}
