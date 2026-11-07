#include "Test.h"

#include <quench/app/ThermalGovernor.h>

#include <string>

using namespace quench;
using app::ThermalGovernor;
using app::ThermalGovernorDesc;
using app::ThermalLimits;
using app::ThermalState;
using app::thermalStateName;

QUENCH_TEST(thermal_governor_off_by_default) {
    ThermalGovernor governor;
    CHECK(!governor.enabled(), "off until enabled");
    CHECK(!governor.update(ThermalState::Critical, 0.0), "no reaction while off");
    CHECK(governor.level() == ThermalState::Nominal, "level %s", thermalStateName(governor.level()));
    CHECK(governor.limits() == ThermalLimits{}, "no limits while off");
    CHECK(governor.frameRateLimit(0.0) == 0.0 && governor.frameRateLimit(120.0) == 120.0, "the player's frame rate");
    CHECK(governor.renderScale(0.5f) == 0.5f && !governor.reduceEffects(), "the player's scale and effects");
    CHECK(governor.changes() == 0, "%u changes", governor.changes());
}

QUENCH_TEST(thermal_governor_escalates_at_once) {
    ThermalGovernor governor;
    governor.enable();
    CHECK(governor.enabled() && governor.level() == ThermalState::Nominal, "enabled at nominal");
    CHECK(governor.update(ThermalState::Fair, 0.0), "fair is a change of level");
    CHECK(governor.limits() == ThermalLimits{}, "the defaults leave fair alone");
    CHECK(governor.update(ThermalState::Serious, 0.5), "serious at once");
    CHECK(governor.limits().maxFrameRate == 60.0 && governor.limits().renderScaleFactor == 0.8f, "serious: %g Hz, x%g",
          governor.limits().maxFrameRate, double(governor.limits().renderScaleFactor));
    CHECK(!governor.reduceEffects(), "effects stay at serious");
    CHECK(governor.update(ThermalState::Critical, 1.0), "critical at once");
    CHECK(governor.limits().maxFrameRate == 30.0 && governor.limits().renderScaleFactor == 0.6f &&
              governor.reduceEffects(),
          "critical: %g Hz, x%g", governor.limits().maxFrameRate, double(governor.limits().renderScaleFactor));
    CHECK(!governor.update(ThermalState::Critical, 2.0), "the same state is no change");
    CHECK(governor.changes() == 3, "%u changes", governor.changes());
}

QUENCH_TEST(thermal_governor_recovers_after_hold) {
    ThermalGovernor governor;
    governor.enable({.recoverSeconds = 10.0});
    governor.update(ThermalState::Critical, 0.0);
    CHECK(!governor.update(ThermalState::Nominal, 1.0), "a fall waits");
    CHECK(governor.recoveringSince() == 1.0, "recovering since %g", governor.recoveringSince());
    CHECK(!governor.update(ThermalState::Nominal, 10.9), "9.9 s lower: still critical");
    CHECK(governor.level() == ThermalState::Critical, "level %s", thermalStateName(governor.level()));
    CHECK(governor.update(ThermalState::Nominal, 11.0), "10 s lower: lifted");
    CHECK(governor.level() == ThermalState::Nominal && governor.limits() == ThermalLimits{}, "level %s",
          thermalStateName(governor.level()));
    CHECK(governor.recoveringSince() < 0.0, "not recovering any more");
}

QUENCH_TEST(thermal_governor_rise_restarts_hold) {
    ThermalGovernor governor;
    governor.enable({.recoverSeconds = 10.0});
    governor.update(ThermalState::Critical, 0.0);
    governor.update(ThermalState::Nominal, 1.0);
    governor.update(ThermalState::Critical, 5.0); // back at the level: the hold ends
    CHECK(governor.recoveringSince() < 0.0, "the hold ended at the level");
    governor.update(ThermalState::Nominal, 6.0);
    CHECK(!governor.update(ThermalState::Nominal, 15.0), "9 s since the last fall: still critical");
    CHECK(governor.update(ThermalState::Nominal, 16.0) && governor.level() == ThermalState::Nominal, "level %s",
          thermalStateName(governor.level()));
}

QUENCH_TEST(thermal_governor_recovers_to_highest_in_hold) {
    ThermalGovernor governor;
    governor.enable({.recoverSeconds = 10.0});
    governor.update(ThermalState::Critical, 0.0);
    governor.update(ThermalState::Fair, 1.0);
    governor.update(ThermalState::Nominal, 2.0);
    governor.update(ThermalState::Serious, 3.0); // lower than critical: part of the hold
    governor.update(ThermalState::Nominal, 4.0);
    CHECK(governor.update(ThermalState::Nominal, 11.0), "a step down after 10 s");
    CHECK(governor.level() == ThermalState::Serious, "down to the highest state in the hold, not to the current: %s",
          thermalStateName(governor.level()));
    CHECK(governor.recoveringSince() == 11.0, "the next step's hold starts at once: %g", governor.recoveringSince());
    CHECK(!governor.update(ThermalState::Nominal, 20.9), "still serious");
    CHECK(governor.update(ThermalState::Nominal, 21.0) && governor.level() == ThermalState::Nominal, "level %s",
          thermalStateName(governor.level()));
}

QUENCH_TEST(thermal_governor_player_choice_first) {
    ThermalGovernor governor;
    governor.enable({.recoverSeconds = 1.0});
    governor.update(ThermalState::Serious, 0.0);
    CHECK(governor.frameRateLimit(0.0) == 60.0, "the display's rate capped: %g", governor.frameRateLimit(0.0));
    CHECK(governor.frameRateLimit(120.0) == 60.0, "120 capped: %g", governor.frameRateLimit(120.0));
    CHECK(governor.frameRateLimit(30.0) == 30.0, "a lower choice is never raised: %g", governor.frameRateLimit(30.0));
    CHECK(governor.renderScale(0.5f) == 0.5f * 0.8f, "scale %g", double(governor.renderScale(0.5f)));
    governor.update(ThermalState::Nominal, 1.0);
    governor.update(ThermalState::Nominal, 2.0);
    CHECK(governor.level() == ThermalState::Nominal, "lifted");
    CHECK(governor.frameRateLimit(0.0) == 0.0 && governor.frameRateLimit(120.0) == 120.0, "the player's rate back");
    CHECK(governor.renderScale(0.5f) == 0.5f && governor.renderScale(0.67f) == 0.67f, "the player's scale exactly");
}

QUENCH_TEST(thermal_governor_unknown_counts_as_nominal) {
    ThermalGovernor governor;
    governor.enable({.recoverSeconds = 2.0});
    CHECK(!governor.update(ThermalState::Unknown, 0.0), "unknown: no change");
    CHECK(governor.level() == ThermalState::Nominal && governor.limits() == ThermalLimits{}, "nominal");
    governor.update(ThermalState::Serious, 1.0);
    governor.update(ThermalState::Unknown, 2.0);
    CHECK(governor.level() == ThermalState::Serious, "unknown is a fall, held like one");
    CHECK(governor.update(ThermalState::Unknown, 4.0) && governor.level() == ThermalState::Nominal, "level %s",
          thermalStateName(governor.level()));
}

QUENCH_TEST(thermal_governor_limits_accumulate) {
    ThermalGovernor governor;
    governor.enable({.fair = {.maxFrameRate = 90.0},
                     .serious = {.renderScaleFactor = 0.9f},
                     .critical = {.maxFrameRate = 100.0, .renderScaleFactor = 0.95f, .reduceEffects = true}});
    governor.update(ThermalState::Serious, 0.0);
    CHECK(governor.limits().maxFrameRate == 90.0 && governor.limits().renderScaleFactor == 0.9f,
          "serious keeps fair's 90 Hz: %g Hz, x%g", governor.limits().maxFrameRate,
          double(governor.limits().renderScaleFactor));
    governor.update(ThermalState::Critical, 1.0);
    CHECK(governor.limits().maxFrameRate == 90.0 && governor.limits().renderScaleFactor == 0.9f &&
              governor.reduceEffects(),
          "critical never less strict than below it: %g Hz, x%g", governor.limits().maxFrameRate,
          double(governor.limits().renderScaleFactor));
}

QUENCH_TEST(thermal_governor_disable_and_callbacks) {
    ThermalGovernor governor;
    int calls = 0;
    ThermalState seen = ThermalState::Unknown;
    const auto onChange = [&](const ThermalGovernor& g) {
        ++calls;
        seen = g.level();
    };
    governor.enable({.onChange = onChange});
    CHECK(calls == 0, "enabling at nominal changes nothing: %d calls", calls);
    governor.update(ThermalState::Serious, 0.0);
    CHECK(calls == 1 && seen == ThermalState::Serious, "%d calls, %s", calls, thermalStateName(seen));
    governor.disable();
    CHECK(calls == 2 && seen == ThermalState::Nominal, "disable lifts the limits: %d calls, %s", calls,
          thermalStateName(seen));
    CHECK(!governor.enabled() && governor.limits() == ThermalLimits{} && governor.frameRateLimit(0.0) == 0.0,
          "off: nothing limited");
    governor.disable();
    CHECK(calls == 2, "a second disable changes nothing: %d calls", calls);
    governor.enable({.onChange = onChange});
    governor.update(ThermalState::Serious, 1.0);
    CHECK(calls == 3, "%d calls", calls);
    // New limits for the level already reached apply at once.
    governor.enable({.serious = {.maxFrameRate = 40.0}, .onChange = onChange});
    CHECK(calls == 4 && governor.level() == ThermalState::Serious && governor.limits().maxFrameRate == 40.0 &&
              governor.limits().renderScaleFactor == 1.0f,
          "%d calls, %g Hz", calls, governor.limits().maxFrameRate);
    governor.enable({.serious = {.maxFrameRate = 40.0}, .onChange = onChange});
    CHECK(calls == 4, "the same limits again: %d calls", calls);
}

QUENCH_TEST(thermal_limits_text) {
    CHECK(app::thermalLimitsText({}) == "nothing", "%s", app::thermalLimitsText({}).c_str());
    const std::string serious = app::thermalLimitsText({.maxFrameRate = 60.0, .renderScaleFactor = 0.8f});
    CHECK(serious == "at most 60 Hz, render scale x0.80", "%s", serious.c_str());
    const std::string critical =
        app::thermalLimitsText({.maxFrameRate = 30.0, .renderScaleFactor = 0.6f, .reduceEffects = true});
    CHECK(critical == "at most 30 Hz, render scale x0.60, effects reduced", "%s", critical.c_str());
}
