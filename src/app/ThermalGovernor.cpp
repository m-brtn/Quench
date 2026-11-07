#include <quench/app/ThermalGovernor.h>

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace quench::app {
namespace {

void sanitize(ThermalLimits& limits) {
    QUENCH_ASSERT(limits.renderScaleFactor > 0.0f && limits.renderScaleFactor <= 1.0f,
                  "ThermalLimits::renderScaleFactor must be in (0, 1]");
    QUENCH_ASSERT(limits.maxFrameRate >= 0.0, "ThermalLimits::maxFrameRate must not be negative");
    limits.renderScaleFactor =
        std::isfinite(limits.renderScaleFactor) ? std::clamp(limits.renderScaleFactor, 0.05f, 1.0f) : 1.0f;
    limits.maxFrameRate = std::isfinite(limits.maxFrameRate) ? std::clamp(limits.maxFrameRate, 0.0, 1000.0) : 0.0;
}

} // namespace

std::string thermalLimitsText(const ThermalLimits& limits) {
    std::string text;
    const auto add = [&text](const std::string& part) { text += (text.empty() ? "" : ", ") + part; };
    if (limits.maxFrameRate > 0.0) add(std::format("at most {} Hz", limits.maxFrameRate));
    if (limits.renderScaleFactor < 1.0f) add(std::format("render scale x{:.2f}", limits.renderScaleFactor));
    if (limits.reduceEffects) add("effects reduced");
    return text.empty() ? "nothing" : text;
}

void ThermalGovernor::enable(ThermalGovernorDesc desc) {
    sanitize(desc.fair);
    sanitize(desc.serious);
    sanitize(desc.critical);
    desc.recoverSeconds = std::isfinite(desc.recoverSeconds) ? std::max(desc.recoverSeconds, 0.0) : 0.0;
    m_desc = std::move(desc);
    log::info("app", "thermal reaction on: fair {}; serious {}; critical {}; lifted after {} s lower",
              thermalLimitsText(limitsAt(ThermalState::Fair)), thermalLimitsText(limitsAt(ThermalState::Serious)),
              thermalLimitsText(limitsAt(ThermalState::Critical)), m_desc.recoverSeconds);
    if (!m_enabled) {
        m_enabled = true;
        m_level = ThermalState::Nominal;
        m_limits = {};
        m_lowerSince = -1.0;
        return;
    }
    // New limits for the level already reached.
    if (const ThermalLimits limits = limitsAt(m_level); limits != m_limits) {
        m_limits = limits;
        ++m_changes;
        log::info("app", "thermal reaction at {}: {}", thermalStateName(m_level), thermalLimitsText(m_limits));
        if (m_desc.onChange) m_desc.onChange(*this);
    }
}

void ThermalGovernor::disable() {
    if (!m_enabled) return;
    m_enabled = false;
    m_lowerSince = -1.0;
    const bool changed = m_level != ThermalState::Nominal;
    log::info("app", "thermal reaction off{}",
              changed ? std::format(" (lifted at {})", thermalStateName(m_level)) : "");
    m_level = ThermalState::Nominal;
    m_limits = {};
    if (changed) {
        ++m_changes;
        if (m_desc.onChange) m_desc.onChange(*this);
    }
}

bool ThermalGovernor::update(ThermalState reported, double seconds) {
    if (!m_enabled) return false;
    const ThermalState state = std::max(reported, ThermalState::Nominal);
    if (state > m_level) {
        apply(state, "");
        m_lowerSince = -1.0;
        return true;
    }
    if (state == m_level) {
        m_lowerSince = -1.0;
        return false;
    }
    if (m_lowerSince < 0.0) {
        m_lowerSince = seconds;
        m_lowerMax = state;
        return false;
    }
    m_lowerMax = std::max(m_lowerMax, state);
    if (seconds - m_lowerSince < m_desc.recoverSeconds) return false;
    const ThermalState target = m_lowerMax;
    apply(target, std::format(" after {:.0f} s lower", seconds - m_lowerSince));
    // Still lower than the level just reached: the next step down needs a time of its own.
    m_lowerSince = state < target ? seconds : -1.0;
    m_lowerMax = state;
    return true;
}

double ThermalGovernor::frameRateLimit(double playerHz) const {
    const double cap = m_limits.maxFrameRate;
    if (cap <= 0.0) return playerHz;
    return playerHz > 0.0 ? std::min(playerHz, cap) : cap;
}

ThermalLimits ThermalGovernor::limitsAt(ThermalState level) const {
    ThermalLimits limits;
    const ThermalLimits* steps[] = {&m_desc.fair, &m_desc.serious, &m_desc.critical};
    for (int i = 0; i < int(level) && i < 3; ++i) {
        const ThermalLimits& step = *steps[i];
        if (step.maxFrameRate > 0.0)
            limits.maxFrameRate =
                limits.maxFrameRate > 0.0 ? std::min(limits.maxFrameRate, step.maxFrameRate) : step.maxFrameRate;
        limits.renderScaleFactor = std::min(limits.renderScaleFactor, step.renderScaleFactor);
        limits.reduceEffects = limits.reduceEffects || step.reduceEffects;
    }
    return limits;
}

void ThermalGovernor::apply(ThermalState level, std::string_view why) {
    log::info("app", "thermal reaction {} -> {}{}: {}", thermalStateName(m_level), thermalStateName(level), why,
              thermalLimitsText(limitsAt(level)));
    m_level = level;
    m_limits = limitsAt(level);
    ++m_changes;
    if (m_desc.onChange) m_desc.onChange(*this);
}

} // namespace quench::app
