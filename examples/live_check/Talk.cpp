// The live check's talk mode (quench-live-check --talk): a person talks to a GPT-Live session through the Mac's
// default microphone and hears it on the default output, to find out whether the station hears itself on
// loudspeakers. No window: miniaudio's capture and playback devices and the AVFoundation permission run without one
// (the permission prompt, the first time, is the terminal's). Never run by the automated checks or by agents.
//
// What it prints, each line with its milliseconds: state changes, the player's and the station's transcript deltas
// (the station's own words showing up as the player's means it hears itself), the levels of the microphone and of
// the output four times a second while the station is audible (their ratio is the echo's strength), tool calls, each
// stop of the station's speech before its sentence ended, classified roughly, and at the end the seconds billed and
// the cost.
//
// The classification of a stop: the station's audio fell silent for 0.8 s while its transcript did not end with
// . ! ? or an ellipsis. The echo level is the mean microphone level while the station was audible (what the
// microphone hears of the speakers, plus the room); if the microphone's highest level in the 0.6 s before the stop
// was under twice that, nothing but the echo was there: "self-interruption suspected"; louder: a person spoke,
// "barge-in". Rough: a quiet voice counts as an echo, a loud room as a person.

#include "Talk.h"

#include <quench/assistant/Live.h>
#include <quench/audio/AudioSystem.h>
#include <quench/audio/Device.h>
#include <quench/audio/Microphone.h>
#include <quench/audio/VoiceStream.h>
#include <quench/core/Jobs.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace qa = quench::assistant;
namespace audio = quench::audio;

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop.store(true); }

double now() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point origin = clock::now();
    return std::chrono::duration<double>(clock::now() - origin).count();
}

double db(float level) { return level > 1e-6f ? 20.0 * std::log10(double(level)) : -120.0; }

bool sentenceEnded(const std::string& text) {
    std::size_t end = text.find_last_not_of(" \t\n");
    if (end == std::string::npos) return true;
    const char c = text[end];
    if (c == '.' || c == '!' || c == '?') return true;
    return end >= 2 && text.compare(end - 2, 3, "\xE2\x80\xA6") == 0; // an ellipsis character
}

} // namespace

int runTalk(qa::LiveSession& s, bool gate, double seconds) {
    std::signal(SIGINT, onSignal);
    quench::JobSystem jobs;
    audio::AudioSettings settings;
    settings.buses = {{"voice"}};
    audio::AudioSystem mixer(jobs, nullptr, settings);
    quench::Result<std::unique_ptr<audio::Device>> device = audio::Device::open(mixer);
    if (!device) {
        std::printf("talk: no audio output: %s\n", device.error().c_str());
        return 1;
    }
    auto voice = std::make_shared<audio::VoiceStream>(audio::VoiceStreamDesc{.sampleRate = s.settings().sampleRate});
    mixer.playGenerator(voice, {.bus = mixer.bus("voice")});

    audio::declareMicrophoneUsage("the live check's talk mode");
    std::unique_ptr<audio::Microphone> mic = audio::makeMicrophone(jobs);
    const double t0 = now();
    auto ms = [&] { return int((now() - t0) * 1000.0 + 0.5); };
    if (mic->permission() == audio::MicPermission::NotAsked) mic->requestPermission();
    while (mic->permission() == audio::MicPermission::Asking && now() - t0 < 60.0 && !g_stop) {
        mic->update(0.01);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (mic->permission() != audio::MicPermission::Granted && mic->permission() != audio::MicPermission::Unknown) {
        std::printf("talk: no microphone: %s %s\n", audio::micPermissionName(mic->permission()), mic->error().c_str());
        return 1;
    }
    if (quench::Result<void> r = mic->start({.sampleRate = s.settings().sampleRate, .stream = true}); !r) {
        std::printf("talk: microphone: %s\n", r.error().c_str());
        return 1;
    }

    s.settings().uplinkGate = gate; // the idle close stays on (settings().idleSeconds)
    std::printf("talk: output '%s', gate %s, %.0f s at most (Ctrl-C closes), idle close after %.0f s; speak now\n",
                (*device)->info().name.c_str(), gate ? "on" : "off", seconds, s.settings().idleSeconds);
    if (quench::Result<void> r = s.start(); !r) {
        std::printf("talk: start refused: %s\n", r.error().c_str());
        return 1;
    }

    std::vector<float> buf;
    double last = now(), lastLevels = 0.0;
    bool closing = false;
    double closeAt = 0.0;
    // The echo estimate and the stop detection.
    double echoSum = 0.0;
    long echoCount = 0;
    std::deque<std::pair<double, float>> micRecent; // (time, level) of the last 0.6 s
    bool wasAudible = false;
    double silentSince = -1.0;
    float micMaxAtStop = 0.0f;
    double echoAtStop = 0.0;
    int selfInterruptions = 0, bargeIns = 0;

    while (true) {
        const double t = now();
        const double dt = t - last;
        last = t;
        mic->update(dt);
        buf.clear();
        mic->takeStream(buf);
        s.sendAudio(buf);
        s.update();
        buf.clear();
        s.takeAudio(buf);
        voice->push(buf);
        const bool audible = voice->audible();
        s.setPlaybackActive(audible);
        mixer.update();
        (*device)->update();

        const float micLevel = mic->level();
        micRecent.emplace_back(t, micLevel);
        while (!micRecent.empty() && micRecent.front().first < t - 0.6) micRecent.pop_front();
        if (audible) {
            echoSum += micLevel;
            ++echoCount;
            if (t - lastLevels >= 0.25) {
                lastLevels = t;
                std::printf("%7d ms  levels: mic %.3f (%.0f dB), out %.3f (%.0f dB), mic/out %.0f dB%s\n", ms(),
                            double(micLevel), db(micLevel), double(voice->level()), db(voice->level()),
                            db(micLevel) - db(voice->level()), s.gateClosed() ? " (gate closed)" : "");
            }
            silentSince = -1.0;
        } else if (wasAudible) {
            silentSince = t;
            micMaxAtStop = 0.0f;
            for (const auto& [when, level] : micRecent) micMaxAtStop = std::max(micMaxAtStop, level);
            echoAtStop = echoCount ? echoSum / double(echoCount) : 0.0;
        }
        wasAudible = audible;
        if (silentSince > 0.0 && t - silentSince >= 0.8) {
            silentSince = -1.0;
            std::string text;
            for (auto it = s.history().rbegin(); it != s.history().rend(); ++it)
                if (it->speaker == qa::LiveSpeaker::Station) {
                    text = it->text;
                    break;
                }
            if (!sentenceEnded(text)) {
                const bool self = double(micMaxAtStop) < 2.0 * echoAtStop;
                (self ? selfInterruptions : bargeIns) += 1;
                std::printf("%7d ms  the station stopped mid-sentence: %s (microphone at most %.3f before the stop, "
                            "echo level %.3f)\n",
                            ms(), self ? "self-interruption suspected" : "barge-in", double(micMaxAtStop), echoAtStop);
            }
        }

        for (const qa::LiveEvent& e : s.takeEvents()) {
            switch (e.kind) {
            case qa::LiveEventKind::StateChanged:
                std::printf("%7d ms  state: %s%s\n", ms(), qa::liveStateName(e.state),
                            e.state == qa::LiveState::CallingTool ? (" " + s.toolName()).c_str() : "");
                break;
            case qa::LiveEventKind::InputTranscript:
                std::printf("%7d ms  player: \"%s\"\n", ms(), e.text.c_str());
                break;
            case qa::LiveEventKind::OutputTranscript:
                std::printf("%7d ms  station: \"%s\"\n", ms(), e.text.c_str());
                break;
            case qa::LiveEventKind::ToolStarted: {
                const qa::LiveToolCall& c = s.toolCalls()[e.tool];
                std::printf("%7d ms  tool call %s %s\n", ms(), c.name.c_str(), c.arguments.c_str());
                break;
            }
            case qa::LiveEventKind::Closed:
                std::printf("%7d ms  closed: %s\n", ms(), qa::liveCloseReasonName(e.reason));
                break;
            case qa::LiveEventKind::Failed: std::printf("%7d ms  FAILED: %s\n", ms(), e.text.c_str()); break;
            case qa::LiveEventKind::ServerError: std::printf("%7d ms  server error: %s\n", ms(), e.text.c_str()); break;
            default: break;
            }
        }
        if (s.state() == qa::LiveState::Closed || s.state() == qa::LiveState::Failed) break;
        if (!closing && (g_stop || t - t0 >= seconds)) {
            std::printf("%7d ms  closing (%s)\n", ms(), g_stop ? "Ctrl-C" : "the time limit");
            s.close();
            closing = true;
            closeAt = t;
        }
        if (closing && t - closeAt > 20.0) {
            s.cancel();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    mic->stop();
    const qa::LiveUsage u = s.usage();
    std::printf("\ntalk: gate %s; self-interruptions suspected: %d; barge-ins: %d; echo level (mean microphone level "
                "while the station was audible) %.3f (%.0f dB)\n",
                gate ? "on" : "off", selfInterruptions, bargeIns, echoCount ? echoSum / double(echoCount) : 0.0,
                db(float(echoCount ? echoSum / double(echoCount) : 0.0)));
    std::printf("talk: billed %.1f s (%s), cost $%.4f session + $%.6f backend\n", u.totalSeconds,
                u.confirmed ? "confirmed" : "unconfirmed", u.sessionCost, u.backendCost);
    return s.state() == qa::LiveState::Failed ? 1 : 0;
}
