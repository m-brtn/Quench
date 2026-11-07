#pragma once

// The Graphics panel's values (GraphicsPanel.cpp): render options set once for every section. An option set there
// replaces that option of every section's RenderSettings until it is given back ("Use section defaults"); options
// left alone keep each section's own values. Exposure is not a render setting: it scales each section's own exposure
// when the frame is rendered. Kept in settings.json under "graphics" (Settings.h).

#include <quench/core/Json.h>
#include <quench/render/Renderer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace demo {

// In the panel's order; the names in settings.json are those of the scenario command `set` (graphicsOptionKey).
enum class GraphicsOption : std::uint8_t {
    AntiAliasing,
    Sharpen,
    RenderScale,
    Upscaling,
    SunShadows,
    Cascades,
    ShadowMapSize,
    ShadowDistance,
    ShadowFilter,
    TranslucentShadows,
    ShadowRays,
    ShadowSoftness,
    AmbientOcclusion,
    OcclusionRays,
    OcclusionRadius,
    OcclusionStrength,
    ScreenSpaceFallbacks,
    LocalShadows,
    LocalShadowMaps,
    Reflections,
    ReflectionRoughness,
    IndirectLight,
    ProbeSpacing,
    ProbeUpdates,
    RayDenoiser,
    PathTracer,
    PathBounces,
    Bloom,
    BloomStrength,
    BloomThreshold,
    Sky,
    ToneCurve,
    Exposure,
    Vignette,
    Dither,
    Count,
};

inline constexpr std::size_t kGraphicsOptions = std::size_t(GraphicsOption::Count);

struct GraphicsOverrides {
    // A value per option set in the panel (enums by their index, flags 0 or 1); none: the section's own.
    std::array<std::optional<double>, kGraphicsOptions> values{};

    bool empty() const;
    std::size_t count() const;
    bool has(GraphicsOption option) const { return values[std::size_t(option)].has_value(); }
    std::optional<double> get(GraphicsOption option) const { return values[std::size_t(option)]; }
    void set(GraphicsOption option, double value); // kept in the option's range
    void reset(GraphicsOption option) { values[std::size_t(option)].reset(); }

    // A section's own settings with the values set here in their place.
    quench::render::RenderSettings apply(const quench::render::RenderSettings& own) const;
    // After a section's controls ran on apply(own) and left `edited`: an edit of an option set here becomes its value
    // here, every other edit stays the section's. Returns the section's own settings.
    quench::render::RenderSettings takeEdits(const quench::render::RenderSettings& own,
                                             const quench::render::RenderSettings& edited);
    // The factor on each section's own exposure (1 when not set).
    float exposure() const;

    friend bool operator==(const GraphicsOverrides&, const GraphicsOverrides&) = default;
};

const char* graphicsOptionKey(GraphicsOption option);  // "aa", "cascades", ...: settings.json and `set`
const char* graphicsOptionName(GraphicsOption option); // "anti-aliasing", ...: the panel and the sections' note
// A value as the panel shows it ("TAA", "on", "0.25").
std::string graphicsValueText(GraphicsOption option, double value);
// The option's value in render settings (Exposure: 1).
double graphicsValue(const quench::render::RenderSettings& settings, GraphicsOption option);

// "graphics" in settings.json: an object with the options that are set, by key; unknown keys, values of the wrong
// type and unknown names are left out. A missing or non-object value: nothing set.
GraphicsOverrides parseGraphics(const quench::json::Value& value);
quench::json::Value writeGraphics(const GraphicsOverrides& overrides);

} // namespace demo
