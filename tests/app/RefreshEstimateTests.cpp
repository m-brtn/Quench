#include "Test.h"

#include "app/RefreshEstimate.h"

using namespace quench;
using app::detail::RefreshEstimate;

namespace {

// Callbacks for `seconds` at `hz`, every second interval longer and shorter by `jitter` seconds.
void run(RefreshEstimate& estimate, double seconds, double hz, double jitter = 0.0) {
    const int count = int(seconds * hz);
    for (int i = 0; i < count; ++i) estimate.callback(1.0 / hz + (i % 2 == 0 ? jitter : -jitter));
}

} // namespace

QUENCH_TEST(refresh_estimate_follows_the_cadence) {
    RefreshEstimate estimate;
    CHECK(estimate.rate() == 60.0, "60 Hz until the callbacks say more: %g", estimate.rate());
    run(estimate, 3.0, 60.0, 0.002);
    CHECK(estimate.rate() == 60.0, "60 Hz with jitter: %g", estimate.rate());
    run(estimate, 2.0, 119.0, 0.001);
    CHECK(estimate.rate() == 120.0, "a common rate nearby: %g", estimate.rate());
    // A page that does not keep up is called less often: the display is still what it was.
    run(estimate, 5.0, 45.0);
    CHECK(estimate.rate() == 120.0, "slower callbacks: %g", estimate.rate());
    // Ten windows later the faster ones are forgotten (another display), and nothing is below 60 Hz.
    run(estimate, 11.0, 45.0);
    CHECK(estimate.rate() == 60.0, "at least 60 Hz: %g", estimate.rate());
    run(estimate, 2.0, 90.0);
    CHECK(estimate.rate() == 90.0, "%g", estimate.rate());
    RefreshEstimate odd;
    run(odd, 2.0, 83.0);
    CHECK(odd.rate() > 82.0 && odd.rate() < 84.0, "no common rate nearby: %g", odd.rate());
}

QUENCH_TEST(refresh_estimate_ignores_pauses) {
    RefreshEstimate estimate;
    run(estimate, 0.5, 120.0);
    CHECK(!estimate.callback(5.0), "a hidden tab");
    run(estimate, 0.9, 120.0);
    CHECK(estimate.rate() == 60.0, "no whole window yet: %g", estimate.rate());
    run(estimate, 0.2, 120.0);
    CHECK(estimate.rate() == 120.0, "%g", estimate.rate());
}
