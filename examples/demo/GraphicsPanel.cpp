// The Graphics panel: the render options of RenderSettings and the app's display options in one window that works in
// every section (in the wide layout a window between the menu and the section's panel, in the compact layout a sheet
// docked as the controls are). A value changed here replaces that option in every section until it is given back
// (Reset, or Use section defaults for all); under an option a line says what runs instead after the device's
// fallbacks and why (Renderer::effects, Renderer::fallbacks). The values are kept in settings.json (GraphicsPanel.h).
// docs/demo.md, "Graphics panel".

#include "GraphicsPanel.h"

#include "DemoGame.h"

#include <quench/core/Log.h>

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace demo {

using namespace quench;
using app::App;
using render::RenderSettings;

namespace {

enum class Kind : std::uint8_t { Number, Integer, Flag, Choice };

struct OptionInfo {
    GraphicsOption option;
    const char* key;   // settings.json and the scenario command `set`
    const char* name;  // in the sections' note
    const char* label; // in the panel
    Kind kind;
    double min; // the range a value is kept in (Choice: the index, Flag: 0 or 1)
    double max;
    std::array<const char*, 4> choices{}; // Choice: the names in settings.json, by value
    std::array<const char*, 4> labels{};  // Choice: the names in the panel
};

using O = GraphicsOption;

constexpr std::array<OptionInfo, kGraphicsOptions> kOptions{{
    {O::AntiAliasing,
     "aa",
     "anti-aliasing",
     "Anti-aliasing",
     Kind::Choice,
     0,
     2,
     {"none", "fxaa", "taa"},
     {"None", "FXAA", "TAA"}},
    {O::Sharpen, "sharpen", "sharpening", "Sharpen", Kind::Number, 0, 1},
    {O::RenderScale, "scale", "render scale", "Render scale", Kind::Number, 0.25, 1},
    {O::Upscaling,
     "upscaling",
     "upscaling",
     "Upscaling",
     Kind::Choice,
     0,
     3,
     {"bilinear", "spatial", "temporal", "denoised"},
     {"Bilinear", "MetalFX spatial", "MetalFX temporal", "MetalFX denoising"}},
    {O::SunShadows,
     "sun_shadows",
     "sun shadows",
     "Sun shadows",
     Kind::Choice,
     0,
     1,
     {"maps", "rt"},
     {"Shadow maps", "Ray traced"}},
    {O::Cascades, "cascades", "cascades", "Cascades", Kind::Integer, 0, render::kMaxCascades},
    {O::ShadowMapSize, "shadow_size", "shadow map size", "Map size", Kind::Integer, 256, 4096},
    {O::ShadowDistance, "shadow_distance", "shadow distance", "Distance", Kind::Number, 1, 1000},
    {O::ShadowFilter, "shadow_filter", "shadow filter", "Filter", Kind::Integer, 0, 2},
    {O::TranslucentShadows,
     "translucent_shadows",
     "translucent shadows",
     "Translucent",
     Kind::Choice,
     0,
     1,
     {"map", "dithered"},
     {"Transmittance map", "Dithered"}},
    {O::ShadowRays, "shadow_rays", "shadow rays", "Shadow rays", Kind::Integer, 1, 8},
    {O::ShadowSoftness, "shadow_softness", "shadow softness", "Softness", Kind::Number, 0, 16},
    {O::AmbientOcclusion,
     "ao",
     "ambient occlusion",
     "Occlusion",
     Kind::Choice,
     0,
     2,
     {"none", "rt", "screen"},
     {"None", "Ray traced", "Screen space (GTAO)"}},
    {O::OcclusionRays, "ao_rays", "occlusion rays", "AO rays", Kind::Integer, 1, 8},
    {O::OcclusionRadius, "ao_radius", "occlusion radius", "AO radius", Kind::Number, 0.05, 10},
    {O::OcclusionStrength, "ao_strength", "occlusion strength", "AO strength", Kind::Number, 0, 1},
    {O::ScreenSpaceFallbacks, "screen_fallbacks", "screen-space fallbacks", "Screen-space fallbacks", Kind::Flag, 0, 1},
    {O::LocalShadows, "local_shadows", "local light shadows", "Local shadows", Kind::Integer, 0,
     render::kMaxRayLocalShadows},
    // RenderSettings::localShadowMapSize: off, 256, 512 or 1024.
    {O::LocalShadowMaps,
     "local_shadow_maps",
     "local light shadow maps",
     "Shadow maps",
     Kind::Choice,
     0,
     3,
     {"off", "256", "512", "1024"},
     {"Off (unshadowed)", "256", "512", "1024"}},
    {O::Reflections,
     "reflections",
     "reflections",
     "Reflections",
     Kind::Choice,
     0,
     2,
     {"sky", "rt", "screen"},
     {"Sky", "Ray traced", "Screen space (SSR)"}},
    {O::ReflectionRoughness, "reflection_roughness", "reflection roughness", "Up to roughness", Kind::Number, 0, 1},
    {O::IndirectLight,
     "indirect",
     "indirect light",
     "Indirect light",
     Kind::Choice,
     0,
     2,
     {"sky", "probes", "rt"},
     {"Sky", "Probe grid", "Ray traced"}},
    {O::ProbeSpacing, "probe_spacing", "probe spacing", "Probe spacing", Kind::Number, 0.25, 16},
    // The index of RenderSettings::probeUpdatePeriod 1, 2, 4 or 8.
    {O::ProbeUpdates,
     "probe_updates",
     "probe updates",
     "Probe updates",
     Kind::Choice,
     0,
     3,
     {"1", "2", "4", "8"},
     {"Every probe per frame", "1/2 per frame", "1/4 per frame", "1/8 per frame"}},
    {O::RayDenoiser,
     "denoiser",
     "ray denoiser",
     "Denoiser",
     Kind::Choice,
     0,
     1,
     {"spatial", "temporal"},
     {"Spatial", "Temporal"}},
    {O::PathTracer, "path_tracer", "path tracer", "Path tracer", Kind::Flag, 0, 1},
    {O::PathBounces, "path_bounces", "path bounces", "Bounces", Kind::Integer, 1, 16},
    {O::Bloom, "bloom", "bloom", "Bloom", Kind::Flag, 0, 1},
    {O::BloomStrength, "bloom_strength", "bloom strength", "Strength", Kind::Number, 0, 1},
    {O::BloomThreshold, "bloom_threshold", "bloom threshold", "Threshold", Kind::Number, 0, 16},
    {O::Sky, "sky", "sky", "Sky", Kind::Flag, 0, 1},
    {O::ToneCurve, "tone", "tone curve", "Tone curve", Kind::Choice, 0, 1, {"aces", "agx"}, {"ACES", "AgX"}},
    {O::Exposure, "exposure", "exposure", "Exposure", Kind::Number, 0.05, 16},
    {O::Vignette, "vignette", "vignette", "Vignette", Kind::Number, 0, 1},
    {O::Dither, "dither", "dither", "Dither", Kind::Flag, 0, 1},
}};

constexpr bool optionsInOrder() {
    for (std::size_t i = 0; i < kOptions.size(); ++i)
        if (std::size_t(kOptions[i].option) != i) return false;
    return true;
}
static_assert(optionsInOrder(), "kOptions lists the options in the order of GraphicsOption");

const OptionInfo& info(GraphicsOption option) { return kOptions[std::size_t(option)]; }

void setValue(RenderSettings& s, GraphicsOption option, double v) {
    switch (option) {
    case O::AntiAliasing: s.antiAliasing = render::AntiAliasing(int(v)); break;
    case O::Sharpen: s.sharpen = float(v); break;
    case O::RenderScale: s.renderScale = float(v); break;
    case O::Upscaling: s.upscaling = render::Upscaling(int(v)); break;
    case O::SunShadows: s.sunShadows = render::SunShadows(int(v)); break;
    case O::Cascades: s.shadowCascades = int(v); break;
    case O::ShadowMapSize: s.shadowMapSize = std::uint32_t(v); break;
    case O::ShadowDistance: s.shadowDistance = float(v); break;
    case O::ShadowFilter: s.shadowFilter = int(v); break;
    case O::TranslucentShadows: s.translucentShadows = render::TranslucentShadows(int(v)); break;
    case O::ShadowRays: s.shadowRays = int(v); break;
    case O::ShadowSoftness: s.shadowSoftness = float(v); break;
    case O::AmbientOcclusion: s.ambientOcclusion = render::AmbientOcclusion(int(v)); break;
    case O::OcclusionRays: s.occlusionRays = int(v); break;
    case O::OcclusionRadius: s.occlusionRadius = float(v); break;
    case O::OcclusionStrength: s.occlusionStrength = float(v); break;
    case O::ScreenSpaceFallbacks: s.screenSpaceFallbacks = v != 0.0; break;
    case O::LocalShadows: s.localLightShadows = int(v); break;
    case O::LocalShadowMaps: s.localShadowMapSize = v < 0.5 ? 0u : 128u << std::clamp(int(v), 1, 3); break;
    case O::Reflections: s.reflections = render::Reflections(int(v)); break;
    case O::ReflectionRoughness: s.reflectionRoughness = float(v); break;
    case O::IndirectLight: s.indirectLight = render::IndirectLight(int(v)); break;
    case O::ProbeSpacing: s.probeSpacing = float(v); break;
    case O::ProbeUpdates: s.probeUpdatePeriod = 1 << std::clamp(int(v), 0, 3); break;
    case O::RayDenoiser: s.rayDenoiser = render::RayDenoiser(int(v)); break;
    case O::PathTracer: s.pathTracer = v != 0.0; break;
    case O::PathBounces: s.pathBounces = int(v); break;
    case O::Bloom: s.bloom = v != 0.0; break;
    case O::BloomStrength: s.bloomStrength = float(v); break;
    case O::BloomThreshold: s.bloomThreshold = float(v); break;
    case O::Sky: s.sky = v != 0.0; break;
    case O::ToneCurve: s.toneCurve = render::ToneCurve(int(v)); break;
    case O::Exposure: break; // the scene's (DemoGame::render)
    case O::Vignette: s.vignette = float(v); break;
    case O::Dither: s.dither = v != 0.0; break;
    case O::Count: break;
    }
}

gpu::PresentMode presentModeOf(std::string_view name) {
    return name == "immediate" ? gpu::PresentMode::Immediate
           : name == "mailbox" ? gpu::PresentMode::Mailbox
                               : gpu::PresentMode::Vsync;
}

constexpr ImU32 kSetColor = IM_COL32(120, 200, 255, 255);     // an option set in the panel
constexpr ImU32 kFallbackColor = IM_COL32(255, 205, 90, 255); // something else runs (as the log's warnings)

constexpr ImGuiWindowFlags kSheetFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                         ImGuiWindowFlags_NoSavedSettings;

// A line under an option: what runs instead and why (`fallback`), or how the option works now.
void note(const std::string& text, bool fallback) {
    ImGui::Indent();
    ImGui::PushStyleColor(ImGuiCol_Text, fallback ? kFallbackColor : ImGui::GetColorU32(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
    ImGui::Unindent();
}

// The option rows: in the window a column of widgets and one of names; in the sheet the name over a widget as wide
// as the sheet, for fingers. A name in kSetColor with a Reset button: the panel sets that option.
class Rows {
public:
    Rows(GraphicsOverrides& overrides, const RenderSettings& own, bool stacked, bool large, bool locked)
        : m_overrides(overrides), m_own(own), m_stacked(stacked), m_large(large), m_locked(locked),
          m_width(11.0f * ImGui::GetFontSize()) {}

    // `widget` draws the option's control on its value in use and returns the new value when it was changed.
    template <typename Widget>
    void row(GraphicsOption option, Widget&& widget) {
        ImGui::PushID(int(option));
        ImGui::BeginDisabled(m_locked);
        std::optional<double> edited;
        if (m_stacked) {
            ImGui::AlignTextToFramePadding();
            name(option);
            ImGui::SetNextItemWidth(-FLT_MIN);
            edited = widget();
        } else {
            const float x = ImGui::GetCursorPosX();
            ImGui::SetNextItemWidth(m_width);
            edited = widget();
            ImGui::SameLine(x + m_width + ImGui::GetStyle().ItemInnerSpacing.x);
            name(option);
        }
        ImGui::EndDisabled();
        if (edited) m_overrides.set(option, *edited);
        ImGui::PopID();
    }

private:
    void name(GraphicsOption option) {
        const bool set = m_overrides.has(option);
        if (set) ImGui::PushStyleColor(ImGuiCol_Text, kSetColor);
        ImGui::TextUnformatted(info(option).label);
        if (!set) return;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (m_large ? ImGui::Button("Reset") : ImGui::SmallButton("Reset")) m_overrides.reset(option);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Back to the section's value: %s",
                              graphicsValueText(option, graphicsValue(m_own, option)).c_str());
    }

    GraphicsOverrides& m_overrides;
    const RenderSettings& m_own;
    bool m_stacked, m_large, m_locked;
    float m_width;
};

std::optional<double> combo(GraphicsOption option, int value) {
    const OptionInfo& o = info(option);
    const int count = int(std::ranges::count_if(o.labels, [](const char* label) { return label != nullptr; }));
    if (ImGui::Combo("##value", &value, o.labels.data(), count)) return double(value);
    return std::nullopt;
}

std::optional<double> slider(float value, float min, float max, const char* format, ImGuiSliderFlags flags = 0) {
    if (ImGui::SliderFloat("##value", &value, min, max, format, flags)) return double(value);
    return std::nullopt;
}

std::optional<double> sliderInt(int value, int min, int max, const char* format = "%d") {
    if (ImGui::SliderInt("##value", &value, min, max, format)) return double(value);
    return std::nullopt;
}

std::optional<double> checkbox(bool value) {
    if (ImGui::Checkbox("##value", &value)) return value ? 1.0 : 0.0;
    return std::nullopt;
}

} // namespace

// ---- the values

bool GraphicsOverrides::empty() const { return count() == 0; }

std::size_t GraphicsOverrides::count() const {
    return std::size_t(std::ranges::count_if(values, [](const std::optional<double>& v) { return v.has_value(); }));
}

void GraphicsOverrides::set(GraphicsOption option, double value) {
    const OptionInfo& o = info(option);
    if (!std::isfinite(value)) return;
    if (o.kind != Kind::Number) value = std::round(value);
    values[std::size_t(option)] = std::clamp(value, o.min, o.max);
}

RenderSettings GraphicsOverrides::apply(const RenderSettings& own) const {
    RenderSettings s = own;
    for (std::size_t i = 0; i < kGraphicsOptions; ++i)
        if (values[i]) setValue(s, GraphicsOption(i), *values[i]);
    return s;
}

RenderSettings GraphicsOverrides::takeEdits(const RenderSettings& own, const RenderSettings& edited) {
    const RenderSettings shown = apply(own);
    RenderSettings result = edited;
    for (std::size_t i = 0; i < kGraphicsOptions; ++i) {
        if (!values[i]) continue;
        const auto option = GraphicsOption(i);
        if (const double v = graphicsValue(edited, option); v != graphicsValue(shown, option)) set(option, v);
        setValue(result, option, graphicsValue(own, option));
    }
    return result;
}

float GraphicsOverrides::exposure() const { return float(get(GraphicsOption::Exposure).value_or(1.0)); }

const char* graphicsOptionKey(GraphicsOption option) { return info(option).key; }
const char* graphicsOptionName(GraphicsOption option) { return info(option).name; }

std::string graphicsValueText(GraphicsOption option, double value) {
    const OptionInfo& o = info(option);
    switch (o.kind) {
    case Kind::Choice: {
        const char* label = o.labels[std::size_t(std::clamp(int(value), 0, int(o.labels.size()) - 1))];
        return label ? label : "?";
    }
    case Kind::Flag: return value != 0.0 ? "on" : "off";
    case Kind::Integer:
        if (option == O::ShadowFilter) return value <= 0.0 ? "1 tap" : value <= 1.0 ? "4 taps" : "12 taps";
        return std::format("{}", int(value));
    case Kind::Number: return option == O::Exposure ? std::format("x {:.2f}", value) : std::format("{:.2f}", value);
    }
    return {};
}

double graphicsValue(const RenderSettings& s, GraphicsOption option) {
    switch (option) {
    case O::AntiAliasing: return double(int(s.antiAliasing));
    case O::Sharpen: return double(s.sharpen);
    case O::RenderScale: return double(s.renderScale);
    case O::Upscaling: return double(int(s.upscaling));
    case O::SunShadows: return double(int(s.sunShadows));
    case O::Cascades: return double(s.shadowCascades);
    case O::ShadowMapSize: return double(s.shadowMapSize);
    case O::ShadowDistance: return double(s.shadowDistance);
    case O::ShadowFilter: return double(s.shadowFilter);
    case O::TranslucentShadows: return double(int(s.translucentShadows));
    case O::ShadowRays: return double(s.shadowRays);
    case O::ShadowSoftness: return double(s.shadowSoftness);
    case O::AmbientOcclusion: return double(int(s.ambientOcclusion));
    case O::OcclusionRays: return double(s.occlusionRays);
    case O::OcclusionRadius: return double(s.occlusionRadius);
    case O::OcclusionStrength: return double(s.occlusionStrength);
    case O::ScreenSpaceFallbacks: return s.screenSpaceFallbacks ? 1.0 : 0.0;
    case O::LocalShadows: return double(s.localLightShadows);
    case O::LocalShadowMaps:
        return s.localShadowMapSize >= 1024  ? 3.0
               : s.localShadowMapSize >= 512 ? 2.0
               : s.localShadowMapSize > 0    ? 1.0
                                             : 0.0;
    case O::Reflections: return double(int(s.reflections));
    case O::ReflectionRoughness: return double(s.reflectionRoughness);
    case O::IndirectLight: return double(int(s.indirectLight));
    case O::ProbeSpacing: return double(s.probeSpacing);
    case O::ProbeUpdates:
        return s.probeUpdatePeriod >= 8 ? 3.0 : s.probeUpdatePeriod >= 4 ? 2.0 : s.probeUpdatePeriod >= 2 ? 1.0 : 0.0;
    case O::RayDenoiser: return double(int(s.rayDenoiser));
    case O::PathTracer: return s.pathTracer ? 1.0 : 0.0;
    case O::PathBounces: return double(s.pathBounces);
    case O::Bloom: return s.bloom ? 1.0 : 0.0;
    case O::BloomStrength: return double(s.bloomStrength);
    case O::BloomThreshold: return double(s.bloomThreshold);
    case O::Sky: return s.sky ? 1.0 : 0.0;
    case O::ToneCurve: return double(int(s.toneCurve));
    case O::Exposure: return 1.0;
    case O::Vignette: return double(s.vignette);
    case O::Dither: return s.dither ? 1.0 : 0.0;
    case O::Count: break;
    }
    return 0.0;
}

GraphicsOverrides parseGraphics(const json::Value& value) {
    GraphicsOverrides g;
    if (!value.isObject()) return g;
    for (const OptionInfo& o : kOptions) {
        const json::Value& v = value[o.key];
        switch (o.kind) {
        case Kind::Choice:
            for (std::size_t i = 0; i < o.choices.size(); ++i)
                if (o.choices[i] && v.stringOr("") == o.choices[i]) g.set(o.option, double(i));
            break;
        case Kind::Flag:
            if (v.type == json::Value::Type::Bool) g.set(o.option, v.boolean ? 1.0 : 0.0);
            break;
        case Kind::Number:
        case Kind::Integer:
            if (v.isNumber()) g.set(o.option, v.number);
            break;
        }
    }
    return g;
}

json::Value writeGraphics(const GraphicsOverrides& overrides) {
    json::Value v = json::Value::makeObject();
    for (const OptionInfo& o : kOptions) {
        const std::optional<double> value = overrides.get(o.option);
        if (!value) continue;
        switch (o.kind) {
        case Kind::Choice: v.object[o.key] = json::Value::makeString(o.choices[std::size_t(*value)]); break;
        case Kind::Flag: v.object[o.key] = json::Value::makeBool(*value != 0.0); break;
        case Kind::Integer: v.object[o.key] = json::Value::makeNumber(*value); break;
        // Four decimals: a float from a slider reads back as itself, not as 0.30000001192092896.
        case Kind::Number: v.object[o.key] = json::Value::makeNumber(std::round(*value * 1.0e4) / 1.0e4); break;
        }
    }
    return v;
}

// ---- the demo's side

bool DemoGame::graphicsSuspended() const { return m_mode == Mode::Checks || m_mode == Mode::Benchmark; }

RenderSettings DemoGame::graphicsSettings(const RenderSettings& own) const {
    return graphicsSuspended() ? own : m_settings.graphics.apply(own);
}

float DemoGame::graphicsExposure() const { return graphicsSuspended() ? 1.0f : m_settings.graphics.exposure(); }

void DemoGame::graphicsNote() {
    const GraphicsOverrides& g = m_settings.graphics;
    if (graphicsSuspended() || g.empty()) return;
    std::string names;
    for (std::size_t i = 0; i < kGraphicsOptions; ++i)
        if (const auto option = GraphicsOption(i); option != O::Exposure && g.has(option))
            names += std::format("{}{}", names.empty() ? "" : ", ", graphicsOptionName(option));
    ImGui::PushStyleColor(ImGuiCol_Text, kSetColor);
    if (!names.empty())
        ImGui::TextWrapped("Set in Graphics for every section: %s. The controls here show those values and change "
                           "them there.",
                           names.c_str());
    if (g.has(O::Exposure)) ImGui::TextWrapped("Graphics scales the exposure by %.2f.", double(g.exposure()));
    ImGui::PopStyleColor();
    if (m_appliedTouch ? ImGui::Button("Open Graphics") : ImGui::SmallButton("Open Graphics")) openGraphics(false);
}

RenderSettings DemoGame::takeSectionEdits(const RenderSettings& own, const RenderSettings& edited) {
    if (graphicsSuspended()) return edited;
    const GraphicsOverrides before = m_settings.graphics;
    const RenderSettings result = m_settings.graphics.takeEdits(own, edited);
    if (!(m_settings.graphics == before)) noteSettings();
    return result;
}

// The button and G open it; G closes it again. It shows the windows when they are hidden.
void DemoGame::openGraphics(bool toggle) {
    const bool shown = compact() ? m_sheet == Sheet::Graphics : m_graphicsOpen;
    const bool open = !m_showUi || !toggle || !shown;
    m_graphicsOpen = open;
    if (compact()) {
        if (open) {
            m_aboutOpen = m_settingsOpen = false;
            m_sheet = Sheet::Graphics;
        } else if (m_sheet == Sheet::Graphics) {
            m_sheet = Sheet::None;
        }
    }
    if (open && !m_showUi) {
        m_showUi = true;
        m_uiReappears = true;
        noteSettings();
    }
}

void DemoGame::graphicsWindow(App& app, ImVec2 min, ImVec2 max) {
    const app::WindowRect safe = app.safeArea();
    const float em = ImGui::GetFontSize();
    const float width = std::min(27.0f * em, float(safe.width) - 1.2f * em);
    ImVec2 pos = min;
    if (max.x - min.x < width) // no room between the menu and the section's window: over the middle
        pos.x = float(safe.x) + (float(safe.width) - width) * 0.5f;
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, std::max(max.y - pos.y, 8.0f * em)));
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("Graphics", &m_graphicsOpen,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | navFlags()))
        graphicsContents(app, false);
    ImGui::End();
}

void DemoGame::graphicsSheet(App& app, ImVec2 pos, ImVec2 size) {
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("##graphics", nullptr, kSheetFlags | navFlags())) {
        if (ImGui::Button("Close")) {
            m_graphicsOpen = false;
            m_sheet = Sheet::None;
        }
        recordPoint("ui:close");
        ImGui::SameLine();
        ImGui::TextUnformatted("Graphics");
        graphicsContents(app, true);
        dragScroll();
    }
    ImGui::End();
}

void DemoGame::graphicsContents(App& app, bool sheet) {
    const render::ActiveEffects& e = m_renderer->effects();
    const render::RenderStats& stats = m_renderer->stats();
    const std::span<const std::string> fallbacks = m_renderer->fallbacks();
    const gpu::Caps& caps = app.device().caps();
    const app::FrameStats& frame = app.stats();
    GraphicsOverrides& g = m_settings.graphics;
    const GraphicsOverrides before = g;
    const RenderSettings& own = current().settings;
    const RenderSettings s = graphicsSettings(own); // what the renderer was given, before Adapt to heat
    const bool locked = graphicsSuspended();
    // The renderer's reason for a fallback without the effect's name and what replaced it.
    const auto reason = [&](std::string_view effect, std::string_view otherwise) {
        for (const std::string& line : fallbacks) {
            if (!line.starts_with(effect) || line.size() < effect.size() + 2 || line[effect.size()] != ':') continue;
            std::string why = line.substr(effect.size() + 2);
            if (const std::size_t cut = why.rfind("; "); cut != std::string::npos) why.resize(cut);
            return why;
        }
        return std::string(otherwise);
    };
    const bool sdlGpu = caps.backend == gpu::Backend::SdlGpu && gpu::backendAvailable(gpu::Backend::Metal);
    const std::string_view metalHint = sdlGpu ? "; the native Metal backend has it on Apple GPUs (below)" : "";

    // What the options cost: the same numbers as the Performance panel.
    if (app.deterministic())
        ImGui::TextUnformatted("Timings hidden (deterministic run)");
    else if (frame.gpuMs >= 0.0)
        ImGui::Text("%.0f fps, frame %.2f ms, CPU %.2f ms, GPU %.2f ms", frame.fps, frame.frameMs, frame.cpuMs,
                    frame.gpuMs);
    else
        ImGui::Text("%.0f fps, frame %.2f ms, CPU %.2f ms, GPU n/a", frame.fps, frame.frameMs, frame.cpuMs);
    const std::string_view title = current().title();
    ImGui::TextDisabled("%.*s; %zu fallbacks", int(title.size()), title.data(), fallbacks.size());
    if (const app::ThermalGovernor& heat = app.thermalGovernor(); !(heat.limits() == app::ThermalLimits{}))
        ImGui::TextWrapped("Adapt to heat (Settings), thermal state %s: %s on top of these options.",
                           app::thermalStateName(heat.level()), app::thermalLimitsText(heat.limits()).c_str());
    ImGui::BeginDisabled(locked || g.empty());
    if (ImGui::Button("Use section defaults")) g = {};
    ImGui::EndDisabled();
    recordPoint("ui:graphics_defaults");
    ImGui::SameLine();
    if (g.empty())
        ImGui::TextDisabled("Nothing set: each section's own");
    else
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kSetColor), "%zu set, for every section", g.count());
    if (locked)
        note(std::format("Not used while the {} runs: it {} each section with its own settings; the values come back "
                         "when it ends.",
                         m_mode == Mode::Checks ? "check mode" : "benchmark",
                         m_mode == Mode::Checks ? "sets up" : "measures"),
             true);

    Rows rows(g, own, sheet, sheet || m_appliedTouch, locked);
    const auto header = [](const char* label) {
        return ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
    };

    if (header("Anti-aliasing and resolution")) {
        rows.row(O::AntiAliasing, [&] { return combo(O::AntiAliasing, int(s.antiAliasing)); });
        if (e.upscaling == render::Upscaling::Temporal || e.upscaling == render::Upscaling::TemporalDenoised)
            note("MetalFX resolves in TAA's place, from TAA's jitter and motion", false);
        else if (e.pathTracer)
            note("Not used: the path tracer's mean of jittered samples is the anti-aliasing", false);
        else if (e.antiAliasing != s.antiAliasing)
            note(std::format("-> {}: {}", antiAliasingName(e.antiAliasing),
                             reason("anti-aliasing", "the device lacks what it needs")),
                 true);
        rows.row(O::Sharpen, [&] { return slider(s.sharpen, 0.0f, 1.0f, "%.2f"); });
        if (e.antiAliasing != render::AntiAliasing::Taa || e.upscaling != render::Upscaling::Bilinear)
            note("Not used: it follows TAA's resolve, without MetalFX", false);
        rows.row(O::RenderScale, [&] { return slider(s.renderScale, 0.25f, 1.0f, "%.2f"); });
        note(std::format("3D at {} x {} for {} x {}", stats.renderWidth, stats.renderHeight, app.pixelWidth(),
                         app.pixelHeight()),
             false);
        rows.row(O::Upscaling, [&] { return combo(O::Upscaling, int(s.upscaling)); });
        if (e.upscaling == render::Upscaling::TemporalDenoised)
            note("Denoises the traced effects as it scales up, from the G-buffer; the engine's denoiser is off", false);
        if (s.upscaling != e.upscaling) {
            if (s.upscaling == render::Upscaling::Spatial && caps.spatialUpscaling)
                note("-> bilinear: spatial upscaling runs below render scale 1", true);
            else if (s.upscaling == render::Upscaling::Spatial)
                note(std::format("-> bilinear: {}{}",
                                 reason("spatial upscaling", "not on this device (gpu::Caps::spatialUpscaling)"),
                                 metalHint),
                     true);
            else if (e.pathTracer)
                note(std::format("-> {}: the path tracer accumulates its own samples", upscalingName(e.upscaling)),
                     true);
            else if (s.upscaling == render::Upscaling::TemporalDenoised)
                note(std::format("-> {}: {}{}", upscalingName(e.upscaling),
                                 reason("denoising upscaler", "not on this device (gpu::Caps::denoisedUpscaling)"),
                                 e.upscaling == render::Upscaling::Bilinear ? metalHint : ""),
                     true);
            else
                note(std::format("-> bilinear: {}{}",
                                 reason("temporal upscaling", "not on this device (gpu::Caps::temporalUpscaling)"),
                                 metalHint),
                     true);
        }
    }

    if (header("Sun shadows")) {
        rows.row(O::SunShadows, [&] { return combo(O::SunShadows, int(s.sunShadows)); });
        if (s.shadowCascades == 0)
            note("Off: 0 cascades turns the sun's shadows off, the ray-traced ones too", false);
        else if (s.sunShadows == render::SunShadows::RayTraced && e.sunShadows != render::SunShadows::RayTraced)
            note(std::format("-> shadow maps: {}", caps.rayTracing ? reason("ray-traced shadows", "no ray tracing here")
                                                                   : std::string(rayTracingWhyNot(caps))),
                 true);
        if (e.sunShadows == render::SunShadows::RayTraced)
            note("Ray traced: map size, filter and translucent are for shadow maps", false);
        else if (s.shadowCascades > 0)
            note("Shadow rays and softness are for ray-traced shadows", false);
        rows.row(O::Cascades, [&] { return sliderInt(s.shadowCascades, 0, render::kMaxCascades); });
        if (e.sunShadows == render::SunShadows::ShadowMaps && s.shadowCascades > 0 && e.shadowCascades == 0)
            note(std::format("-> none: {}", reason("shadows off", "no depth texture array to sample")), true);
        else if (e.shadowCascades > 0 && !e.shadowDepthClamp)
            note("No depth clamp (gpu::Caps::depthClamp): casters in front of a cascade are clipped", true);
        rows.row(O::ShadowMapSize, [&] {
            static constexpr std::array<std::uint32_t, 4> kSizes{512, 1024, 2048, 4096};
            const auto found = std::ranges::find(kSizes, s.shadowMapSize);
            int size = found == kSizes.end() ? -1 : int(found - kSizes.begin());
            if (ImGui::Combo("##value", &size,
                             "512\0"
                             "1024\0"
                             "2048\0"
                             "4096\0") &&
                size >= 0)
                return std::optional<double>(kSizes[std::size_t(size)]);
            return std::optional<double>();
        });
        rows.row(O::ShadowDistance, [&] { return slider(s.shadowDistance, 10.0f, 200.0f, "%.0f m"); });
        if (e.sunShadows == render::SunShadows::RayTraced)
            note("Ray traced: skinned casters this near are posed for the rays", false);
        rows.row(O::ShadowFilter, [&] {
            return sliderInt(s.shadowFilter, 0, 2,
                             s.shadowFilter <= 0   ? "1 tap"
                             : s.shadowFilter == 1 ? "4 taps"
                                                   : "12 taps");
        });
        rows.row(O::TranslucentShadows, [&] { return combo(O::TranslucentShadows, int(s.translucentShadows)); });
        if (e.shadowCascades > 0 && s.translucentShadows != e.translucentShadows)
            note(std::format("-> dithered: {}", reason("translucent shadows", "no texture array to render to")), true);
        rows.row(O::ShadowRays, [&] { return sliderInt(s.shadowRays, 1, 8); });
        rows.row(O::ShadowSoftness, [&] { return slider(s.shadowSoftness, 0.0f, 6.0f, "%.1f x sun"); });
    }

    if (header("Ambient occlusion")) {
        rows.row(O::AmbientOcclusion, [&] { return combo(O::AmbientOcclusion, int(s.ambientOcclusion)); });
        const bool screenOcclusion = e.ambientOcclusion == render::AmbientOcclusion::ScreenSpace;
        if (s.ambientOcclusion == render::AmbientOcclusion::RayTraced &&
            e.ambientOcclusion != render::AmbientOcclusion::RayTraced)
            note(std::format("-> {}: {}", screenOcclusion ? "screen space (GTAO)" : "none",
                             caps.rayTracing ? reason("ray-traced ambient occlusion", "no ray tracing here")
                                             : std::string(rayTracingWhyNot(caps))),
                 true);
        else if (s.ambientOcclusion == render::AmbientOcclusion::ScreenSpace && !screenOcclusion)
            note(std::format("-> none: {}", reason("screen-space ambient occlusion", "no depth buffer to sample")),
                 true);
        if (screenOcclusion)
            note("From the depth of what is on the screen, at half resolution: rays are slices of 4 steps to either "
                 "side",
                 false);
        else if (e.ambientOcclusion != render::AmbientOcclusion::RayTraced)
            note("Rays, radius and strength are for ray-traced or screen-space occlusion", false);
        rows.row(O::OcclusionRays, [&] { return sliderInt(s.occlusionRays, 1, 8); });
        rows.row(O::OcclusionRadius, [&] { return slider(s.occlusionRadius, 0.2f, 3.0f, "%.1f m"); });
        rows.row(O::OcclusionStrength, [&] { return slider(s.occlusionStrength, 0.0f, 1.0f, "%.2f"); });
        rows.row(O::ScreenSpaceFallbacks, [&] { return checkbox(s.screenSpaceFallbacks); });
        note(s.screenSpaceFallbacks ? "Where occlusion and reflections cannot be ray traced: their screen-space "
                                      "versions in place of none and the sky's"
                                    : "Where occlusion and reflections cannot be ray traced: none and the sky's",
             false);
    }

    if (header("Ray-traced lighting")) {
        const std::string whyNot = caps.rayTracing ? std::string() : std::string(rayTracingWhyNot(caps));
        rows.row(O::LocalShadows, [&] { return sliderInt(s.localLightShadows, 0, render::kMaxRayLocalShadows); });
        if (s.localLightShadows > 0 && e.localLightShadows == 0)
            note(
                std::format("-> {}: {}", e.localShadowMaps > 0 ? "shadow maps" : "unshadowed",
                            caps.rayTracing ? reason("ray-traced local light shadows", "no ray tracing here") : whyNot),
                true);
        else if (e.localLightShadows > 0)
            note(std::format("The {} nearest point and spot lights that cast shadows", e.localLightShadows), false);
        rows.row(O::LocalShadowMaps,
                 [&] { return combo(O::LocalShadowMaps, int(graphicsValue(s, O::LocalShadowMaps))); });
        if (e.localShadowMaps > 0)
            note(std::format("{} lights with {} maps this frame (a spot light one, a point light six); area lights "
                             "unshadowed",
                             stats.localShadowLights, stats.localShadowMaps),
                 false);
        else
            note("Where local light shadows cannot be ray traced", false);
        rows.row(O::Reflections, [&] { return combo(O::Reflections, int(s.reflections)); });
        const bool screenReflections = e.reflections == render::Reflections::ScreenSpace;
        if (s.reflections == render::Reflections::RayTraced && e.reflections != render::Reflections::RayTraced)
            note(std::format("-> {}: {}", screenReflections ? "screen space (SSR)" : "the sky's",
                             caps.rayTracing ? reason("ray-traced reflections", "no ray tracing here") : whyNot),
                 true);
        else if (s.reflections == render::Reflections::ScreenSpace && !screenReflections)
            note(std::format("-> the sky's: {}", reason("screen-space reflections", "no depth buffer to sample")),
                 true);
        if (screenReflections)
            note("From the previous frame's image, at half resolution: only what was on the screen is reflected, the "
                 "sky's reflection elsewhere",
                 false);
        rows.row(O::ReflectionRoughness, [&] { return slider(s.reflectionRoughness, 0.0f, 1.0f, "%.2f"); });
        if (e.reflections != render::Reflections::Sky) note("Rougher surfaces fade into the sky's reflection", false);
        rows.row(O::IndirectLight, [&] { return combo(O::IndirectLight, int(s.indirectLight)); });
        if (s.indirectLight != render::IndirectLight::Sky && e.indirectLight == render::IndirectLight::Sky)
            note(std::format("-> the sky's ambient light: {}",
                             caps.rayTracing ? reason(s.indirectLight == render::IndirectLight::Probes
                                                          ? "the probe grid"
                                                          : "ray-traced indirect light",
                                                      "no ray tracing here")
                                             : whyNot),
                 true);
        else if (e.indirectLight != render::IndirectLight::Sky)
            note(
                std::format("{} of 2048 probes traced this frame, 16 x 8 x 16 around the camera's focus", stats.probes),
                false);
        rows.row(O::ProbeSpacing, [&] { return slider(s.probeSpacing, 0.5f, 8.0f, "%.2f m"); });
        rows.row(O::ProbeUpdates, [&] { return combo(O::ProbeUpdates, int(graphicsValue(s, O::ProbeUpdates))); });
        if (e.indirectLight != render::IndirectLight::Sky && s.probeUpdatePeriod > 1)
            note(std::format("Every probe once in {} frames: the light follows changes {} times more slowly",
                             s.probeUpdatePeriod, s.probeUpdatePeriod),
                 false);
        rows.row(O::RayDenoiser, [&] { return combo(O::RayDenoiser, int(s.rayDenoiser)); });
        if (e.upscaling == render::Upscaling::TemporalDenoised)
            note("Not used: MetalFX denoises (Upscaling)", false);
        else if (s.rayDenoiser != e.rayDenoiser)
            note(std::format("-> spatial: {}", reason("temporal ray denoiser", "no motion vectors here")), true);
        rows.row(O::PathTracer, [&] { return checkbox(s.pathTracer); });
        if (s.pathTracer && !e.pathTracer)
            note(std::format("-> the real-time frame: {}",
                             caps.rayTracing ? reason("path tracer", "no ray tracing here") : whyNot),
                 true);
        else if (e.pathTracer)
            note(
                std::format("A reference, not for play: {} samples so far; any change starts again", stats.pathSamples),
                false);
        rows.row(O::PathBounces, [&] { return sliderInt(s.pathBounces, 1, 16); });
    }

    if (header("Bloom, tone and sky")) {
        rows.row(O::Bloom, [&] { return checkbox(s.bloom); });
        if (s.bloom && !e.bloom) note("Off: strength 0", false);
        rows.row(O::BloomStrength, [&] { return slider(s.bloomStrength, 0.0f, 0.3f, "%.3f"); });
        rows.row(O::BloomThreshold, [&] { return slider(s.bloomThreshold, 0.2f, 4.0f, "%.2f"); });
        rows.row(O::ToneCurve, [&] { return combo(O::ToneCurve, int(s.toneCurve)); });
        if (app.output().hdr())
            note(std::format("HDR output: the curve with its shoulder up to headroom {:.2f}",
                             double(app.output().headroom)),
                 false);
        rows.row(O::Exposure,
                 [&] { return slider(g.exposure(), 0.25f, 4.0f, "x %.2f", ImGuiSliderFlags_Logarithmic); });
        note("On top of each section's own exposure", false);
        rows.row(O::Vignette, [&] { return slider(s.vignette, 0.0f, 1.0f, "%.2f"); });
        rows.row(O::Dither, [&] { return checkbox(s.dither); });
        if (app.output().hdr()) note("Not used in HDR output", false);
        rows.row(O::Sky, [&] { return checkbox(s.sky); });
        if (!s.sky) note("Off: the section's clear color", false);
    }

    if (header("Display")) outputControls(app);

    if (header("GPU backend")) {
        ImGui::TextWrapped("%s backend (%s, %s); ray tracing %s, MetalFX %s, GPU timing %s",
                           gpu::backendName(caps.backend), caps.driver.c_str(), caps.deviceName.c_str(),
                           caps.rayTracing ? "yes" : "no",
                           caps.denoisedUpscaling                            ? "yes, with denoising"
                           : caps.temporalUpscaling || caps.spatialUpscaling ? "yes"
                                                                             : "no",
                           caps.gpuTiming ? "yes" : "no");
        if (!caps.rayTracing) note(std::format("No ray tracing: {}", rayTracingWhyNot(caps)), false);
        relaunchButtons(app);
    }

    if (!(g == before)) noteSettings();
}

// The app's display options: in Settings and in the Graphics panel.
void DemoGame::outputControls(App& app) {
    int hdr = int(app.hdrMode());
    ImGui::TextUnformatted("HDR output");
    bool changed = ImGui::RadioButton("off##hdr", &hdr, int(app::HdrMode::Off));
    ImGui::SameLine();
    changed |= ImGui::RadioButton("auto##hdr", &hdr, int(app::HdrMode::Auto));
    ImGui::SameLine();
    changed |= ImGui::RadioButton("on##hdr", &hdr, int(app::HdrMode::On));
    if (changed) app.setHdrMode(app::HdrMode(hdr));
    const gpu::DisplayOutput& out = app.output();
    if (out.hdr())
        note(std::format("HDR extended linear, headroom {:.2f}, paper white {:.2f}{}", double(out.headroom),
                         double(out.paperWhite),
                         app.displayHeadroom() <= 1.0f ? "; this display shows no HDR range: clipped at paper white"
                                                       : ""),
             app.displayHeadroom() <= 1.0f);
    else if (app.hdrMode() != app::HdrMode::Off && !app.hdrSupported())
        note("-> SDR: this GPU backend cannot show the window in HDR", true);
    else if (app.hdrMode() == app::HdrMode::Auto)
        note(std::format("SDR: the display shows no HDR range (headroom {:.2f})",
                         double(app.displayPotentialHeadroom())),
             false);
    ImGui::TextUnformatted("Present");
    const std::string present = m_settings.present.empty() ? "vsync" : m_settings.present;
    for (const char* mode : {"vsync", "immediate", "mailbox"}) {
        ImGui::SameLine();
        const std::string label = std::format("{}##present", mode);
        if (ImGui::RadioButton(label.c_str(), present == mode)) {
            m_settings.present = mode;
            app.setPresentMode(presentModeOf(mode));
            noteSettings();
        }
    }
    if (const gpu::PresentMode mode = presentModeOf(present);
        mode != gpu::PresentMode::Vsync && !app.device().supportsPresentMode(app.window(), mode))
        note(std::format("-> vsync: no {} for this window", present), true);
}

// On a desktop: the demo again with the other GPU backend (the same command line with another --gpu-backend).
void DemoGame::relaunchButtons(App& app) {
    const gpu::Caps& caps = app.device().caps();
    if (!app.canRelaunch()) {
        const char* other = gpu::defaultBackend() == gpu::Backend::Metal ? "sdl" : "metal";
        ImGui::TextWrapped("This platform cannot start the app again from inside: the backend is chosen at launch, "
                           "%s by default, the other one with --gpu-backend %s or the environment variable "
                           "QUENCH_GPU_BACKEND=%s (docs/demo.md: devicectl with --environment-variables).",
                           gpu::defaultBackend() == gpu::Backend::Metal ? "the native Metal backend" : "SDL GPU", other,
                           other);
        return;
    }
    for (const gpu::Backend backend : {gpu::Backend::Metal, gpu::Backend::SdlGpu}) {
        if (!gpu::backendAvailable(backend) || backend == caps.backend) continue;
        const std::string label =
            std::format("Relaunch with {}{}", backend == gpu::Backend::Metal ? "the native Metal backend" : "SDL GPU",
                        backend == gpu::defaultBackend() ? " (the default)" : "");
        if (ImGui::Button(label.c_str())) {
            std::vector<std::string> args;
            const std::span<const std::string> old = app.arguments();
            for (std::size_t i = 0; i < old.size(); ++i) {
                if (old[i] == "--gpu-backend") {
                    ++i;
                    continue;
                }
                if (old[i].starts_with("--gpu-backend=")) continue;
                args.push_back(old[i]);
            }
            args.push_back("--gpu-backend");
            args.push_back(backend == gpu::Backend::Metal ? "metal" : "sdl");
            saveSettings(true);
            if (Result<void> r = app.relaunch(args); !r) log::error("demo", "relaunch: {}", r.error());
        }
        recordPoint("ui:relaunch");
    }
}

} // namespace demo
