#pragma once

// The display's refresh rate as the cadence of the main callbacks shows it, where the system does not tell it: a
// browser calls once per refresh (requestAnimationFrame) and names no rate.

#include <array>
#include <cstddef>

namespace quench::app::detail {

// Counts the callbacks in windows of a second and keeps the highest rate of the last ten: callbacks come at the
// display's rate while the page keeps up and slower while it does not, never faster. Never below 60 Hz, which is
// what a page that has not kept up yet is taken to run on; within 4% of a common rate it is that rate.
class RefreshEstimate {
public:
    // One callback, `seconds` after the one before. Returns true when rate() changed.
    bool callback(double seconds);
    double rate() const { return m_rate; } // Hz; 60 until a window says more

private:
    static constexpr double kWindowSeconds = 1.0;
    static constexpr double kPauseSeconds = 0.1; // a longer gap is a pause (a hidden tab), no cadence
    std::array<double, 10> m_windows{};          // Hz of the last windows, 0: none yet
    std::size_t m_next = 0;
    double m_time = 0.0;
    int m_count = 0;
    double m_rate = 60.0;
};

} // namespace quench::app::detail
