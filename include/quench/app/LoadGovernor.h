#pragma once

// An opt-in reaction to frames that come late (docs/app.md, "Frame load"): a factor for the render scale that goes
// down while the frames miss their interval and back up while they fit. It is for devices whose speed a game cannot
// know in advance and whose heat nobody reports (a browser has no thermal state; ThermalGovernor.h reacts to one
// where the system has it). The engine measures and decides the factor; the game applies it to its own render scale.

#include <cstdint>
#include <functional>

namespace quench::app {

class LoadGovernor;

struct LoadGovernorDesc {
    // The frame rate to hold. 0: the App's frame rate limit in effect (App::frameRateLimit), else the display's
    // refresh rate, else 60 (a browser does not tell the display's). A rate above those is lowered to them.
    double frameRate = 0.0;
    float minScale = 0.5f; // the lowest factor, (0, 1]
    // A step down multiplies the factor by this, a step up divides by it; (0, 1). 0.85 is 0.72 of the pixels.
    float step = 0.85f;
    // Frames are judged a window at a time. A frame is late when it took more than 1.25 intervals of the frame rate;
    // a window is overloaded when at least `lateShare` of its frames were, so single hitches (a load, a new
    // pipeline) do not count.
    double windowSeconds = 1.0;
    float lateShare = 0.25f;
    // Seconds without an overloaded window before a step up. Where the GPU time of a frame is known
    // (gpu::Caps::gpuTiming) the step is taken only when the frame would still fit after it; elsewhere it is a try,
    // and a try that brings late frames back doubles the wait for the next one, up to maxRaiseSeconds.
    double raiseSeconds = 10.0;
    double maxRaiseSeconds = 160.0;
    double startSeconds = 2.0; // frames ignored after enable(): a game's first frames load and compile
    // Called after every change of the factor, also by disable() when it lifts one.
    std::function<void(const LoadGovernor&)> onChange;
};

// One frame on the screen, in seconds.
struct FrameLoad {
    double frame = 0.0;    // from its start to the next frame's start
    double cpu = 0.0;      // the main thread's work in it (FrameStats::cpuMs)
    double gpu = -1.0;     // its GPU time; negative: unknown
    double interval = 0.0; // of the frame rate the App aims at (the limit in effect, else the display's rate, else 60)
};

// The opt-in reaction to late frames, off until enable(). The App owns one (App::loadGovernor()) and feeds it every
// frame on the screen that runs in real time (not offscreen, not the deterministic frames of a scripted run; the
// scenario command frame_load feeds fake frames to both). The player's render scale stays the game's:
// renderScale() returns it times the factor, and exactly the player's while nothing is late.
//
// The factor only helps where the GPU's work per pixel is what makes the frames late: while the main thread alone
// takes more than 0.8 of the interval the factor holds.
class LoadGovernor {
public:
    void enable(LoadGovernorDesc desc = {});
    void disable();
    bool enabled() const { return m_enabled; }
    const LoadGovernorDesc& desc() const { return m_desc; }

    // One frame. Returns true when the factor changed (after a log line and onChange). Nothing happens while off.
    bool update(const FrameLoad& frame);
    // Forgets the frames of the open window, so that what follows is judged on its own: after a loading screen or
    // a change of level, whose frames say nothing about the ones to come. The factor stays.
    void resetWindow();

    float renderScaleFactor() const { return m_factor; } // [minScale, 1]; 1 while off
    float renderScale(float playerScale) const { return playerScale * m_factor; }
    // The wait before the next step up: raiseSeconds, longer after tries that failed.
    double raiseSeconds() const { return m_raiseSeconds; }
    std::uint32_t changes() const { return m_changes; } // changes of the factor since the start

private:
    void apply(float factor, const char* why, double frameMs, double lateShare);

    LoadGovernorDesc m_desc;
    bool m_enabled = false;
    float m_factor = 1.0f;
    double m_time = 0.0;       // seconds of frames fed since enable()
    double m_interval = 0.0;   // of the window's frames
    double m_windowTime = 0.0; // the window so far
    double m_windowCpu = 0.0, m_windowGpu = 0.0;
    std::uint32_t m_windowFrames = 0, m_windowLate = 0, m_windowGpuFrames = 0;
    double m_calm = 0.0;         // seconds of windows without overload since the last change
    double m_raiseSeconds = 0.0; // the wait before the next step up
    bool m_trying = false;       // the last change was a step up that has not lasted raiseSeconds yet
    std::uint32_t m_changes = 0;
};

} // namespace quench::app
