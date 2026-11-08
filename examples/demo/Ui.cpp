// The demo's windows. Wide layout (a desktop window, a tablet): the menu at the top left, the section's panel at the
// top right, the mode windows at the bottom. Compact layout (phones in either orientation, small windows): a bar at
// the top and one sheet at a time (the sections and the menu on the left, the section's controls at the bottom in
// portrait and on the right in landscape, About and Settings over the whole screen); sheets scroll by a drag. Sizes
// follow the display's scale (the engine's style), the user's UI scale and, on touch screens, larger controls.

#include "DemoGame.h"

#include <demo_build_info.h>

#include <quench/audio/AudioSystem.h>
#include <quench/audio/Device.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Log.h>

#include <imgui.h>

#include <SDL3/SDL_platform.h>

#include <algorithm>
#include <cfloat>
#include <format>
#include <string>
#include <utility>

namespace demo {

using namespace quench;
using app::App;

namespace {

constexpr ImGuiWindowFlags kSheetFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                         ImGuiWindowFlags_NoSavedSettings;

// The controls of every 3D section, per device (also in the About window and docs/demo.md).
void controlsHelp() {
    ImGui::SeparatorText("Camera");
    ImGui::BulletText("Mouse: left drag orbits, right or middle drag pans, the wheel zooms; a click acts.");
    ImGui::BulletText("Touch: one finger orbits, two fingers pan and pinch to zoom; a tap acts.");
    ImGui::BulletText("Gamepad: the right stick orbits, the left stick pans, the triggers zoom.");
    ImGui::BulletText("Keyboard: the arrows orbit, + and - zoom.");
    ImGui::SeparatorText("Sections and windows");
    ImGui::BulletText("Next, previous: ] [ or PgDn PgUp, the shoulder buttons, the arrows of the top bar.");
    ImGui::BulletText("Keys 1 to 9: sections 1 to 9; key 0: section 10; section 11 by next or previous.");
    ImGui::BulletText("Menu by keyboard or gamepad: Tab or the D-pad; Enter or A press, Escape or B go back.");
    ImGui::BulletText("Hide the windows: H, Start, or Hide in the menu.");
    ImGui::BulletText("Graphics panel (render options for every section): G, or Graphics in the menu.");
    ImGui::TextWrapped("The input section's character takes W A S D and the left stick; a section's own keys are in "
                       "its description.");
}

} // namespace

ImGuiWindowFlags DemoGame::navFlags() const { return m_menuFocus ? 0 : ImGuiWindowFlags_NoNavInputs; }

void DemoGame::recordPoint(std::string name) {
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    m_points.emplace_back(std::move(name), glm::vec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f));
}

// The engine's style for the display, scaled by the user's UI scale; on touch screens taller controls, wider
// scrollbars and grabs, and a touch margin around every item.
void DemoGame::applyStyle() {
    const bool touch =
        m_settings.touch == TouchControls::On || (m_settings.touch == TouchControls::Auto && m_touchScreen);
    const float scale = m_settings.uiScale;
    if (scale == m_appliedScale && touch == m_appliedTouch) return;
    ImGuiStyle style = m_baseStyle;
    if (scale != 1.0f) style.ScaleAllSizes(scale);
    style.FontScaleMain = scale;
    if (touch) {
        const float em = (style.FontSizeBase > 0.0f ? style.FontSizeBase : 13.0f) * scale * style.FontScaleDpi;
        style.FramePadding =
            ImVec2(std::max(style.FramePadding.x, 0.5f * em), std::max(style.FramePadding.y, 0.4f * em));
        style.ItemSpacing.y = std::max(style.ItemSpacing.y, 0.4f * em);
        style.TouchExtraPadding = ImVec2(0.2f * em, 0.2f * em);
        style.ScrollbarSize = std::max(style.ScrollbarSize, 1.0f * em);
        style.GrabMinSize = std::max(style.GrabMinSize, 1.0f * em);
    }
    ImGui::GetStyle() = style;
    m_appliedScale = scale;
    m_appliedTouch = touch;
}

// Phones in either orientation and small windows: under 40 x 26 em of safe area.
bool DemoGame::compact() const {
    if (!m_context) return false;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float em =
        (style.FontSizeBase > 0.0f ? style.FontSizeBase : 13.0f) * style.FontScaleMain * style.FontScaleDpi;
    const app::WindowRect safe = m_context->app.safeArea();
    return float(safe.width) < m_compactWidthEm * em || float(safe.height) < m_compactHeightEm * em;
}

void DemoGame::ui(App& app) {
    m_points.clear();
    if (!m_showUi) {
        m_bottomTop = 1.0e9f;
        m_bottomLeft = m_bottomRight = 0.0f;
        return;
    }
    if (currentReady()) current().overlay(*m_context);
    if (m_windowsOff) {
        m_bottomTop = 1.0e9f;
        m_bottomLeft = m_bottomRight = 0.0f;
        return;
    }
    if (compact())
        compactLayout(app);
    else
        wideLayout(app);
    bottomWindows(app);
    if (m_uiReappears || m_focusMenuNow) {
        // Windows that reappear take the focus, and after a key (H) ImGui shows its navigation frame on their first
        // item: only noise for a click or a tap; Tab and the D-pad show it on purpose.
        ImGui::SetNavCursorVisible(m_focusMenuNow);
        m_uiReappears = false;
        m_focusMenuNow = false;
    }
}

void DemoGame::sectionList(bool compactList) {
    for (std::size_t i = 0; i < m_slots.size(); ++i) {
        const std::string_view title = m_slots[i].section->title();
        const std::string label = std::format("{:>2}  {}", i + 1, title);
        // Tab or the D-pad start on the current section.
        if (i == m_current && m_focusMenuNow) ImGui::SetKeyboardFocusHere();
        if (ImGui::Selectable(label.c_str(), i == m_current)) {
            select(i);
            if (compactList) m_sheet = Sheet::None;
        }
        if (i == m_current) ImGui::SetItemDefaultFocus();
        recordPoint(std::format("menu:{}", m_slots[i].section->id()));
    }
}

void DemoGame::modeButtons() {
    if (ImGui::Button(m_mode == Mode::Tour ? "Stop tour" : "Tour")) {
        if (m_mode == Mode::Tour)
            stopMode("stopped");
        else
            startTour(m_tourSeconds);
    }
    recordPoint("ui:tour");
    ImGui::SameLine();
    if (ImGui::Button(m_mode == Mode::Checks ? "End checks" : "Checks")) {
        if (m_mode == Mode::Checks)
            stopMode("ended");
        else
            startChecks();
    }
    recordPoint("ui:checks");
    ImGui::SameLine();
    if (ImGui::Button(m_mode == Mode::Benchmark ? "Stop benchmark" : "Benchmark")) {
        if (m_mode == Mode::Benchmark)
            stopMode("stopped");
        else
            startBenchmark(5.0f, false);
    }
    recordPoint("ui:benchmark");
    if (ImGui::Button("About")) {
        m_aboutOpen = true;
        if (compact()) m_sheet = Sheet::About;
    }
    recordPoint("ui:about");
    ImGui::SameLine();
    if (ImGui::Button("Settings")) {
        m_settingsOpen = true;
        if (compact()) m_sheet = Sheet::Settings;
    }
    recordPoint("ui:settings");
    ImGui::SameLine();
    if (ImGui::Button("Graphics")) openGraphics(false);
    recordPoint("ui:graphics");
    ImGui::SameLine();
    if (ImGui::Button("Hide")) {
        m_showUi = false;
        noteSettings();
    }
    recordPoint("ui:hide");
    if (!m_modeMessage.empty()) ImGui::TextWrapped("%s", m_modeMessage.c_str());
}

void DemoGame::performancePanel(App& app) {
    if (!ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const app::FrameStats& stats = app.stats();
    const gpu::Caps& caps = app.device().caps();
    const float em = ImGui::GetFontSize();
    if (app.deterministic()) {
        ImGui::TextUnformatted("Timings hidden (deterministic run)"); // captures compare against golden images
    } else {
        ImGui::Text("%.0f fps, frame %.2f ms", stats.fps, stats.frameMs);
        if (stats.gpuMs >= 0.0)
            ImGui::Text("CPU %.2f ms, GPU %.2f ms", stats.cpuMs, stats.gpuMs);
        else
            ImGui::Text("CPU %.2f ms, GPU time n/a", stats.cpuMs);
        const float longest = *std::max_element(m_frameTimes.begin(), m_frameTimes.end());
        const std::string overlay = std::format("frames, up to {:.1f} ms", longest);
        ImGui::PlotLines("##frames", m_frameTimes.data(), int(m_frameTimes.size()), int(m_frameTimeNext),
                         overlay.c_str(), 0.0f, std::max(longest, 1.0f), ImVec2(16.0f * em, 2.4f * em));
        app.requestPassTiming();
        ImGui::Text("GPU backend %s, pass timing %s", gpu::backendName(caps.backend),
                    caps.gpuPassTiming ? "on" : "n/a");
        // GPU busy time per range and when it ran in the frame; nested ranges indented under theirs.
        for (const gpu::GpuTiming& t : stats.gpuPasses)
            ImGui::Text("%*s%-8s %.3f ms  (%.2f .. %.2f)", int(2 + 2 * t.depth), "", t.name.c_str(), t.milliseconds,
                        t.startMilliseconds, t.endMilliseconds);
        if (const std::optional<std::uint64_t> bytes = app.memoryFootprint())
            ImGui::Text("Memory %.0f MB, GPU resources %lld", double(*bytes) / double(1u << 20),
                        static_cast<long long>(app.device().liveResources()));
        else
            ImGui::Text("GPU resources %lld", static_cast<long long>(app.device().liveResources()));
        ImGui::Text("Thermal state %s", app::thermalStateName(stats.thermal));
        heatStatus(app);
        const Slot& slot = m_slots[m_current];
        if (slot.initMs >= 0.0) ImGui::Text("Section ready in %.1f ms, switch %.1f ms", slot.initMs, m_switchMs);
        ImGui::Text("Jobs: %d threads, %d pending", app.jobs().threadCount(), app.jobs().pendingCount());
        if (ImGui::Button("Job test")) startJobTest(app);
        recordPoint("ui:job_test");
        ImGui::SameLine();
        if (!m_jobTest) {
            ImGui::TextDisabled("256 items on the pool");
        } else if (m_jobTest->running) {
            ImGui::TextUnformatted("running...");
        } else {
            std::string perThread;
            for (std::size_t t = 0; t < m_jobTest->ranges.size(); ++t)
                if (const int n = m_jobTest->ranges[t].load(std::memory_order_relaxed); n > 0)
                    perThread += std::format("{}{}:{}", perThread.empty() ? "" : " ", t, n);
            ImGui::Text("%.1f ms", m_jobTest->ms);
            ImGui::TextWrapped("Items per thread %s", perThread.c_str());
        }
    }
    const render::RenderStats& s = m_renderer->stats();
    ImGui::Text("%u draws, %u shadow draws", s.draws, s.shadowDraws);
    ImGui::Text("%s, %s", caps.driver.c_str(), caps.deviceName.c_str());
}

void DemoGame::heatStatus(App& app) {
    const app::ThermalGovernor& heat = app.thermalGovernor();
    if (!heat.enabled()) {
        ImGui::TextDisabled("Adapt to heat off (Settings)");
        return;
    }
    if (heat.limits() == app::ThermalLimits{}) {
        ImGui::TextUnformatted("Adapt to heat: nothing reduced");
    } else {
        std::string reduced;
        const auto add = [&reduced](const std::string& part) { reduced += (reduced.empty() ? "" : ", ") + part; };
        if (app.frameRateLimit() != app.maxFrameRate()) add(std::format("at most {:.0f} fps", app.frameRateLimit()));
        if (heat.limits().renderScaleFactor < 1.0f)
            add(std::format("render scale {:.2f} -> {:.2f}", graphicsSettings(current().settings).renderScale,
                            rendererSettings(current().settings).renderScale));
        if (heat.reduceEffects()) add("traced reflections, indirect light and lamp shadows but one off");
        ImGui::TextWrapped("Adapt to heat, %s for %.0f s: %s", app::thermalStateName(heat.level()),
                           app.time() - m_heatChangedAt,
                           reduced.empty() ? "nothing below your settings" : reduced.c_str());
    }
    if (heat.recoveringSince() >= 0.0)
        ImGui::TextDisabled("Cooler for %.0f of %.0f s", app.time() - heat.recoveringSince(),
                            heat.desc().recoverSeconds);
}

void DemoGame::logPanel() {
    const std::string title = m_context->app.deterministic()
                                  ? std::string("Log")
                                  : std::format("Log ({} warnings, {} errors)###log", m_log.warnings(), m_log.errors());
    if (!ImGui::CollapsingHeader(title.c_str())) return;
    const float em = ImGui::GetFontSize();
    ImGui::BeginChild("##log", ImVec2(std::min(30.0f * em, ImGui::GetContentRegionAvail().x + 12.0f * em), 12.0f * em),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    for (const std::string& line : m_log.lines()) {
        const bool error = line.starts_with("error"), warning = line.starts_with("warning");
        if (error || warning)
            ImGui::PushStyleColor(ImGuiCol_Text, error ? IM_COL32(255, 110, 100, 255) : IM_COL32(255, 205, 90, 255));
        ImGui::TextUnformatted(line.c_str());
        if (error || warning) ImGui::PopStyleColor();
    }
    if (atBottom) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// About the section (in the wide layout a caption at the bottom), its controls and numbers, the renderer's frame and
// fallbacks, and the resets.
void DemoGame::sectionPanel(bool withAbout) {
    Slot& slot = m_slots[m_current];
    Section& section = *slot.section;
    if (withAbout) {
        const std::string_view about = section.about();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(about.data(), about.data() + about.size());
        ImGui::PopTextWrapPos();
    }
    // Until the end of the frame and fallbacks the section's settings hold the Graphics panel's values.
    const render::RenderSettings own = section.settings;
    section.settings = graphicsSettings(own);
    if (!slot.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 100, 255));
        ImGui::TextWrapped("Not available: %s", slot.error.c_str());
        ImGui::PopStyleColor();
    } else if (slot.ready) {
        ImGui::SeparatorText("Controls");
        graphicsNote();
        ImGui::PushItemWidth(itemWidth());
        section.controls(*m_context);
        ImGui::PopItemWidth();
        ImGui::SeparatorText("Stats");
        section.stats(*m_context);
    }
    effectsPanel(*m_context, section.settings, currentScene());
    section.settings = takeSectionEdits(own, section.settings);
    ImGui::Separator();
    const bool reset = ImGui::Button("Reset section");
    recordPoint("ui:reset_section");
    ImGui::SameLine();
    if (ImGui::Button("Reset camera")) resetCamera();
    recordPoint("ui:reset_camera");
    if (reset) resetSection(); // last: the section object is replaced
}

void DemoGame::wideLayout(App& app) {
    const app::WindowRect safe = app.safeArea();
    const float em = ImGui::GetFontSize();
    const float margin = 0.6f * em;
    const float left = float(safe.x), top = float(safe.y);
    const float right = float(safe.x + safe.width), bottom = std::min(float(safe.y + safe.height), m_bottomTop);

    ImGui::SetNextWindowPos(ImVec2(left + margin, top + margin), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                        ImVec2(FLT_MAX, std::max(bottom - top - 2.0f * margin, 8.0f * em)));
    ImGui::SetNextWindowBgAlpha(0.8f);
    if (m_focusMenuNow) ImGui::SetNextWindowFocus();
    // Both windows collapse with their title arrow (H and Start are not there on a phone).
    if (ImGui::Begin("Quench Demo", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | navFlags())) {
        sectionList(false);
        ImGui::Separator();
        modeButtons();
        performancePanel(app);
        logPanel();
        ImGui::TextDisabled("Sections: keys 1-9, 0 = 10");
        ImGui::TextDisabled("Previous, next: PgUp PgDn LB RB");
        ImGui::TextDisabled("Menu: Tab, D-pad; back: Esc, B");
        ImGui::TextDisabled("Hide: H, Start; Graphics: G");
    }
    const ImVec2 menuEnd(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                         ImGui::GetWindowPos().y + ImGui::GetWindowSize().y);
    ImGui::End();

    // The section's window at the top right, or under the menu where the screen is too narrow for both; down to the
    // caption or the mode window where it is under them (last frame's), else to the bottom.
    const float width = 18.0f * em;
    const float x = right - margin - width;
    const bool stacked = x < menuEnd.x + margin;
    const float y = stacked ? menuEnd.y + margin : top + margin;
    const bool overBottom = x < m_bottomRight && x + width > m_bottomLeft;
    const float limit = (overBottom ? bottom : float(safe.y + safe.height)) - margin;
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, std::max(limit - y, 6.0f * em)));
    ImGui::SetNextWindowBgAlpha(0.8f);
    const std::string title = std::format("{}###section", current().title());
    m_sectionOpen =
        ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | navFlags());
    if (m_sectionOpen) sectionPanel(false);
    ImGui::End();

    // About and Settings over the middle.
    const float wide = std::min(40.0f * em, float(safe.width) - 4.0f * margin);
    if (m_aboutOpen) {
        ImGui::SetNextWindowPos(ImVec2(left + float(safe.width) * 0.5f, top + float(safe.height) * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(wide, float(safe.height) * 0.85f), ImGuiCond_Always);
        if (m_focusMenuNow) ImGui::SetNextWindowFocus();
        if (ImGui::Begin("About Quench Demo", &m_aboutOpen,
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                             navFlags()))
            aboutContents(app);
        ImGui::End();
    }
    if (m_settingsOpen) {
        ImGui::SetNextWindowPos(ImVec2(left + float(safe.width) * 0.5f, top + float(safe.height) * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(24.0f * em, 0.0f), ImVec2(wide, float(safe.height) * 0.85f));
        if (ImGui::Begin("Settings", &m_settingsOpen,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             navFlags()))
            settingsContents(app);
        ImGui::End();
    }
    if (m_graphicsOpen)
        graphicsWindow(app, ImVec2(menuEnd.x + margin, top + margin),
                       ImVec2((stacked ? right : x) - margin, bottom - margin));
}

void DemoGame::compactLayout(App& app) {
    const app::WindowRect safe = app.safeArea();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();
    const float left = float(safe.x), top = float(safe.y);
    const float width = float(safe.width);
    const float right = left + width, bottom = std::min(float(safe.y + safe.height), m_bottomTop);
    const bool portrait = safe.height > safe.width;
    const auto toggle = [this](Sheet sheet) { m_sheet = m_sheet == sheet ? Sheet::None : sheet; };

    // The bar: the menu, the previous section, the section's title (its controls), the next one, its controls.
    ImGui::SetNextWindowPos(ImVec2(left, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    float barBottom = top;
    if (ImGui::Begin("##bar", nullptr, kSheetFlags | navFlags())) {
        if (ImGui::Button(m_sheet == Sheet::Sections ? "Close" : "Menu")) toggle(Sheet::Sections);
        recordPoint("ui:sections");
        ImGui::SameLine();
        if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) select((m_current + m_slots.size() - 1) % m_slots.size());
        recordPoint("ui:prev");
        ImGui::SameLine();
        const char* controls = m_sheet == Sheet::Controls ? "Close##controls" : "Controls";
        const float tail = ImGui::GetFrameHeight() + ImGui::CalcTextSize("Controls").x + 2.0f * style.FramePadding.x +
                           2.0f * style.ItemSpacing.x;
        const std::string title = std::format("{}##title", current().title());
        if (ImGui::Button(title.c_str(), ImVec2(std::max(ImGui::GetContentRegionAvail().x - tail, 3.0f * em), 0.0f)))
            toggle(Sheet::Controls);
        recordPoint("ui:title");
        ImGui::SameLine();
        if (ImGui::ArrowButton("##next", ImGuiDir_Right)) select((m_current + 1) % m_slots.size());
        recordPoint("ui:next");
        ImGui::SameLine();
        if (ImGui::Button(controls)) toggle(Sheet::Controls);
        recordPoint("ui:controls");
        barBottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
    }
    ImGui::End();
    if (m_aboutOpen && m_sheet != Sheet::About) m_sheet = Sheet::About;
    if (m_settingsOpen && m_sheet != Sheet::Settings && m_sheet != Sheet::About) m_sheet = Sheet::Settings;
    if (m_graphicsOpen && m_sheet == Sheet::None) m_sheet = Sheet::Graphics; // opened in the wide layout
    m_graphicsOpen = m_sheet == Sheet::Graphics;

    const float height = std::max(bottom - barBottom, 6.0f * em);
    switch (m_sheet) {
    case Sheet::None: break;
    case Sheet::Sections: {
        ImGui::SetNextWindowPos(ImVec2(left, barBottom), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(std::min(22.0f * em, width), height), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.9f);
        if (m_focusMenuNow) ImGui::SetNextWindowFocus();
        if (ImGui::Begin("##sections", nullptr, kSheetFlags | navFlags())) {
            // The buttons first: a phone in landscape has no room for the whole list above them.
            modeButtons();
            ImGui::Separator();
            sectionList(true);
            ImGui::Separator();
            performancePanel(app);
            logPanel();
            dragScroll();
        }
        ImGui::End();
        break;
    }
    case Sheet::Controls: {
        // Portrait: a bottom sheet under the scene; landscape: a sheet on the right.
        const ImVec2 size = portrait ? ImVec2(width, std::min(height, float(safe.height) * 0.45f))
                                     : ImVec2(std::min(20.0f * em, width * 0.5f), height);
        ImGui::SetNextWindowPos(portrait ? ImVec2(left, bottom - size.y) : ImVec2(right - size.x, barBottom),
                                ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.85f);
        if (ImGui::Begin("##controls", nullptr, kSheetFlags | navFlags())) {
            sectionPanel(true);
            dragScroll();
        }
        ImGui::End();
        break;
    }
    case Sheet::Graphics: {
        // Docked as the controls are, a little larger: the scene stays in view while options change.
        const ImVec2 size = portrait ? ImVec2(width, std::min(height, float(safe.height) * 0.5f))
                                     : ImVec2(std::min(24.0f * em, width * 0.6f), height);
        graphicsSheet(app, portrait ? ImVec2(left, bottom - size.y) : ImVec2(right - size.x, barBottom), size);
        break;
    }
    case Sheet::About:
    case Sheet::Settings: {
        ImGui::SetNextWindowPos(ImVec2(left, barBottom), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.95f);
        if (ImGui::Begin(m_sheet == Sheet::About ? "##about" : "##settings", nullptr, kSheetFlags | navFlags())) {
            if (ImGui::Button("Close")) {
                m_aboutOpen = m_settingsOpen = false;
                m_sheet = Sheet::None;
            }
            recordPoint("ui:close");
            if (m_sheet == Sheet::About)
                aboutContents(app);
            else if (m_sheet == Sheet::Settings)
                settingsContents(app);
            dragScroll();
        }
        ImGui::End();
        break;
    }
    }
}

// A drag that starts on a sheet's background (not on an item) scrolls it, as a finger does on a phone.
void DemoGame::dragScroll() {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        m_dragScrolling = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsAnyItemHovered();
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_dragScrolling = false;
    if (m_dragScrolling && io.MouseDelta.y != 0.0f) ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
}

void DemoGame::aboutContents(App& app) {
    const gpu::Caps& caps = app.device().caps();
    // The licences ship as assets (LICENSE, THIRD_PARTY_LICENSES), read once when About first opens.
    for (const auto& [path, text] :
         {std::pair("LICENSE", &m_licence), std::pair("THIRD_PARTY_LICENSES", &m_thirdParty)})
        if (text->empty()) {
            const Result<std::string> read = fs::readAssetText(path);
            *text = read ? *read : std::format("{} is missing: {}", path, read.error());
            if (!read) log::warning("demo", "about: {} not read: {}", path, read.error());
        }
    if (ImGui::BeginTabBar("##about")) {
        if (ImGui::BeginTabItem("About")) {
            ImGui::Text("Quench Demo %s", QUENCH_GAME_VERSION);
            ImGui::TextWrapped("Build: %s, commit %s", QUENCH_DEMO_CONFIG, QUENCH_DEMO_COMMIT);
            ImGui::TextWrapped("Platform: %s", SDL_GetPlatform());
            ImGui::TextWrapped("GPU: %s backend, driver %s, %s", gpu::backendName(caps.backend), caps.driver.c_str(),
                               caps.deviceName.c_str());
            ImGui::TextWrapped("Ray tracing %s, MetalFX %s, GPU timing %s", caps.rayTracing ? "yes" : "no",
                               caps.temporalUpscaling || caps.spatialUpscaling ? "yes" : "no",
                               caps.gpuTiming ? "yes" : "no");
            ImGui::TextWrapped("Window %d x %d points, %.2f pixels per point; display headroom %.2f", app.windowWidth(),
                               app.windowHeight(), double(app.pixelDensity()), double(app.displayHeadroom()));
            if (const audio::Device* device = app.audioDevice())
                ImGui::TextWrapped("Audio: %s (%s)", device->info().name.c_str(), device->info().backend.c_str());
            else
                ImGui::TextUnformatted("Audio: offline mix (no device)");
            ImGui::TextWrapped("Jobs: %d threads", app.jobs().threadCount());
            ImGui::TextWrapped("Thermal state: %s (from fair on, the system lowers clocks and frame times grow)",
                               app::thermalStateName(app.stats().thermal));
            ImGui::SeparatorText("GPU backend");
            if (gpu::defaultBackend() == gpu::Backend::Metal)
                ImGui::TextWrapped("The native Metal backend is the default here: GPU times per pass, ray tracing and "
                                   "MetalFX. SDL GPU (--gpu-backend sdl) is the portable one, the default on other "
                                   "platforms.");
            else
                ImGui::TextWrapped("SDL GPU is the default here; the native Metal backend (Apple platforms only) adds "
                                   "GPU times per pass, ray tracing and MetalFX.");
            if (caps.backend != gpu::defaultBackend())
                ImGui::TextWrapped("This run is not on the default: chosen at launch (--gpu-backend, "
                                   "QUENCH_GPU_BACKEND, --gpu-driver), or the native backend did not start (the log "
                                   "says why).");
            relaunchButtons(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controls")) {
            controlsHelp();
            ImGui::EndTabItem();
        }
        for (int i = 0; i < 2; ++i) {
            if (!ImGui::BeginTabItem(i == 0 ? "Licence" : "Third-party licences")) continue;
            const std::string& text = i == 0 ? m_licence : m_thirdParty;
            ImGui::BeginChild(i == 0 ? "##licence" : "##third", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(text.data(), text.data() + text.size());
            ImGui::PopTextWrapPos();
            dragScroll();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void DemoGame::settingsContents(App& app) {
    if (!m_scaleEditing) m_editScale = m_settings.uiScale;
    ImGui::SetNextItemWidth(itemWidth());
    ImGui::SliderFloat("UI scale", &m_editScale, 0.75f, 2.0f, "x %.2f");
    m_scaleEditing = ImGui::IsItemActive();
    // Applied when the slider is let go: a scale under the finger would move the slider.
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        m_settings.uiScale = m_editScale;
        applyStyle();
        noteSettings();
    }
    int touch = int(m_settings.touch);
    ImGui::TextUnformatted("Touch-sized controls");
    bool changed = ImGui::RadioButton("auto##touch", &touch, int(TouchControls::Auto));
    ImGui::SameLine();
    changed |= ImGui::RadioButton("on##touch", &touch, int(TouchControls::On));
    ImGui::SameLine();
    changed |= ImGui::RadioButton("off##touch", &touch, int(TouchControls::Off));
    if (changed) {
        m_settings.touch = TouchControls(touch);
        applyStyle();
        noteSettings();
    }
    outputControls(app);
    ImGui::SetNextItemWidth(itemWidth());
    if (ImGui::SliderFloat("Tour time", &m_tourSeconds, 2.0f, 30.0f, "%.0f s")) noteSettings();
    if (ImGui::Checkbox("Adapt to heat", &m_settings.adaptToHeat)) {
        applyAdaptToHeat(app);
        noteSettings();
    }
    recordPoint("ui:adapt_heat");
    const app::ThermalGovernorDesc heat;
    ImGui::TextWrapped("Thermal state serious: %s. Critical: %s (fewer traced effects). Lifted after %.0f s cooler.",
                       app::thermalLimitsText(heat.serious).c_str(), app::thermalLimitsText(heat.critical).c_str(),
                       m_heatRecoverSeconds);
    if (ImGui::Button("Defaults")) {
        const std::string section = m_settings.section;
        m_settings = Settings{};
        m_settings.section = section;
        m_tourSeconds = m_settings.tourSeconds;
        app.setHdrMode(app::HdrMode::Off);
        app.setPresentMode(gpu::PresentMode::Vsync);
        applyAdaptToHeat(app);
        applyStyle();
        noteSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Screenshot")) {
        const std::string name = std::format("screenshot-{}.png", ++m_screenshots);
        app.requestCapture(name);
        m_modeMessage = std::format("Screenshot: {} in the user directory", name);
    }
    if (m_settingsEnabled)
        ImGui::TextWrapped("Saved to %s in the user directory, with the last section.", m_settingsFile.c_str());
    else
        ImGui::TextWrapped("Not saved in this run (%s).", m_flags.noSettings ? "--no-settings" : "a scripted run");
}

void DemoGame::bottomWindowPlaced() {
    m_bottomTop = ImGui::GetWindowPos().y;
    m_bottomLeft = ImGui::GetWindowPos().x;
    m_bottomRight = m_bottomLeft + ImGui::GetWindowSize().x;
}

void DemoGame::bottomWindows(App& app) {
    const app::WindowRect safe = app.safeArea();
    const float em = ImGui::GetFontSize();
    const float margin = compact() ? 0.0f : 0.6f * em;
    m_bottomTop = 1.0e9f;
    m_bottomLeft = m_bottomRight = 0.0f;
    if (m_mode == Mode::None) {
        // What the section shows, as a caption at the bottom; collapsing the section window hides it too.
        if (compact() || !m_sectionOpen) return;
        ImGui::SetNextWindowPos(ImVec2(float(safe.x) + float(safe.width) * 0.5f, float(safe.y + safe.height) - margin),
                                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("##caption", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        const std::string_view about = current().about();
        ImGui::PushTextWrapPos(std::min(34.0f * em, float(safe.width) - 4.0f * margin));
        ImGui::TextUnformatted(about.data(), about.data() + about.size());
        ImGui::PopTextWrapPos();
        bottomWindowPlaced();
        ImGui::End();
        return;
    }
    const float width = compact() ? float(safe.width) : std::min(36.0f * em, float(safe.width) - 2.0f * margin);
    ImGui::SetNextWindowPos(ImVec2(float(safe.x) + float(safe.width) * 0.5f, float(safe.y + safe.height) - margin),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("##mode", nullptr, kSheetFlags | ImGuiWindowFlags_AlwaysAutoResize | navFlags())) {
        const std::size_t n = m_slots.size();
        if (m_mode == Mode::Tour) {
            ImGui::TextWrapped("Tour: %.*s (%zu of %zu), next in %.0f s. Any input stops it.",
                               int(current().title().size()), current().title().data(), m_current + 1, n,
                               std::max(0.0, double(m_tourSeconds) - m_modeTime));
            if (ImGui::Button("Stop")) stopMode("stopped");
            recordPoint("ui:stop");
        } else if (m_mode == Mode::Benchmark) {
            ImGui::TextWrapped("Benchmark: %.*s (%zu of %zu), %s", int(current().title().size()),
                               current().title().data(), m_benchmarkIndex + 1, n,
                               m_benchmarkMeasuring ? "measuring" : "warming up");
            ImGui::ProgressBar(float(m_benchmarkIndex) / float(n), ImVec2(-1.0f, 0.0f));
            if (ImGui::Button("Stop")) stopMode("stopped");
            recordPoint("ui:stop");
        } else if (m_mode == Mode::Checks) {
            checksWindow(app);
        }
        bottomWindowPlaced();
    }
    ImGui::End();
}

} // namespace demo
