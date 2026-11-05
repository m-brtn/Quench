#pragma once

// The display's HDR headroom while the app runs. SDL refreshes its window property (SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT)
// only on display events (SDL 3.4.18 on iOS: once, when it adds the display), so neither a brightness change nor the
// rise of the headroom while HDR content is on screen reaches it. Where the platform has a query
// (DisplayHeadroomApple.m: UIScreen, NSScreen), the app reads it every frame.

#include <optional>

struct SDL_Window;

namespace quench::app::detail {

struct DisplayHeadroom {
    float current = 1.0f;   // what the display shows now: output values above it are clipped
    float potential = 1.0f; // the most it can show (HDR content on screen, brightness permitting)
    bool operator==(const DisplayHeadroom&) const = default;
};

// The platform's values for the display `window` is on; nullopt where there is no query.
std::optional<DisplayHeadroom> queryDisplayHeadroom(SDL_Window* window);

// The values the app uses: the platform's where it has them, else SDL's property for both (SDL reports the current
// headroom when it is above 1, else the potential one). Both at least 1, potential at least current.
DisplayHeadroom resolveDisplayHeadroom(const std::optional<DisplayHeadroom>& platform, float sdl);

// Rate limit of the log line for a headroom change: a brightness change ramps the headroom over many frames.
class HeadroomLog {
public:
    static constexpr double kInterval = 1.0; // seconds between lines

    // True when a line with `headroom` is due at `now` (seconds): it differs by more than 1% from the last value
    // logged, and the last line is at least kInterval old. Checked every frame, so the value a ramp settles on is
    // logged too.
    bool due(float headroom, double now);
    // `headroom` was logged at `now` by another line (the display output line).
    void logged(float headroom, double now);

private:
    float m_value = 0.0f;
    double m_time = -1.0e9;
};

} // namespace quench::app::detail
