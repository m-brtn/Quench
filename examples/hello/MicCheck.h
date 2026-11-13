#pragma once

// The microphone check a person runs (docs/audio.md, "The manual check"): `hello --mic-check SECONDS` asks for the
// permission (the system's prompt the first time), records SECONDS from the default input, prints the level four
// times a second and the length at the end, writes mic_check.wav (16-bit mono, 16 kHz) into the user directory and
// quits (exit code 0; 1 when the permission is refused, there is no input, or nothing was recorded). The window
// shows the same in its ImGui panel. Never run by the automated checks: it opens the real input.

#include <quench/app/App.h>
#include <quench/audio/Analysis.h>
#include <quench/audio/Microphone.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Log.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

class MicCheck {
public:
    explicit MicCheck(double seconds) : m_seconds(seconds) {}

    void update(quench::app::App& app, double dt) {
        namespace audio = quench::audio;
        audio::Microphone& mic = app.microphone();
        m_time += dt;
        if (m_done) return;
        switch (mic.state()) {
        case audio::MicState::Idle:
            if (!m_started) begin(app, mic);
            break;
        case audio::MicState::Opening: break;
        case audio::MicState::Recording:
            if (m_time >= m_nextPrint) {
                m_nextPrint = m_time + 0.25;
                const float level = mic.level();
                const int bars = std::clamp(int(std::lround(level * 40.0f)), 0, 40);
                std::printf("mic-check: %5.2f s  level %.3f  |%s%s|\n", mic.seconds(), double(level),
                            std::string(std::size_t(bars), '#').c_str(),
                            std::string(std::size_t(40 - bars), ' ').c_str());
                std::fflush(stdout);
            }
            break;
        case audio::MicState::Finished: finish(app, mic); break;
        case audio::MicState::Failed:
            std::printf("mic-check: cannot record: %s\n", mic.error().c_str());
            end(app, 1);
            break;
        }
    }

    void ui(quench::app::App& app) {
        const quench::audio::Microphone& mic = app.microphone();
        ImGui::Separator();
        ImGui::Text("Microphone check: %s, permission %s", quench::audio::micStateName(mic.state()),
                    quench::audio::micPermissionName(mic.permission()));
        ImGui::ProgressBar(mic.level(), ImVec2(ImGui::GetFontSize() * 14.0f, 0.0f), "level");
        ImGui::Text("%.2f of %.2f s", mic.seconds(), m_seconds);
    }

private:
    void begin(quench::app::App& app, quench::audio::Microphone& mic) {
        namespace audio = quench::audio;
        const audio::MicPermission permission = mic.permission();
        if (permission != m_reported) {
            m_reported = permission;
            std::printf("mic-check: permission %s%s\n", audio::micPermissionName(permission),
                        mic.fake() ? " (the fake source: an automated run)" : "");
            std::fflush(stdout);
        }
        switch (permission) {
        case audio::MicPermission::NotAsked: mic.requestPermission(); return;
        case audio::MicPermission::Asking: return; // the system's prompt is up
        case audio::MicPermission::Denied:
        case audio::MicPermission::Unavailable:
            std::printf("mic-check: no recording (%s). %s\n", audio::micPermissionName(permission),
                        mic.error().empty() ? "macOS: System Settings, Privacy & Security, Microphone"
                                            : mic.error().c_str());
            end(app, 1);
            return;
        case audio::MicPermission::Unknown:
        case audio::MicPermission::Granted: break;
        }
        if (quench::Result<void> r = mic.start({.sampleRate = 16000, .maxSeconds = m_seconds}); !r) {
            std::printf("mic-check: %s\n", r.error().c_str());
            end(app, 1);
            return;
        }
        m_started = true;
        std::printf("mic-check: recording %.1f s, speak now\n", m_seconds);
        std::fflush(stdout);
    }

    void finish(quench::app::App& app, quench::audio::Microphone& mic) {
        std::optional<quench::audio::MicRecording> r = mic.takeRecording();
        if (!r) return;
        const quench::audio::Levels levels = quench::audio::measureLevels(r->samples, 1);
        std::printf("mic-check: recorded %.2f s at %u Hz from '%s' (%s, %u Hz): rms %.4f, peak %.4f, %llu input "
                    "frames lost%s\n",
                    r->seconds(), r->sampleRate, mic.info().device.c_str(), mic.info().backend.c_str(),
                    mic.info().deviceSampleRate, double(levels.rmsMono()), double(levels.peak[0]),
                    static_cast<unsigned long long>(r->droppedFrames), r->error.empty() ? "" : ", ended early");
        const quench::fs::Bytes wav = r->wav();
        if (quench::Result<void> written = quench::fs::writeUser("mic_check.wav", wav); !written) {
            std::printf("mic-check: mic_check.wav: %s\n", written.error().c_str());
            end(app, 1);
            return;
        }
        char* pref = SDL_GetPrefPath(app.desc().organization.c_str(), app.desc().name.c_str());
        std::printf("mic-check: wrote mic_check.wav (%zu bytes) in the user directory (%s unless --user-dir)\n",
                    wav.size(), pref ? pref : "?");
        SDL_free(pref);
        end(app, r->samples.empty() ? 1 : 0);
    }

    void end(quench::app::App& app, int code) {
        std::fflush(stdout);
        m_done = true;
        app.quit(code);
    }

    double m_seconds;
    double m_time = 0.0, m_nextPrint = 0.0;
    bool m_started = false, m_done = false;
    quench::audio::MicPermission m_reported = quench::audio::MicPermission(255);
};
