#pragma once

// The source behind an audio::Microphone (Microphone.cpp): the fake one there, miniaudio's capture device
// (MicrophoneDevice.cpp) or the browser's getUserMedia (MicrophoneWeb.cpp) in quench_audio_device. All calls come
// from the main thread. Frames reach the Microphone through a SampleRing (Queue.h) of mono floats at sampleRate():
// a device's callback thread writes it, the main thread reads it in Microphone::update().

#include <quench/audio/Microphone.h>

#include "Queue.h"

#include <cstdint>
#include <string>

namespace quench::audio::detail {

enum class MicOpen : std::uint8_t { Closed, Opening, Open, Failed };

class MicBackend {
public:
    virtual ~MicBackend() = default;

    virtual bool fake() const { return false; }
    // Polled every update(): cheap (a cached value the platform's callbacks change).
    virtual MicPermission permission() = 0;
    virtual void requestPermission() = 0;

    // Starts opening the input; poll() follows it. Called only while closed (after close() it may still be closing:
    // the backend opens after that ends).
    virtual void open() = 0;
    // Every update() after open(): Opening until the input delivers or failed. On Open, ring(), sampleRate() and
    // info() are valid until close(). On Failed, `error` says why and `noDevice` is true when there is no input at all
    // (the Microphone reports MicPermission::Unavailable then). Open can turn into Failed later (the input was lost).
    virtual MicOpen poll(std::string& error, bool& noDevice) = 0;
    // Main thread, every update() while open: the fake source and the web backend write their frames here; `dt` is
    // the frame's time (the fake's pace).
    virtual void pump(double dt) { (void)dt; }
    virtual SampleRing* ring() = 0;
    virtual std::uint32_t sampleRate() const = 0;
    virtual MicInfo info() const = 0;
    // Frames the input delivered that did not fit into the ring.
    virtual std::uint64_t dropped() const = 0;
    // Stops delivering; closing may end later (a job). The ring is not read afterwards.
    virtual void close() = 0;
};

} // namespace quench::audio::detail
