// audio::VoiceStream (quench/audio/VoiceStream.h): streamed playback with a jitter buffer, offline through the
// generator itself and through the mixer: the prebuffer, a short last piece, fades at underruns and flushes (no
// step larger than a fade's), the level, the played position, a full queue.

#include "Test.h"

#include <quench/audio/AudioSystem.h>
#include <quench/audio/VoiceStream.h>
#include <quench/core/Jobs.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <numbers>
#include <thread>
#include <vector>

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#define QUENCH_TEST_THREADS 1
#else
#define QUENCH_TEST_THREADS 0
#endif

using namespace quench;
using namespace quench::audio;

namespace {

std::vector<float> tone(double seconds, std::uint32_t rate = 24000, float amplitude = 0.5f, double hz = 300.0) {
    std::vector<float> out(std::size_t(seconds * rate));
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = amplitude * float(std::sin(2.0 * std::numbers::pi * hz * double(i) / rate));
    return out;
}

// Pulls `seconds` from the generator in blocks of 240 frames (10 ms).
std::vector<float> pull(VoiceStream& s, double seconds) {
    std::vector<float> out(std::size_t(seconds * s.desc().sampleRate));
    for (std::size_t at = 0; at < out.size(); at += 240) {
        const auto n = std::uint32_t(std::min<std::size_t>(240, out.size() - at));
        CHECK(s.generate(out.data() + at, n) == n, "always fills its frames");
    }
    return out;
}

// The largest difference between neighbouring samples: a step (a click) shows up here.
float largestStep(const std::vector<float>& v) {
    float worst = 0.0f;
    for (std::size_t i = 1; i < v.size(); ++i) worst = std::max(worst, std::abs(v[i] - v[i - 1]));
    return worst;
}

std::size_t firstNonZero(const std::vector<float>& v) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i] != 0.0f) return i;
    return v.size();
}

} // namespace

// Nothing plays until the prebuffer is queued; then it plays in order, fading in.
QUENCH_TEST(voice_stream_prebuffer) {
    VoiceStream s; // 24 kHz, 80 ms
    CHECK(!s.active() && !s.audible(), "idle");
    const std::vector<float> a = tone(0.05);
    s.push(a);
    CHECK(s.active() && std::abs(s.queuedSeconds() - 0.05) < 1e-6, "queued 50 ms");
    std::vector<float> out = pull(s, 0.02);
    CHECK(firstNonZero(out) == out.size(), "waiting under the prebuffer");
    s.push(tone(0.05));
    out = pull(s, 0.2);
    CHECK(firstNonZero(out) < 10, "starts once 80 ms are queued: at %zu", firstNonZero(out));
    // The tone's own largest step at 300 Hz is 0.5 * 2 pi 300 / 24000 = 0.039.
    CHECK(largestStep(out) < 0.045f, "no click at the start or at the underrun: %f", double(largestStep(out)));
    CHECK(s.playedFrames() == 2400, "played 100 ms: %llu", (unsigned long long)s.playedFrames());
    CHECK(s.underruns() == 1, "ran dry once at its end: %llu", (unsigned long long)s.underruns());
    CHECK(!s.active(), "nothing left");
}

// A short last piece (under the prebuffer) plays after waiting the prebuffer's time, not never.
QUENCH_TEST(voice_stream_short_tail_plays) {
    VoiceStream s;
    s.push(tone(0.03));
    const std::vector<float> out = pull(s, 0.2);
    const std::size_t start = firstNonZero(out);
    CHECK(start >= 1900 && start <= 1930, "after 80 ms of waiting: %zu", start);
    CHECK(s.playedFrames() == 720, "all 30 ms played: %llu", (unsigned long long)s.playedFrames());
}

// An underrun in the middle of a sentence fades out, waits for the prebuffer, fades back in: no step.
QUENCH_TEST(voice_stream_underrun_without_click) {
    VoiceStream s;
    // A constant level is the worst case for a step (a DC offset that drops to zero).
    s.push(std::vector<float>(2400, 0.8f));
    std::vector<float> first = pull(s, 0.15);
    s.push(std::vector<float>(2400, 0.8f));
    std::vector<float> second = pull(s, 0.2);
    first.insert(first.end(), second.begin(), second.end());
    const float step = largestStep(first);
    CHECK(step <= 0.8f / 96.0f + 1e-4f, "steps within a 4 ms fade's: %f", double(step));
    CHECK(s.underruns() == 2, "two underruns: %llu", (unsigned long long)s.underruns());
    CHECK(s.playedFrames() == 4800, "both pieces played");
}

// A flush drops what is queued and fades out what plays; what comes after plays.
QUENCH_TEST(voice_stream_flush) {
    VoiceStream s;
    s.push(std::vector<float>(24000, 0.5f)); // 1 s
    std::vector<float> out = pull(s, 0.2);
    CHECK(s.active() && s.queuedSeconds() > 0.7, "playing a long piece");
    s.flush();
    CHECK(s.queuedSeconds() == 0.0, "nothing queued after the flush");
    std::vector<float> after = pull(s, 0.05);
    out.insert(out.end(), after.begin(), after.end());
    CHECK(largestStep(out) <= 0.5f / 96.0f + 1e-4f, "faded out: %f", double(largestStep(out)));
    CHECK(firstNonZero(std::vector<float>(after.begin() + 100, after.end())) == after.size() - 100, "then silence");
    CHECK(!s.active() && s.flushes() == 1, "idle");
    s.push(tone(0.1));
    out = pull(s, 0.2);
    CHECK(firstNonZero(out) < 10 && s.playedFrames() > 24000 * 0.2, "the next answer plays");
}

// The level follows what plays, audible() above -50 dBFS; a full queue keeps what fits.
QUENCH_TEST(voice_stream_level_and_capacity) {
    VoiceStream s(VoiceStreamDesc{.sampleRate = 16000, .capacitySeconds = 1.0});
    s.push(tone(0.5, 16000, 0.6f));
    pull(s, 0.3);
    CHECK(s.level() > 0.55f && s.audible(), "level %f", double(s.level()));
    pull(s, 0.6);
    CHECK(s.level() < 0.05f, "falls by half every 0.1 s after: %f", double(s.level()));
    pull(s, 0.5);
    CHECK(!s.audible(), "silent");
    const std::vector<float> big = tone(1.5, 16000);
    CHECK(s.push(big) == 16000 && s.droppedFrames() == 8000, "1 s fits, 0.5 s dropped");
    CHECK(std::abs(s.playedSeconds() - 0.5) < 1e-6, "played 0.5 s: %f", s.playedSeconds());
}

// Through the mixer on a bus: the tone at its level on the master's output, at 48 kHz from 24 kHz.
QUENCH_TEST(voice_stream_in_the_mixer) {
    JobSystem jobs(JobSystemDesc{.workerThreads = 0});
    AudioSettings settings;
    settings.buses = {{"voice"}};
    AudioSystem audio(jobs, nullptr, settings);
    audio.setOffline(true);
    auto voice = std::make_shared<VoiceStream>();
    const SoundHandle h = audio.playGenerator(voice, {.bus = audio.bus("voice")});
    CHECK(h.valid(), "playing");
    voice->push(tone(0.5, 24000, 0.5f, 440.0));
    audio.update();
    std::vector<float> mix;
    for (int i = 0; i < 40; ++i) {
        audio.mixOffline(1.0 / 60.0, &mix);
        audio.update();
    }
    float peak = 0.0f;
    for (float v : mix) peak = std::max(peak, std::abs(v));
    CHECK(peak > 0.3f && peak < 0.6f, "the voice in the mix: peak %f", double(peak));
    CHECK(voice->playedFrames() == 12000, "all of it: %llu", (unsigned long long)voice->playedFrames());
    CHECK(audio.alive(h), "the sound does not end by itself");
}

// The audio thread pulls while the game thread pushes, flushes and reads the queries (for the thread sanitizer):
// everything pushed after the last flush is played, nothing more.
QUENCH_TEST(voice_stream_threads) {
#if !QUENCH_TEST_THREADS
    QUENCH_SKIP("no threads in this build");
#else
    VoiceStream s(VoiceStreamDesc{.sampleRate = 24000, .capacitySeconds = 2.0});
    std::atomic<bool> stop{false};
    std::thread audioThread([&] {
        std::vector<float> out(256);
        while (!stop.load()) s.generate(out.data(), 256);
    });
    const std::vector<float> piece(2400, 0.25f);
    std::uint64_t pushed = 0;
    float levels = 0.0f;
    for (int i = 0; i < 400; ++i) {
        pushed += s.push(piece);
        if (i % 50 == 25) s.flush();
        levels += s.level() + float(s.active()) + float(s.audible()) + float(s.queuedSeconds());
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (s.queuedSeconds() > 0.0 && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    stop = true;
    audioThread.join();
    CHECK(s.queuedSeconds() == 0.0, "drained");
    CHECK(s.playedFrames() > 0 && s.playedFrames() <= pushed, "played %llu of %llu pushed",
          (unsigned long long)s.playedFrames(), (unsigned long long)pushed);
    CHECK(levels >= 0.0f, "queries");
#endif
}
