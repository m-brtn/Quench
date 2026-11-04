#include "Test.h"

#include <quench/core/FixedStepClock.h>

#include <cmath>
#include <cstdint>

using namespace quench;

// Powers of two keep the arithmetic exact.
QUENCH_TEST(clock_steady_frames) {
    FixedStepClock clock(1.0 / 128.0);
    int steps = 0;
    for (int frame = 0; frame < 64; ++frame) steps += clock.advance(1.0 / 64.0);
    CHECK(steps == 128, "%d steps in 1 s of 64 frames", steps);
    CHECK(clock.alpha() == 0.0, "alpha %f", clock.alpha());
    CHECK(!clock.overloaded() && clock.totalDropped() == 0.0, "dropped %f", clock.totalDropped());
}

QUENCH_TEST(clock_alpha) {
    FixedStepClock clock(0.01);
    const int steps = clock.advance(0.015);
    CHECK(steps == 1, "%d steps", steps);
    CHECK(std::abs(clock.alpha() - 0.5) < 1e-9, "alpha %f", clock.alpha());
}

// A 1 s stall: clamped to 0.25 s (0.75 s dropped), 30 steps due, 12 run (18 more dropped).
QUENCH_TEST(clock_stall) {
    FixedStepClock clock(1.0 / 120.0);
    const int steps = clock.advance(1.0);
    CHECK(steps == 12, "%d steps", steps);
    CHECK(std::abs(clock.lastDropped() - (0.75 + 18.0 / 120.0)) < 1e-9, "dropped %f", clock.lastDropped());
    CHECK(clock.overloaded(), "not overloaded");
    clock.advance(1.0 / 120.0);
    CHECK(!clock.overloaded() && clock.lastDropped() == 0.0, "overload did not clear");
}

// Uneven frames give the same number of steps as their total time when nothing is dropped.
QUENCH_TEST(clock_uneven_frames) {
    FixedStepClock clock(1.0 / 120.0);
    double total = 0.0;
    int steps = 0;
    for (int frame = 0; frame < 1000; ++frame) {
        std::uint32_t x = std::uint32_t(frame) * 2654435761u + 12345u;
        x ^= x >> 13;
        const double dt = 0.004 + 0.041 * double(x % 1000) / 1000.0; // 4..45 ms
        total += dt;
        steps += clock.advance(dt);
    }
    const int expected = int(total * 120.0);
    CHECK(std::abs(steps - expected) <= 1, "%d steps, %d expected", steps, expected);
    CHECK(clock.totalDropped() == 0.0, "dropped %f", clock.totalDropped());
}

QUENCH_TEST(clock_negative_frame) {
    FixedStepClock clock;
    CHECK(clock.advance(-1.0) == 0 && clock.alpha() == 0.0, "negative frame time advanced the clock");
}
