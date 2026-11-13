#pragma once

// Audio output (quench_audio_device): a miniaudio playback device whose thread pulls the mix from an AudioSystem
// (AudioSystem::render). Core Audio on Apple platforms. On iOS the app's audio session is configured first (its
// category) and activated by the engine; an interruption (a call, Siri, an alarm) stops the device and update()
// starts it again when the interruption ends; route changes (headphones, Bluetooth) are followed by the system and
// logged. Main thread only.
//
// Also the microphone's device side (makeMicrophone below; the recording itself is quench/audio/Microphone.h).

#include <quench/audio/AudioSystem.h>
#include <quench/audio/Microphone.h>
#include <quench/core/Result.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace quench {
class JobSystem;
}

namespace quench::audio {

// iOS audio session categories (AVAudioSession); ignored elsewhere.
enum class SessionCategory : std::uint8_t {
    Ambient,     // mixes with other apps' audio; silenced by the Ring/Silent switch and the screen lock (the default)
    SoloAmbient, // silences other apps' audio; silenced by the switch and the screen lock
    Playback,    // silences other apps' audio; plays with the switch on silent
};

const char* sessionCategoryName(SessionCategory category);

struct DeviceDesc {
    // "" or "default": the system's default output (followed when it changes); "null": miniaudio's null backend, a
    // device thread that discards the mix (tests); anything else: the first output whose name contains it (else the
    // default, with a warning listing the outputs).
    std::string name{};
    SessionCategory session = SessionCategory::Ambient;
    std::uint32_t periodMs = 10; // requested callback period; the system may choose another
};

struct DeviceInfo {
    std::string name;                   // the output device
    std::string backend;                // miniaudio's backend name ("Core Audio", "Null")
    std::uint32_t sampleRate = 0;       // of the mix (miniaudio converts to the device's rate)
    std::uint32_t deviceSampleRate = 0; // of the hardware
    std::uint32_t periodFrames = 0;     // at the device's rate
};

class Device {
public:
    static Result<std::unique_ptr<Device>> open(AudioSystem& audio, const DeviceDesc& desc = {});
    // Stops the device thread (render() is not called afterwards).
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // Once per frame: starts the device again after an interruption ended, logs route changes.
    void update();
    // App lifecycle: the app goes to the background (stop) or comes back (start).
    void suspend();
    void resume();
    bool running() const;
    const DeviceInfo& info() const;
    std::uint64_t interruptions() const;
    std::uint64_t routeChanges() const;

    // Names of the playback devices of the default backend.
    static std::vector<std::string> outputNames();

private:
    struct Impl;
    explicit Device(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

struct MicDeviceDesc {
    // "" or "default": the system's default input; "null": miniaudio's null backend (silence at 48 kHz, no permission
    // to ask; tests). Choosing another input is not there yet (a warning, then the default).
    std::string name{};
    // iOS: the session category to go back to after a recording (AppDesc::audioDevice.session). While recording the
    // session is PlayAndRecord (AudioSession.h): playback stays on the speaker, other apps' audio stays as the
    // category has it.
    SessionCategory session = SessionCategory::Ambient;
};

// A Microphone on the system's input: miniaudio's capture device (Core Audio on Apple platforms), opened and closed
// on `jobs` (which must outlive it); getUserMedia in a browser. Its permission is the system's (MicPermission.h on
// Apple platforms, the Permissions API in a browser; unknown on Windows and Linux). App::microphone() is one of these
// outside scripted runs (docs/audio.md, "Microphone").
std::unique_ptr<Microphone> makeMicrophone(JobSystem& jobs, const MicDeviceDesc& desc = {});

} // namespace quench::audio
