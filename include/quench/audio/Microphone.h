#pragma once

// Microphone capture (docs/audio.md, "Microphone"): short recordings from the default input, mono, resampled to the
// rate a game asks for (16 kHz by default, what speech-to-text services take), with a length limit and a level for a
// meter; or, in the streaming mode (MicDesc::stream), the same samples handed over in pieces while it records
// (takeStream()), without a length limit, for a live conversation (assistant::LiveSession), with a mute that keeps
// the input open. Headless: the recording side (the ring buffer the input fills, the resampler, the meter, the limit)
// and the fake source live here and run in tests and under Node.js; the system's input is a backend that
// quench_audio_device adds (audio::makeMicrophone in Device.h; App::microphone() in a game).
//
// Threads: a device's callback thread only copies its frames into a lock-free ring buffer. Everything a game reads is
// made on the main thread in update(): the ring is drained there, measured, resampled and cut at the limit. Opening
// and closing a device run on the job system (they can take tens of milliseconds, more with a Bluetooth headset), so
// start() and stop() return at once; state() says where it is. Main thread only.
//
// Permission: requestPermission() shows the system's prompt where there is one (macOS, iOS, a browser); the answer
// arrives in a later frame. start() needs Granted, or Unknown where the platform has no permission query.
//
// The fake source (the default Microphone(), and App::microphone() in scripted runs) plays samples fed to it
// (feed()) at the pace of update()'s time, then silence; its permission is set with setFakePermission(). No device
// and no prompt is ever opened through it.

#include <quench/audio/Sound.h> // SoundData
#include <quench/audio/Wav.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Result.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quench::audio {

namespace detail {
class MicBackend;
}

enum class MicPermission : std::uint8_t {
    Unknown,  // the platform has no query (Windows, Linux) or has not answered yet: start() tries the device
    NotAsked, // requestPermission() shows the system's prompt
    Asking,   // the prompt is up (or the request is on its way)
    Granted,
    Denied,      // the player (or a policy) said no: only the system's settings change it now
    Unavailable, // no input device, no capture API (a web page that is not https or localhost), or the game was built
                 // without quench_add_game(... MICROPHONE ...)
};

enum class MicState : std::uint8_t {
    Idle,      // nothing recorded yet, or the last recording was taken or cancelled
    Opening,   // start() was called; the input is being opened (a job, or the browser)
    Recording, // frames arrive; level() and seconds() move
    Finished,  // stop(), the limit or a lost input ended it; takeRecording() has it
    Failed,    // the input could not be opened (error() says why); nothing was recorded
};

const char* micPermissionName(MicPermission permission); // "unknown", "not asked", ...
const char* micStateName(MicState state);                // "idle", "opening", ...

struct MicDesc {
    std::uint32_t sampleRate = 16000; // of the recording, 8000 .. 48000 Hz (a live session takes 24000)
    double maxSeconds = 30.0;         // a recording that reaches it ends there (MicRecording::limitReached), up to 600
    // The streaming mode: the samples go to takeStream() as they arrive instead of into a recording; no length limit
    // (maxSeconds is not used); takeRecording() gives a recording without samples at the end (its error and dropped
    // frames).
    bool stream = false;
    // Streaming: samples not taken yet are kept up to this many seconds; beyond it the oldest are dropped
    // (streamDropped()).
    double streamBufferSeconds = 5.0;
};

struct MicRecording {
    std::uint32_t sampleRate = 0;
    // Mono, -1 .. 1; a full-scale input can overshoot 1 a little after the resampler's filter (pcm16() and wav()
    // clamp).
    std::vector<float> samples;
    bool limitReached = false;       // it ran into MicDesc::maxSeconds
    std::string error;               // why it ended early (the input was lost); empty after stop() and the limit
    std::uint64_t droppedFrames = 0; // input frames lost because update() did not drain the ring in time

    double seconds() const { return sampleRate ? double(samples.size()) / sampleRate : 0.0; }
    // 16-bit samples, clamped and rounded as in encodeWav.
    std::vector<std::int16_t> pcm16() const;
    // A 16-bit mono WAV file at sampleRate, in memory: what a speech-to-text upload takes.
    fs::Bytes wav() const;
};

struct MicInfo {
    std::string device;                 // the input's name; "fake" for the fake source
    std::string backend;                // "Core Audio", "Null", "Web Audio", "fake"
    std::uint32_t deviceSampleRate = 0; // of the frames the input delivers (before the resampler)
};

// The reason shown to the player in the system's prompt. quench_add_game(... MICROPHONE "reason") declares it before
// main (a generated source of the game) and puts it into the iOS Info.plist as NSMicrophoneUsageDescription; a game
// built without it has no microphone entry, and requestPermission() and start() fail with an error that says so.
// Tests and tools that are not built with quench_add_game() declare it themselves.
void declareMicrophoneUsage(std::string_view reason);
const std::string& microphoneUsage(); // empty: not declared

class Microphone {
public:
    // The fake source: no device, no prompt (tests, scripted runs).
    Microphone();
    // A backend of quench_audio_device (audio::makeMicrophone).
    explicit Microphone(std::unique_ptr<detail::MicBackend> backend);
    // Closes the input (waits for an open or close job that is still running).
    ~Microphone();
    Microphone(const Microphone&) = delete;
    Microphone& operator=(const Microphone&) = delete;

    MicPermission permission() const;
    // Shows the system's prompt when permission() is NotAsked (on the web: whenever it is not Denied or
    // Unavailable; a browser that remembers the answer shows nothing). The answer arrives in permission() in a later
    // frame. Without a declared usage (declareMicrophoneUsage) permission() becomes Unavailable and error() says why.
    void requestPermission();

    // Starts a recording: Opening, then Recording once the input delivers. An error, and nothing changes, when the
    // usage is not declared, the permission is NotAsked, Asking, Denied or Unavailable, the desc is out of range, or
    // a recording is under way (Opening, Recording). A recording not taken yet is dropped.
    Result<void> start(const MicDesc& desc = {});
    // Ends the recording (Opening or Recording): what arrived until now is kept, state() is Finished at once and the
    // input closes in the background. Nothing otherwise.
    void stop();
    // Ends it and drops what was recorded: Idle.
    void cancel();
    // Once per frame on the main thread (App does it before Game::update): the permission's answer, the input's
    // opening, the ring drained into the recording, the level, the limit. `dt` paces the fake source only.
    void update(double dt);

    MicState state() const;
    bool active() const { return state() == MicState::Opening || state() == MicState::Recording; }
    // A smoothed peak of the input, 0 .. 1 (rises at once, falls by half in about 0.1 s), while recording; 0 otherwise.
    // Linear: a meter reads better on a dB scale (20 log10) or its square root.
    float level() const;
    // The length of the recording so far (at MicDesc::sampleRate), also after it finished.
    double seconds() const;
    bool limitReached() const;
    // Why the last request, start or recording failed or ended early; empty otherwise.
    const std::string& error() const;
    // The finished recording, once (state() becomes Idle); nullopt before it finished.
    std::optional<MicRecording> takeRecording();
    const MicInfo& info() const;

    // ---- the streaming mode (MicDesc::stream)
    // True while a stream records (Opening, Recording with MicDesc::stream).
    bool streaming() const;
    // Appends the samples (mono, -1 .. 1, at MicDesc::sampleRate) made since the last call to `out`; returns how many.
    // Call it every frame after update(); the samples of a frame's update() are there (about 1/60 s at 60 Hz).
    std::size_t takeStream(std::vector<float>& out);
    // Muted: silence goes into the stream instead of the input (the timeline stays continuous), the input stays open
    // and level() still shows it (a meter while the player is not heard). Kept across start() calls.
    void setStreamMuted(bool muted);
    bool streamMuted() const;
    // Seconds handed to the stream since start() (also after it ended).
    double streamedSeconds() const;
    // Samples dropped because takeStream() was not called for longer than MicDesc::streamBufferSeconds.
    std::uint64_t streamDropped() const;

    // The fake source. On a device's microphone feed() and setFakePermission() do nothing (one warning).
    bool fake() const;
    // Queues samples to play into the input after those queued before: interleaved frames of 1 or more channels
    // (averaged to mono) at any rate (resampled to the fake input's 48 kHz).
    void feed(std::span<const float> samples, std::uint32_t channels, std::uint32_t sampleRate);
    void feed(const SoundData& sound);
    void clearFeed();
    double queuedSeconds() const; // fed and not played yet
    // The permission the fake source reports from now on; a request turns NotAsked into Granted in the next update().
    void setFakePermission(MicPermission permission);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace quench::audio
