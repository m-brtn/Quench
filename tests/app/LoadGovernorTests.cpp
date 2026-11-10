#include "Test.h"

#include <quench/app/LoadGovernor.h>

#include <cmath>

using namespace quench;
using app::FrameLoad;
using app::LoadGovernor;
using app::LoadGovernorDesc;

namespace {

constexpr double kInterval = 1.0 / 60.0;

// Feeds `seconds` of frames of `frame` seconds each; returns the changes of the factor in that time.
int feed(LoadGovernor& governor, double seconds, double frame, double cpu = 0.001, double gpu = -1.0) {
    int changes = 0;
    for (double t = 0.0; t < seconds - 1e-9; t += frame)
        if (governor.update({.frame = frame, .cpu = cpu, .gpu = gpu, .interval = kInterval})) ++changes;
    return changes;
}

// Feeds frames of `frame` seconds until the factor changes; returns the seconds that took (100 for never). The
// governor starts a new window at a change, so what follows is judged on its own.
double untilChange(LoadGovernor& governor, double frame, double cpu = 0.001, double gpu = -1.0) {
    for (double t = frame; t < 100.0; t += frame)
        if (governor.update({.frame = frame, .cpu = cpu, .gpu = gpu, .interval = kInterval})) return t;
    return 100.0;
}

bool near(float a, float b) { return std::abs(a - b) < 1e-4f; }

} // namespace

QUENCH_TEST(load_governor_off_by_default) {
    LoadGovernor governor;
    CHECK(!governor.enabled(), "off until enabled");
    CHECK(feed(governor, 5.0, 0.05) == 0, "no reaction while off");
    CHECK(governor.renderScaleFactor() == 1.0f && governor.renderScale(0.7f) == 0.7f, "the player's scale");
    CHECK(governor.changes() == 0, "%u changes", governor.changes());
}

QUENCH_TEST(load_governor_holds_while_frames_fit) {
    LoadGovernor governor;
    governor.enable({.startSeconds = 0.0});
    CHECK(feed(governor, 30.0, kInterval) == 0, "frames on time");
    // A hitch now and then is not load: 1 late frame in 20.
    int changes = 0;
    for (int i = 0; i < 1200; ++i)
        changes += governor.update({.frame = i % 20 == 0 ? 0.050 : kInterval, .cpu = 0.001, .interval = kInterval});
    CHECK(changes == 0 && governor.renderScaleFactor() == 1.0f, "%d changes for single hitches", changes);
    // A pause is no frame at all.
    CHECK(!governor.update({.frame = 3.0, .cpu = 0.001, .interval = kInterval}), "a pause");
    CHECK(feed(governor, 2.0, kInterval) == 0, "after the pause");
}

QUENCH_TEST(load_governor_steps_down_when_late) {
    LoadGovernor governor;
    int called = 0;
    governor.enable({.startSeconds = 0.0, .onChange = [&called](const LoadGovernor&) { ++called; }});
    // 45 frames per second for 60: every frame is late by less than 1.75 intervals, one step per window.
    CHECK(feed(governor, 0.9, 1.0 / 45.0) == 0, "a window is not over yet");
    CHECK(feed(governor, 0.2, 1.0 / 45.0) == 1, "one step after the first window");
    CHECK(near(governor.renderScaleFactor(), 0.85f), "x%g", double(governor.renderScaleFactor()));
    CHECK(near(governor.renderScale(0.8f), 0.68f), "the player's 0.8: %g", double(governor.renderScale(0.8f)));
    CHECK(feed(governor, 1.05, 1.0 / 45.0) == 1, "another step after the second");
    CHECK(near(governor.renderScaleFactor(), 0.85f * 0.85f), "x%g", double(governor.renderScaleFactor()));
    // 30 frames per second: two steps at once, and not below the lowest factor.
    CHECK(feed(governor, 1.05, 1.0 / 30.0) == 1, "a severe window");
    CHECK(near(governor.renderScaleFactor(), 0.85f * 0.85f * 0.85f * 0.85f), "x%g",
          double(governor.renderScaleFactor()));
    CHECK(feed(governor, 1.05, 1.0 / 30.0) == 1, "down to the lowest factor");
    CHECK(governor.renderScaleFactor() == 0.5f, "x%g", double(governor.renderScaleFactor()));
    CHECK(feed(governor, 10.0, 1.0 / 30.0) == 0, "nothing left to give");
    CHECK(governor.changes() == 4 && called == 4, "%u changes, %d calls", governor.changes(), called);
}

QUENCH_TEST(load_governor_start_is_ignored) {
    LoadGovernor governor;
    governor.enable(); // 2 s
    CHECK(feed(governor, 1.9, 1.0 / 20.0) == 0, "the first frames load and compile");
    CHECK(feed(governor, 1.0, kInterval) == 0 && governor.renderScaleFactor() == 1.0f, "then frames on time");
}

QUENCH_TEST(load_governor_cpu_bound_holds) {
    LoadGovernor governor;
    governor.enable({.startSeconds = 0.0});
    // Late frames whose main thread alone takes more than 0.8 of the interval: fewer pixels would not help.
    CHECK(feed(governor, 10.0, 1.0 / 30.0, 0.030) == 0, "the main thread is the limit");
    CHECK(governor.renderScaleFactor() == 1.0f, "x%g", double(governor.renderScaleFactor()));
    const double seconds = untilChange(governor, 1.0 / 30.0, 0.004);
    CHECK(seconds < 2.2 && governor.renderScaleFactor() < 1.0f, "the GPU is: after %g s", seconds);
}

QUENCH_TEST(load_governor_tries_up_and_backs_off) {
    LoadGovernor governor;
    governor.enable({.raiseSeconds = 10.0, .maxRaiseSeconds = 40.0, .startSeconds = 0.0});
    const double late = 1.0 / 45.0;
    const auto within = [](double seconds, double from) { return seconds > from - 0.1 && seconds < from + 1.2; };
    double seconds = untilChange(governor, late);
    CHECK(within(seconds, 1.0) && near(governor.renderScaleFactor(), 0.85f), "one step down after %g s", seconds);
    // Frames fit at 0.85: a try after 10 s without late frames.
    seconds = untilChange(governor, kInterval);
    CHECK(within(seconds, 10.0) && governor.renderScaleFactor() == 1.0f, "a step up after %g s", seconds);
    // The try brings the late frames back: down again, and the next try waits twice as long.
    seconds = untilChange(governor, late);
    CHECK(within(seconds, 1.0) && near(governor.renderScaleFactor(), 0.85f), "back down after %g s", seconds);
    CHECK(governor.raiseSeconds() == 20.0, "%g s", governor.raiseSeconds());
    seconds = untilChange(governor, kInterval);
    CHECK(within(seconds, 20.0) && governor.renderScaleFactor() == 1.0f, "the second try after %g s", seconds);
    untilChange(governor, late);
    CHECK(governor.raiseSeconds() == 40.0, "%g s", governor.raiseSeconds());
    seconds = untilChange(governor, kInterval);
    CHECK(within(seconds, 40.0), "the third try after %g s", seconds);
    untilChange(governor, late);
    CHECK(governor.raiseSeconds() == 40.0, "the wait has a ceiling: %g s", governor.raiseSeconds());
    // A try that lasts: the plain wait again.
    seconds = untilChange(governor, kInterval);
    CHECK(within(seconds, 40.0) && governor.renderScaleFactor() == 1.0f, "the fourth try after %g s", seconds);
    CHECK(feed(governor, 10.5, kInterval) == 0 && governor.raiseSeconds() == 10.0, "it lasted: %g s",
          governor.raiseSeconds());
    CHECK(governor.changes() == 8, "%u changes", governor.changes());
}

QUENCH_TEST(load_governor_gpu_time_gates_the_step_up) {
    LoadGovernor governor;
    governor.enable({.raiseSeconds = 5.0, .startSeconds = 0.0});
    untilChange(governor, 1.0 / 45.0, 0.001, 0.020);
    CHECK(near(governor.renderScaleFactor(), 0.85f), "one step down: x%g", double(governor.renderScaleFactor()));
    // 14 ms on the GPU at 0.85 would be 19.4 ms at 1: no step up, however long the frames fit.
    CHECK(feed(governor, 30.0, kInterval, 0.001, 0.014) == 0, "a step up would not fit");
    // 9 ms would be 12.5 ms: it fits.
    const double seconds = untilChange(governor, kInterval, 0.001, 0.009);
    CHECK(seconds < 6.2 && governor.renderScaleFactor() == 1.0f, "it fits: after %g s", seconds);
}

QUENCH_TEST(load_governor_new_frame_rate_starts_a_window) {
    LoadGovernor governor;
    governor.enable({.startSeconds = 0.0});
    // 30 frames per second are late for 60 and on time for 30: half a window of each is no overloaded window.
    for (int i = 0; i < 15; ++i) governor.update({.frame = 1.0 / 30.0, .cpu = 0.001, .interval = kInterval});
    int changes = 0;
    for (int i = 0; i < 300; ++i)
        changes += governor.update({.frame = 1.0 / 30.0, .cpu = 0.001, .interval = 1.0 / 30.0});
    CHECK(changes == 0 && governor.renderScaleFactor() == 1.0f, "%d changes", changes);
}

QUENCH_TEST(load_governor_disable_and_new_limits) {
    LoadGovernor governor;
    int called = 0;
    const auto count = [&called](const LoadGovernor&) { ++called; };
    governor.enable({.startSeconds = 0.0, .onChange = count});
    feed(governor, 5.0, 1.0 / 30.0);
    CHECK(governor.renderScaleFactor() == 0.5f, "x%g", double(governor.renderScaleFactor()));
    const int before = called;
    governor.enable({.minScale = 0.7f, .startSeconds = 0.0, .onChange = count});
    CHECK(governor.renderScaleFactor() == 0.7f && called == before + 1, "a higher lowest factor applies at once");
    governor.disable();
    CHECK(!governor.enabled() && governor.renderScaleFactor() == 1.0f && called == before + 2, "lifted by disable");
    governor.disable();
    CHECK(called == before + 2, "a second disable does nothing");
}
