#include "RefreshEstimate.h"

#include <algorithm>
#include <cmath>

namespace quench::app::detail {
namespace {

constexpr std::array<double, 9> kCommonRates{60.0, 72.0, 75.0, 90.0, 100.0, 120.0, 144.0, 165.0, 240.0};

double snapped(double hz) {
    double best = hz, distance = 0.04; // relative
    for (const double rate : kCommonRates)
        if (const double d = std::abs(hz - rate) / rate; d < distance) {
            best = rate;
            distance = d;
        }
    return best;
}

} // namespace

bool RefreshEstimate::callback(double seconds) {
    if (!(seconds > 0.0) || seconds > kPauseSeconds) {
        m_time = 0.0;
        m_count = 0;
        return false;
    }
    m_time += seconds;
    ++m_count;
    if (m_time < kWindowSeconds) return false;
    m_windows[m_next] = double(m_count) / m_time;
    m_next = (m_next + 1) % m_windows.size();
    m_time = 0.0;
    m_count = 0;
    const double rate = std::max(60.0, snapped(*std::max_element(m_windows.begin(), m_windows.end())));
    if (rate == m_rate) return false;
    m_rate = rate;
    return true;
}

} // namespace quench::app::detail
