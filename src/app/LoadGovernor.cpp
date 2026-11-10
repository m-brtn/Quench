#include <quench/app/LoadGovernor.h>

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace quench::app {
namespace {

// A frame this long is a pause (a hidden tab, a debugger, a blocking load), not load.
constexpr double kPauseSeconds = 0.5;
constexpr double kLateIntervals = 1.25;   // a frame longer than this many intervals is late
constexpr double kSevereIntervals = 1.75; // a window whose mean frame is: two steps down at once
constexpr double kCpuBoundShare = 0.8;    // of the interval on the main thread: fewer pixels would not help
constexpr double kGpuFitShare = 0.85;     // of the interval the GPU may take after a step up

void sanitize(LoadGovernorDesc& d) {
    QUENCH_ASSERT(d.minScale > 0.0f && d.minScale <= 1.0f, "LoadGovernorDesc::minScale must be in (0, 1]");
    QUENCH_ASSERT(d.step > 0.0f && d.step < 1.0f, "LoadGovernorDesc::step must be in (0, 1)");
    QUENCH_ASSERT(d.frameRate >= 0.0, "LoadGovernorDesc::frameRate must not be negative");
    d.frameRate = std::isfinite(d.frameRate) ? std::clamp(d.frameRate, 0.0, 1000.0) : 0.0;
    d.minScale = std::isfinite(d.minScale) ? std::clamp(d.minScale, 0.05f, 1.0f) : 0.5f;
    d.step = std::isfinite(d.step) ? std::clamp(d.step, 0.5f, 0.98f) : 0.85f;
    d.windowSeconds = std::isfinite(d.windowSeconds) ? std::clamp(d.windowSeconds, 0.1, 60.0) : 1.0;
    d.lateShare = std::isfinite(d.lateShare) ? std::clamp(d.lateShare, 0.01f, 1.0f) : 0.25f;
    d.raiseSeconds = std::isfinite(d.raiseSeconds) ? std::max(d.raiseSeconds, d.windowSeconds) : 10.0;
    d.maxRaiseSeconds = std::isfinite(d.maxRaiseSeconds) ? std::max(d.maxRaiseSeconds, d.raiseSeconds) : 160.0;
    d.startSeconds = std::isfinite(d.startSeconds) ? std::max(d.startSeconds, 0.0) : 0.0;
}

} // namespace

void LoadGovernor::enable(LoadGovernorDesc desc) {
    sanitize(desc);
    m_desc = std::move(desc);
    log::info("app",
              "load reaction on: {}, render scale down to x{:.2f} in steps of x{:.2f}, up again after {} s without "
              "late frames",
              m_desc.frameRate > 0.0 ? std::format("{} Hz", m_desc.frameRate) : std::string("the app's frame rate"),
              m_desc.minScale, m_desc.step, m_desc.raiseSeconds);
    if (!m_enabled) {
        m_enabled = true;
        m_factor = 1.0f;
        m_time = 0.0;
        m_calm = 0.0;
        m_trying = false;
        m_raiseSeconds = m_desc.raiseSeconds;
        resetWindow();
        return;
    }
    // New limits for the factor already reached.
    m_raiseSeconds = std::clamp(m_raiseSeconds, m_desc.raiseSeconds, m_desc.maxRaiseSeconds);
    if (m_factor < m_desc.minScale) apply(m_desc.minScale, "a higher lowest factor", -1.0, -1.0);
}

void LoadGovernor::disable() {
    if (!m_enabled) return;
    m_enabled = false;
    m_trying = false;
    const bool changed = m_factor != 1.0f;
    log::info("app", "load reaction off{}", changed ? std::format(" (render scale x{:.2f} lifted)", m_factor) : "");
    m_factor = 1.0f;
    if (changed) {
        ++m_changes;
        if (m_desc.onChange) m_desc.onChange(*this);
    }
}

void LoadGovernor::resetWindow() {
    m_windowTime = m_windowCpu = m_windowGpu = 0.0;
    m_windowFrames = m_windowLate = m_windowGpuFrames = 0;
}

bool LoadGovernor::update(const FrameLoad& frame) {
    if (!m_enabled || !(frame.frame > 0.0) || !(frame.interval > 0.0)) return false;
    if (frame.frame > kPauseSeconds) {
        resetWindow();
        return false;
    }
    m_time += frame.frame;
    if (m_time < m_desc.startSeconds) return false;
    // A window is judged against one interval: a new frame rate starts a new one.
    if (m_windowFrames > 0 && frame.interval != m_interval) resetWindow();
    m_interval = frame.interval;
    m_windowTime += frame.frame;
    m_windowCpu += std::max(frame.cpu, 0.0);
    ++m_windowFrames;
    if (frame.frame > kLateIntervals * frame.interval) ++m_windowLate;
    if (frame.gpu >= 0.0) {
        m_windowGpu += frame.gpu;
        ++m_windowGpuFrames;
    }
    if (m_windowTime < m_desc.windowSeconds) return false;

    const double frames = double(m_windowFrames);
    const double seconds = m_windowTime;
    const double lateShare = double(m_windowLate) / frames;
    const double frameMean = seconds / frames;
    const double cpuMean = m_windowCpu / frames;
    const double gpuMean = m_windowGpuFrames > 0 ? m_windowGpu / double(m_windowGpuFrames) : -1.0;
    resetWindow();

    if (lateShare >= double(m_desc.lateShare)) {
        m_calm = 0.0;
        // The main thread makes the frames late, or nothing is left to give.
        if (cpuMean > kCpuBoundShare * m_interval || m_factor <= m_desc.minScale) return false;
        if (m_trying) {
            m_trying = false;
            m_raiseSeconds = std::min(m_raiseSeconds * 2.0, m_desc.maxRaiseSeconds);
        }
        float factor = m_factor * m_desc.step;
        if (frameMean > kSevereIntervals * m_interval) factor *= m_desc.step;
        apply(std::max(factor, m_desc.minScale), "late frames", frameMean * 1000.0, lateShare);
        return true;
    }

    m_calm += seconds;
    // A step up that lasted is no try any more, and the next one waits the plain time again.
    if (m_trying && m_calm >= m_desc.raiseSeconds) {
        m_trying = false;
        m_raiseSeconds = m_desc.raiseSeconds;
    }
    if (m_factor >= 1.0f || m_calm < m_raiseSeconds) return false;
    float factor = m_factor / m_desc.step;
    if (factor > 0.99f) factor = 1.0f;
    m_calm = 0.0;
    // Where the GPU's time is known: no step that would not fit (its time grows at most with the pixels).
    if (gpuMean >= 0.0 && gpuMean * double(factor * factor) / double(m_factor * m_factor) > kGpuFitShare * m_interval)
        return false;
    m_trying = true;
    apply(factor, "frames fit", frameMean * 1000.0, lateShare);
    return true;
}

void LoadGovernor::apply(float factor, const char* why, double frameMs, double lateShare) {
    const std::string numbers = frameMs >= 0.0
                                    ? std::format(": {:.0f}% of the frames late, {:.1f} ms a frame for {:.1f} ms",
                                                  lateShare * 100.0, frameMs, m_interval * 1000.0)
                                    : std::string();
    log::info("app", "load reaction: render scale x{:.2f} -> x{:.2f} ({}{})", m_factor, factor, why, numbers);
    m_factor = factor;
    ++m_changes;
    if (m_desc.onChange) m_desc.onChange(*this);
}

} // namespace quench::app
