#pragma once

// The operating system's thermal state and an opt-in reaction to it (docs/app.md, "Thermal state"). The engine reads
// and reports the state; it changes nothing by itself. A game that wants to react enables App::thermalGovernor() with
// the limits it allows per state: the App applies the frame rate limit, the game applies the render scale and its
// effects to its own settings.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace quench::app {

// The operating system's thermal state (Apple platforms: NSProcessInfo's thermalState; Unknown elsewhere). Fair and
// above: the system lowers clocks to cool down, so frame times grow without a change in the work.
enum class ThermalState { Unknown = -1, Nominal = 0, Fair = 1, Serious = 2, Critical = 3 };
const char* thermalStateName(ThermalState state); // "unknown", "nominal", "fair", "serious", "critical"

// What a game allows from one thermal state on.
struct ThermalLimits {
    double maxFrameRate = 0.0;      // at most this many frames per second; 0: no limit
    float renderScaleFactor = 1.0f; // (0, 1]: multiplies the render scale the player chose
    bool reduceEffects = false;     // the game turns its expensive effects down (which ones is its choice)
    friend bool operator==(const ThermalLimits&, const ThermalLimits&) = default;
};

// "at most 60 Hz, render scale x0.80, effects reduced"; "nothing" for no limits.
std::string thermalLimitsText(const ThermalLimits& limits);

class ThermalGovernor;

struct ThermalGovernorDesc {
    // The limits from each state on. A state keeps the limits of the states below it, so the strictest of each applies
    // (a lower frame rate, a smaller factor). Nominal has none. The defaults leave fair alone (phones hold their frames
    // there for minutes, docs/performance.md) and halve a 120 Hz phone's frame rate from serious on.
    ThermalLimits fair{};
    ThermalLimits serious{.maxFrameRate = 60.0, .renderScaleFactor = 0.8f};
    ThermalLimits critical{.maxFrameRate = 30.0, .renderScaleFactor = 0.6f, .reduceEffects = true};
    // App time a lower state must last before the limits are lifted, to the highest state reported in that time; a
    // higher state applies at once. The system's state itself changes slowly (minutes on the iPhone 17).
    double recoverSeconds = 60.0;
    // Called after every change of the level or the limits, also by enable() and disable() when they change them.
    std::function<void(const ThermalGovernor&)> onChange;
};

// The opt-in reaction to the thermal state, off until enable(). The App owns one (App::thermalGovernor()) and feeds it
// FrameStats::thermal every frame before Game::update. The player's choices stay the game's: frameRateLimit() and
// renderScale() take them and return the lower value, so a limit never raises what the player set, and the player's
// value comes back exactly when the limits are lifted. An unknown state (no platform query) counts as nominal.
class ThermalGovernor {
public:
    void enable(ThermalGovernorDesc desc = {});
    void disable();
    bool enabled() const { return m_enabled; }
    const ThermalGovernorDesc& desc() const { return m_desc; }

    // The state reported at app time `seconds` (not decreasing). Returns true when level() changed (after a log line
    // and onChange). Nothing happens while off.
    bool update(ThermalState reported, double seconds);

    // The state whose limits apply: nominal while off; it follows a rise at once and a fall after recoverSeconds.
    ThermalState level() const { return m_level; }
    const ThermalLimits& limits() const { return m_limits; }
    // The frame rate to run at for the player's (0: the display's rate): the lower of the two; 0 for no limit.
    double frameRateLimit(double playerHz) const;
    // The render scale to render at for the player's.
    float renderScale(float playerScale) const { return playerScale * m_limits.renderScaleFactor; }
    bool reduceEffects() const { return m_limits.reduceEffects; }
    // App time since which a lower state has lasted (the limits are lifted at recoveringSince() + recoverSeconds);
    // negative while not recovering.
    double recoveringSince() const { return m_lowerSince; }
    std::uint32_t changes() const { return m_changes; } // changes of level() or limits() since the start

private:
    ThermalLimits limitsAt(ThermalState level) const;
    void apply(ThermalState level, std::string_view why);

    ThermalGovernorDesc m_desc;
    bool m_enabled = false;
    ThermalState m_level = ThermalState::Nominal;
    ThermalLimits m_limits;
    double m_lowerSince = -1.0;
    ThermalState m_lowerMax = ThermalState::Nominal; // the highest state reported while recovering
    std::uint32_t m_changes = 0;
};

} // namespace quench::app
