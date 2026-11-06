// The engine demo: a menu of sections, each a small interactive scene of one engine feature (sections/), and the app
// around them (DemoGame.h): sections, input focus, settings and the scenario hooks. Scripted checks:
// examples/demo/scenarios; docs/demo.md.

#include "DemoGame.h"

#include <quench/app/Scenario.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Log.h>

#include <demo_checker_shaders.h>

#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_touch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

namespace demo {

using namespace quench;
using app::App;
using app::Binding;
using app::GamepadButton;

namespace {

double elapsedMs(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

std::vector<std::string_view> splitWords(std::string_view line) {
    std::vector<std::string_view> words;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ') ++i;
        if (i > start) words.push_back(line.substr(start, i - start));
    }
    return words;
}

bool directTouchDevice() {
    int count = 0;
    SDL_TouchID* devices = SDL_GetTouchDevices(&count);
    bool direct = false;
    for (int i = 0; i < count; ++i)
        if (SDL_GetTouchDeviceType(devices[i]) == SDL_TOUCH_DEVICE_DIRECT) direct = true;
    SDL_free(devices);
    return direct;
}

} // namespace

LogLines::LogLines() {
    m_sink = log::addSink([this](log::Level level, std::string_view category, std::string_view message) {
        if (level == log::Level::Debug) return;
        if (level == log::Level::Error) m_errors.fetch_add(1, std::memory_order_relaxed);
        if (level == log::Level::Warning) m_warnings.fetch_add(1, std::memory_order_relaxed);
        const std::lock_guard lock(m_mutex);
        m_lines.push_back(std::format("{} {}: {}", log::levelName(level), category, message));
        if (m_lines.size() > kLines) m_lines.pop_front();
    });
}

LogLines::~LogLines() { log::removeSink(m_sink); }

std::vector<std::string> LogLines::lines() const {
    const std::lock_guard lock(m_mutex);
    return {m_lines.begin(), m_lines.end()};
}

DemoGame::DemoGame(DemoFlags flags) : m_flags(std::move(flags)) {}

DemoGame::~DemoGame() = default;

Result<void> DemoGame::init(App& app) {
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    // The demo's keys keep working while a window has the keyboard focus (they avoid the navigation keys).
    io.ConfigNavCaptureKeyboard = false;
    m_baseStyle = ImGui::GetStyle();

    Result<render::Renderer> renderer = render::Renderer::create(app.device());
    if (!renderer) return Error{std::format("renderer: {}", renderer.error())};
    m_renderer.emplace(std::move(*renderer));
    render::SurfaceId checker{};
    Result<render::SurfaceId> surface = m_renderer->addSurface({
        .vertexShader = shaders::demo_checker.find("surface_vs"),
        .fragmentShader = shaders::demo_checker.find("surface_fs"),
        .basicFragmentShader = shaders::demo_checker.find("surface_basic_fs"),
        .name = "checker",
    });
    if (surface)
        checker = *surface;
    else
        log::warning("demo", "checker surface: {}; grounds use the standard surface", surface.error());
    m_context.emplace(Context{app, *m_renderer, checker});
    m_emptyScene.emplace(app.device());

    for (const SectionFactory make :
         {makeMaterialsSection, makeShadowsSection, makeSkySection, makePostSection, makeHdrSection, makePhysicsSection,
          makeInputSection, makeAssetsSection, makeAudioSection, makeAnimationSection, makeRayTracingSection,
          makeTunnelSection}) {
        Slot slot{.make = make, .section = make()};
        slot.home = slot.section->camera;
        m_slots.push_back(std::move(slot));
    }

    app::Input& input = app.input();
    m_next = action(input, "next_section",
                    {*Binding::key("]"), *Binding::key("PageDown"), Binding::gamepad(GamepadButton::RightShoulder)});
    m_prev = action(input, "prev_section",
                    {*Binding::key("["), *Binding::key("PageUp"), Binding::gamepad(GamepadButton::LeftShoulder)});
    // Keys 1 to 9, then 0 for the tenth; later sections through the menu and [ ].
    for (std::size_t i = 0; i < m_slots.size() && i < 10; ++i)
        m_direct.push_back(
            action(input, std::format("section_{}", i + 1), {*Binding::key(std::to_string((i + 1) % 10))}));
    m_toggleUi = action(input, "toggle_ui", {*Binding::key("H"), Binding::gamepad(GamepadButton::Start)});
    m_toggleGraphics = action(input, "toggle_graphics", {*Binding::key("G")});
    m_camera.init(input);

    loadSettings(app);
    std::size_t start = 0;
    if (!m_flags.section.empty()) {
        const std::optional<std::size_t> found = find(m_flags.section);
        if (!found) return Error{std::format("--section: no section '{}' (--help lists them)", m_flags.section)};
        start = *found;
    } else if (const std::optional<std::size_t> saved = find(m_settings.section)) {
        start = *saved;
    }
    m_lastHdr = app.hdrMode();
    m_touchScreen = directTouchDevice();
    applyStyle();
    select(start);
    log::info("demo", "{} sections, starting with '{}'", m_slots.size(), current().id());

    if (m_flags.checks)
        startChecks();
    else if (m_flags.benchmark)
        startBenchmark(*m_flags.benchmark, true);
    else if (m_flags.tour)
        startTour(*m_flags.tour);
    return {};
}

void DemoGame::fixedStep(App& app, double dt) {
    (void)app;
    if (currentReady()) current().fixedStep(*m_context, dt);
}

void DemoGame::update(App& app, double frameDt) {
    const app::Input& input = app.input();
    // A touch screen attached later (scripted fingers on a Mac are not one: the layout must not move under them).
    if (!m_touchScreen && !input.fingers().empty() && input.fingers()[0].pressed && directTouchDevice()) {
        m_touchScreen = true;
        applyStyle();
    }
    updateFocus(app);
    m_context->sceneInput = !m_menuFocus;

    // Any press or a moved stick stops the tour.
    bool pressed = false;
    for (int code = 0; code < SDL_SCANCODE_COUNT && !pressed; ++code) pressed = input.keyPressed(code);
    for (int b = 0; b < int(app::MouseButton::Count) && !pressed; ++b)
        pressed = input.mousePressed(app::MouseButton(b));
    for (const app::Finger& f : input.fingers()) pressed = pressed || f.pressed;
    for (int b = 0; b < int(GamepadButton::Count) && !pressed; ++b) pressed = input.gamepadPressed(GamepadButton(b));
    for (int a = 0; a < 4 && !pressed; ++a) pressed = std::abs(input.gamepadAxis(app::GamepadAxis(a))) > 0.5f;
    if (pressed && m_mode == Mode::Tour) stopMode("stopped by input");

    if (input.pressed(m_next)) select((m_current + 1) % m_slots.size());
    if (input.pressed(m_prev)) select((m_current + m_slots.size() - 1) % m_slots.size());
    for (std::size_t i = 0; i < m_direct.size(); ++i)
        if (input.pressed(m_direct[i])) select(i);
    if (input.pressed(m_toggleUi)) {
        m_showUi = !m_showUi;
        m_uiReappears = m_showUi;
        noteSettings();
    }
    if (input.pressed(m_toggleGraphics)) openGraphics(true);
    m_camera.update(app, current(), float(frameDt), !m_menuFocus, [this](glm::vec2 point) { click(point); });

    updateMode(app, frameDt);
    if (currentReady()) {
        current().sectionTime += frameDt;
        current().update(*m_context, frameDt);
    }
    if (const render::RenderSettings settings = graphicsSettings(current().settings);
        !(m_renderer->settings() == settings))
        m_renderer->setSettings(settings);

    if (app.hdrMode() != m_lastHdr) {
        // Changed in the HDR section or in the settings (not by --hdr, which only applies to its run).
        m_lastHdr = app.hdrMode();
        m_settings.hdr = app::hdrModeName(m_lastHdr);
        noteSettings();
    }
    m_frameTimes[m_frameTimeNext] = float(frameDt * 1000.0);
    m_frameTimeNext = (m_frameTimeNext + 1) % m_frameTimes.size();
    updateJobTest(app);
    saveSettings(false);
}

void DemoGame::render(App& app, app::Frame& frame) {
    (void)app;
    Section& section = current();
    const float aspect = float(frame.width) / float(std::max(frame.height, 1u));
    const render::View view{.view = section.camera.view(),
                            .fovY = section.camera.fovFor(aspect),
                            .nearPlane = 0.1f,
                            .cut = m_cut,
                            .time = section.sectionTime,
                            .probeCenter = section.camera.target};
    // The Graphics panel's exposure scales the section's own for this frame only.
    render::Scene& scene = currentScene();
    const render::Environment environment = scene.environment();
    const float exposure = graphicsExposure();
    if (exposure != 1.0f) {
        render::Environment scaled = environment;
        scaled.exposure *= exposure;
        scene.setEnvironment(scaled);
    }
    m_renderer->render(scene, view, frame.commands, frame.target, section.output(frame.output));
    if (exposure != 1.0f) scene.setEnvironment(environment);
    m_targetWidth = frame.width;
    m_cut = false;
}

void DemoGame::shutdown(App& app) {
    if (m_jobTest && m_jobTest->running) app.jobs().wait(m_jobTest->job);
    m_jobTest.reset();
    saveSettings(true);
    if (m_entered && currentReady()) current().leave(*m_context);
    for (Slot& slot : m_slots)
        if (slot.ready) slot.section->shutdown(*m_context);
    m_slots.clear();
    m_emptyScene.reset();
    m_context.reset();
    m_renderer.reset();
}

render::Scene& DemoGame::currentScene() { return currentReady() ? current().scene() : *m_emptyScene; }

std::optional<std::size_t> DemoGame::find(std::string_view id) const {
    for (std::size_t i = 0; i < m_slots.size(); ++i)
        if (m_slots[i].section->id() == id) return i;
    return std::nullopt;
}

bool DemoGame::prepare(Slot& slot) {
    if (slot.ready || !slot.error.empty()) return slot.ready;
    const auto start = std::chrono::steady_clock::now();
    if (Result<void> r = slot.section->init(*m_context); !r) {
        slot.error = r.error();
        log::error("demo", "section {}: {}", slot.section->id(), r.error());
        return false;
    }
    slot.ready = true;
    slot.initMs = elapsedMs(start);
    log::info("demo", "section {} ready in {:.1f} ms", slot.section->id(), slot.initMs);
    return true;
}

void DemoGame::select(std::size_t index) {
    if (m_entered && index == m_current) return;
    const auto start = std::chrono::steady_clock::now();
    if (m_entered && currentReady()) current().leave(*m_context);
    m_current = index;
    m_entered = true;
    if (prepare(m_slots[index])) current().enter(*m_context);
    m_renderer->setSettings(graphicsSettings(current().settings));
    m_cut = true; // another scene: no motion vectors from the previous one
    m_camera.reset();
    m_switchMs = elapsedMs(start);
    if (m_settings.section != current().id()) {
        m_settings.section = std::string(current().id());
        noteSettings();
    }
}

// The section starts again as on its first visit: a new object of its kind, initialized and entered; the camera
// stays where it is.
void DemoGame::resetSection() {
    Slot& slot = m_slots[m_current];
    const OrbitCamera camera = slot.section->camera;
    if (slot.ready) {
        slot.section->leave(*m_context);
        slot.section->shutdown(*m_context);
    }
    slot.section = slot.make();
    slot.section->camera = camera;
    slot.ready = false;
    slot.error.clear();
    if (prepare(slot)) slot.section->enter(*m_context);
    m_renderer->setSettings(graphicsSettings(slot.section->settings));
    m_cut = true;
    m_camera.reset();
}

void DemoGame::resetCamera() {
    current().camera = m_slots[m_current].home;
    m_cut = true;
}

void DemoGame::click(glm::vec2 point) {
    if (m_context->app.input().uiCapturesMouse()) return;
    ++m_clicks;
    if (currentReady()) current().click(*m_context, point);
}

// Tab or the D-pad hand the keyboard and the gamepad to the windows (arrows, the D-pad and the left stick move
// between items, Enter or A press them); Escape, B or a click on the scene hand them back to the camera.
void DemoGame::updateFocus(App& app) {
    const app::Input& input = app.input();
    const ImGuiIO& io = ImGui::GetIO();
    if (!m_menuFocus) {
        const bool dpad =
            input.gamepadPressed(GamepadButton::DpadUp) || input.gamepadPressed(GamepadButton::DpadDown) ||
            input.gamepadPressed(GamepadButton::DpadLeft) || input.gamepadPressed(GamepadButton::DpadRight);
        if ((input.keyPressed(SDL_SCANCODE_TAB) && !io.WantTextInput) || dpad) {
            m_menuFocus = true;
            m_focusMenuNow = true;
            if (!m_showUi) {
                m_showUi = true;
                noteSettings();
            }
            if (compact() && m_sheet == Sheet::None) m_sheet = Sheet::Sections;
        }
        return;
    }
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool back = (input.keyPressed(SDL_SCANCODE_ESCAPE) && !io.WantTextInput) ||
                      (input.gamepadPressed(GamepadButton::East) && !popup);
    const bool sceneClick = !input.uiCapturesMouse() &&
                            (input.mousePressed(app::MouseButton::Left) || input.mousePressed(app::MouseButton::Right));
    if (back || sceneClick || !m_showUi) {
        m_menuFocus = false;
        ImGui::SetWindowFocus(nullptr);
    }
}

// Generic `set` names: the renderer settings of the current section.
bool DemoGame::setRenderSetting(std::string_view name, std::string_view value) {
    render::RenderSettings& s = current().settings;
    const std::optional<float> number = parseFloat(value);
    const std::optional<bool> flag = parseBool(value);
    if (name == "aa") {
        if (value == "none")
            s.antiAliasing = render::AntiAliasing::None;
        else if (value == "fxaa")
            s.antiAliasing = render::AntiAliasing::Fxaa;
        else if (value == "taa")
            s.antiAliasing = render::AntiAliasing::Taa;
        else
            return false;
    } else if (name == "tone") {
        if (value == "aces")
            s.toneCurve = render::ToneCurve::Aces;
        else if (value == "agx")
            s.toneCurve = render::ToneCurve::Agx;
        else
            return false;
    } else if (name == "cascades" && number) {
        s.shadowCascades = std::clamp(int(*number), 0, render::kMaxCascades);
    } else if (name == "shadow_size" && number) {
        s.shadowMapSize = std::uint32_t(std::clamp(*number, 256.0f, 4096.0f));
    } else if (name == "shadow_filter" && number) {
        s.shadowFilter = std::clamp(int(*number), 0, 2);
    } else if (name == "shadow_distance" && number) {
        s.shadowDistance = std::max(*number, 1.0f);
    } else if (name == "translucent_shadows") {
        if (value == "map")
            s.translucentShadows = render::TranslucentShadows::Map;
        else if (value == "dithered")
            s.translucentShadows = render::TranslucentShadows::Dithered;
        else
            return false;
    } else if (name == "scale" && number) {
        s.renderScale = std::clamp(*number, 0.25f, 1.0f);
    } else if (name == "sun_shadows") {
        if (value == "maps")
            s.sunShadows = render::SunShadows::ShadowMaps;
        else if (value == "rt")
            s.sunShadows = render::SunShadows::RayTraced;
        else
            return false;
    } else if (name == "shadow_rays" && number) {
        s.shadowRays = std::clamp(int(*number), 1, 8);
    } else if (name == "shadow_softness" && number) {
        s.shadowSoftness = std::max(*number, 0.0f);
    } else if (name == "ao") {
        if (value == "none")
            s.ambientOcclusion = render::AmbientOcclusion::None;
        else if (value == "rt")
            s.ambientOcclusion = render::AmbientOcclusion::RayTraced;
        else
            return false;
    } else if (name == "ao_rays" && number) {
        s.occlusionRays = std::clamp(int(*number), 1, 8);
    } else if (name == "ao_radius" && number) {
        s.occlusionRadius = std::max(*number, 0.05f);
    } else if (name == "upscaling") {
        if (value == "bilinear")
            s.upscaling = render::Upscaling::Bilinear;
        else if (value == "spatial")
            s.upscaling = render::Upscaling::Spatial;
        else if (value == "temporal")
            s.upscaling = render::Upscaling::Temporal;
        else if (value == "denoised")
            s.upscaling = render::Upscaling::TemporalDenoised;
        else
            return false;
    } else if (name == "local_shadows" && number) {
        s.localLightShadows = std::clamp(int(*number), 0, render::kMaxRayLocalShadows);
    } else if (name == "reflections") {
        if (value == "sky")
            s.reflections = render::Reflections::Sky;
        else if (value == "rt")
            s.reflections = render::Reflections::RayTraced;
        else
            return false;
    } else if (name == "reflection_roughness" && number) {
        s.reflectionRoughness = std::clamp(*number, 0.0f, 1.0f);
    } else if (name == "indirect") {
        if (value == "sky")
            s.indirectLight = render::IndirectLight::Sky;
        else if (value == "probes")
            s.indirectLight = render::IndirectLight::Probes;
        else if (value == "rt")
            s.indirectLight = render::IndirectLight::RayTraced;
        else
            return false;
    } else if (name == "probe_spacing" && number) {
        s.probeSpacing = std::clamp(*number, 0.25f, 16.0f);
    } else if (name == "denoiser") {
        if (value == "spatial")
            s.rayDenoiser = render::RayDenoiser::Spatial;
        else if (value == "temporal")
            s.rayDenoiser = render::RayDenoiser::Temporal;
        else
            return false;
    } else if (name == "path_tracer" && flag) {
        s.pathTracer = *flag;
    } else if (name == "path_bounces" && number) {
        s.pathBounces = std::clamp(int(*number), 1, 16);
    } else if (name == "bloom" && flag) {
        s.bloom = *flag;
    } else if (name == "bloom_strength" && number) {
        s.bloomStrength = std::max(*number, 0.0f);
    } else if (name == "sky" && flag) {
        s.sky = *flag;
    } else if (name == "dither" && flag) {
        s.dither = *flag;
    } else if (name == "sharpen" && number) {
        s.sharpen = std::clamp(*number, 0.0f, 1.0f);
    } else if (name == "vignette" && number) {
        s.vignette = std::clamp(*number, 0.0f, 1.0f);
    } else {
        return false;
    }
    return true;
}

bool DemoGame::runLine(std::string_view line) {
    const std::vector<std::string_view> w = splitWords(line);
    if (w.empty()) return true;
    if (w[0] == "set" && w.size() == 3)
        return setRenderSetting(w[1], w[2]) || (currentReady() && current().set(*m_context, w[1], w[2]));
    if (w[0] == "camera" && w.size() == 4) {
        const std::optional<float> yaw = parseFloat(w[1]), pitch = parseFloat(w[2]), distance = parseFloat(w[3]);
        if (!yaw || !pitch || !distance) return false;
        OrbitCamera& camera = current().camera;
        camera.yaw = glm::radians(*yaw);
        camera.pitch = glm::radians(*pitch);
        camera.distance = std::clamp(*distance, camera.minDistance, camera.maxDistance);
        return true;
    }
    if (w[0] == "hdr" && w.size() == 2) {
        const app::HdrMode mode = w[1] == "on"     ? app::HdrMode::On
                                  : w[1] == "auto" ? app::HdrMode::Auto
                                                   : app::HdrMode::Off;
        m_context->app.setHdrMode(mode);
        return true;
    }
    return false;
}

// ---- settings

void DemoGame::loadSettings(App& app) {
    m_settingsEnabled = !m_flags.noSettings && (!app.scripted() || !m_flags.settingsFile.empty());
    m_settingsFile = m_flags.settingsFile.empty() ? "settings.json" : m_flags.settingsFile;
    if (!m_settingsEnabled) {
        log::info("demo", "settings: defaults ({})", m_flags.noSettings ? "--no-settings" : "scripted run");
        return;
    }
    if (!fs::userExists(m_settingsFile)) {
        log::info("demo", "settings: no {} yet, defaults", m_settingsFile);
        return;
    }
    const Result<std::string> text = fs::readUserText(m_settingsFile);
    Result<Settings> parsed = text ? parseSettings(*text) : Result<Settings>(Error{text.error()});
    if (!parsed) {
        m_settingsLoaded = -1;
        log::warning("demo", "settings: {} not used ({}); defaults", m_settingsFile, parsed.error());
        return;
    }
    m_settings = *parsed;
    m_settingsLoaded = 1;
    m_showUi = m_settings.showUi;
    m_tourSeconds = m_settings.tourSeconds;
    // The command line wins over the saved options.
    const std::span<const std::string> args = app.arguments();
    const auto given = [&](std::string_view flag) {
        return std::ranges::any_of(
            args, [&](const std::string& a) { return a == flag || a.starts_with(std::string(flag) + "="); });
    };
    if (!m_settings.hdr.empty() && !given("--hdr"))
        app.setHdrMode(m_settings.hdr == "on"     ? app::HdrMode::On
                       : m_settings.hdr == "auto" ? app::HdrMode::Auto
                                                  : app::HdrMode::Off);
    if (!m_settings.present.empty() && !given("--present-mode"))
        app.setPresentMode(m_settings.present == "immediate" ? gpu::PresentMode::Immediate
                           : m_settings.present == "mailbox" ? gpu::PresentMode::Mailbox
                                                             : gpu::PresentMode::Vsync);
    log::info("demo", "settings: {} (section {}, UI scale {:.2f}, touch {})", m_settingsFile,
              m_settings.section.empty() ? "-" : m_settings.section, m_settings.uiScale,
              touchControlsName(m_settings.touch));
}

void DemoGame::noteSettings() { m_settingsDirty = true; }

// Written a moment after a change (a slider dragged writes once), and at the end.
void DemoGame::saveSettings(bool now) {
    if (!m_settingsEnabled || !m_settingsDirty) return;
    const double t = m_context ? m_context->app.time() : 0.0;
    if (!now && m_settingsSavedAt >= 0.0 && t - m_settingsSavedAt < 0.5) return;
    m_settings.showUi = m_showUi;
    m_settings.tourSeconds = m_tourSeconds;
    if (Result<void> r = fs::writeUserText(m_settingsFile, writeSettings(m_settings)); !r) {
        log::warning("demo", "settings: {} not written: {}", m_settingsFile, r.error());
        m_settingsEnabled = false; // once: no line every frame
        return;
    }
    m_settingsDirty = false;
    m_settingsSavedAt = t;
    ++m_settingsWrites;
}

// ---- the job test of the Performance panel

void DemoGame::startJobTest(App& app) {
    if (m_jobTest && m_jobTest->running) return;
    m_jobTest = std::make_unique<JobTest>();
    JobTest* test = m_jobTest.get();
    JobSystem& jobs = app.jobs();
    test->start = std::chrono::steady_clock::now();
    test->running = true;
    test->job = jobs.parallelFor(256, 1, [test, &jobs](std::uint32_t begin, std::uint32_t end) {
        // Some 50 microseconds of arithmetic per item.
        volatile double sink = 0.0;
        for (std::uint32_t i = begin; i < end; ++i)
            for (int k = 0; k < 20000; ++k) sink = sink + std::sqrt(double(i) * 20000.0 + double(k));
        const int thread = std::clamp(jobs.threadIndex(), 0, int(test->ranges.size()) - 1);
        test->ranges[std::size_t(thread)].fetch_add(int(end - begin), std::memory_order_relaxed);
        const std::int64_t ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - test->start)
                .count();
        std::int64_t last = test->endNs.load(std::memory_order_relaxed);
        while (ns > last && !test->endNs.compare_exchange_weak(last, ns, std::memory_order_relaxed)) {}
    });
}

void DemoGame::updateJobTest(App& app) {
    if (!m_jobTest || !m_jobTest->running) return;
    if (app.jobs().workerCount() == 0) app.jobs().runFor(0.004);
    if (!m_jobTest->job.done()) return;
    m_jobTest->running = false;
    m_jobTest->ms = double(m_jobTest->endNs.load(std::memory_order_relaxed)) / 1.0e6;
}

// ---- scenario hooks

std::optional<double> DemoGame::scenarioValue(std::string_view name) const {
    const render::RenderStats& s = m_renderer->stats();
    const render::ActiveEffects& e = m_renderer->effects();
    if (name == "section") return double(m_current);
    if (name == "sections") return double(m_slots.size());
    if (name == "ui") return m_showUi ? 1.0 : 0.0;
    if (name == "aa") return double(int(e.antiAliasing));
    if (name == "cascades") return double(e.shadowCascades);
    if (name == "bloom") return e.bloom ? 1.0 : 0.0;
    if (name == "draws") return double(s.draws);
    if (name == "instances") return double(s.instances);
    if (name == "culled") return double(s.culled);
    if (name == "shadow_draws") return double(s.shadowDraws);
    if (name == "shadow_instances") return double(s.shadowInstances);
    if (name == "transmittance_draws") return double(s.transmittanceDraws);
    if (name == "translucent_shadows") return double(int(e.translucentShadows));
    if (name == "moved") return double(s.movedInstances);
    if (name == "lights") return double(s.lights);
    if (name == "area_lights") return double(s.areaLights);
    if (name == "local_shadow_changes") return double(s.localShadowChanges);
    if (name == "skinned_instances") return double(s.skinnedInstances);
    if (name == "skinned_vertices") return double(s.skinnedVertices);
    if (name == "render_w") return double(s.renderWidth);
    if (name == "render_h") return double(s.renderHeight);
    if (name == "render_scale") return m_targetWidth > 0 ? double(s.renderWidth) / double(m_targetWidth) : 0.0;
    if (name == "sun_shadows") return double(int(e.sunShadows));
    if (name == "ambient_occlusion") return double(int(e.ambientOcclusion));
    if (name == "upscaling") return double(int(e.upscaling));
    if (name == "ray_instances") return double(s.rayInstances);
    if (name == "ray_builds") return double(s.rayBuilds);
    if (name == "ray_refits") return double(s.rayRefits);
    if (name == "ray_compactions") return double(s.rayCompactions);
    if (name == "ray_top_refit") return s.rayTopRefit ? 1.0 : 0.0;
    if (name == "ray_memory_mb") return double(s.rayMemoryBytes) / (1024.0 * 1024.0);
    if (name == "hit_textures") return double(s.hitTextures);
    if (name == "local_shadows") return double(e.localLightShadows);
    if (name == "reflections") return double(int(e.reflections));
    if (name == "indirect") return double(int(e.indirectLight));
    if (name == "probes") return double(s.probes);
    if (name == "denoiser") return double(int(e.rayDenoiser));
    if (name == "path_tracer") return e.pathTracer ? 1.0 : 0.0;
    if (name == "path_samples") return double(s.pathSamples);
    if (name == "fallbacks") return double(m_renderer->fallbacks().size());
    if (name == "camera_yaw") return double(glm::degrees(current().camera.yaw));
    if (name == "camera_pitch") return double(glm::degrees(current().camera.pitch));
    if (name == "camera_distance") return double(current().camera.distance);
    if (name == "camera_x") return double(current().camera.target.x);
    if (name == "camera_z") return double(current().camera.target.z);
    if (name == "clicks") return double(m_clicks);
    // The demo around the sections.
    if (name == "section_ready") return currentReady() ? 1.0 : 0.0;
    if (name == "sections_ready")
        return double(std::ranges::count_if(m_slots, [](const Slot& slot) { return slot.ready; }));
    if (name == "init_ms") return m_slots[m_current].initMs;
    if (name == "switch_ms") return m_switchMs;
    if (name == "scene_instances")
        return currentReady() ? double(m_slots[m_current].section->scene().instanceCount()) : 0.0;
    if (name == "compact") return compact() ? 1.0 : 0.0;
    if (name == "sheet") return double(int(m_sheet));
    if (name == "menu_focus") return m_menuFocus ? 1.0 : 0.0;
    if (name == "about") return m_aboutOpen ? 1.0 : 0.0;
    if (name == "licence_bytes") return double(m_licence.size() + m_thirdParty.size());
    if (name == "ui_scale") return double(m_settings.uiScale);
    if (name == "touch_ui") return m_appliedTouch ? 1.0 : 0.0;
    if (name == "settings_loaded") return double(m_settingsLoaded);
    if (name == "settings_writes") return double(m_settingsWrites);
    if (name == "mode") return double(int(m_mode));
    if (name == "tours") return double(m_toursDone);
    if (name == "check_index") return double(m_checkIndex);
    if (name == "check_items") return double(checkItems().size());
    if (name == "check_writes") return double(m_checkWrites);
    if (name.starts_with("checks_")) {
        const std::string_view status = name.substr(7);
        return double(
            std::ranges::count_if(m_checks, [&](const CheckResult& r) { return checkStatusName(r.status) == status; }));
    }
    if (name == "benchmark_sections") return double(m_benchmarkReports.size());
    if (name == "benchmark_written") return double(m_benchmarksWritten);
    if (name == "log_errors") return double(m_log.errors());
    if (name == "log_warnings") return double(m_log.warnings());
    if (name == "job_test_ms") return m_jobTest && !m_jobTest->running ? m_jobTest->ms : -1.0;
    if (!currentReady()) return std::nullopt;
    return current().value(name);
}

// `menu:<section id>`: the center of that menu entry while the menu is shown; `ui:<name>`: a button of the windows
// (ui:sections, ui:controls, ui:prev, ui:next, ui:about, ui:settings, ui:reset_section, ...); other names: the
// section's.
std::optional<glm::vec2> DemoGame::scenarioPoint(std::string_view name) const {
    if (name.starts_with("menu:") || name.starts_with("ui:")) {
        for (const auto& [point, at] : m_points)
            if (point == name) return at;
        return std::nullopt;
    }
    if (!currentReady()) return std::nullopt;
    return current().point(*m_context, name);
}

app::ScenarioExec DemoGame::scenarioCommand(App& app, const app::ScenarioCommand& command) {
    const auto fail = [&command](std::string_view why) {
        log::error("demo", "line {}: {}", command.line, why);
        return app::ScenarioExec::Failed;
    };
    const auto done = app::ScenarioExec::Done;
    const std::vector<std::string>& args = command.args;
    if (command.op == "section") {
        if (args.size() != 1) return fail("section ID");
        const std::optional<std::size_t> found = find(args[0]);
        if (!found) return fail(std::format("no section '{}'", args[0]));
        select(*found);
        return done;
    }
    if (command.op == "camera") {
        // camera YAW PITCH DISTANCE (degrees, m)
        if (args.size() != 3) return fail("camera YAW PITCH DISTANCE");
        const std::optional<float> yaw = parseFloat(args[0]), pitch = parseFloat(args[1]),
                                   distance = parseFloat(args[2]);
        if (!yaw || !pitch || !distance) return fail("camera: numbers expected");
        OrbitCamera& camera = current().camera;
        camera.yaw = glm::radians(*yaw);
        camera.pitch = glm::radians(*pitch);
        camera.distance = *distance;
        return done;
    }
    if (command.op == "set") {
        if (args.size() != 2) return fail("set NAME VALUE");
        const std::optional<float> number = parseFloat(args[1]);
        if (args[0] == "ui_scale" && number) {
            m_settings.uiScale = std::clamp(*number, 0.5f, 3.0f);
            applyStyle();
            noteSettings();
            return done;
        }
        if (args[0] == "touch_controls" && (args[1] == "auto" || args[1] == "on" || args[1] == "off")) {
            m_settings.touch = args[1] == "on"    ? TouchControls::On
                               : args[1] == "off" ? TouchControls::Off
                                                  : TouchControls::Auto;
            applyStyle();
            noteSettings();
            return done;
        }
        if (setRenderSetting(args[0], args[1]) || (currentReady() && current().set(*m_context, args[0], args[1])))
            return done;
        return fail(std::format("set {} {}: unknown name or bad value", args[0], args[1]));
    }
    if (command.op == "reset_section") {
        resetSection();
        return done;
    }
    if (command.op == "windows") {
        // windows off|on: off leaves the scene and the section's overlay, for a capture without the demo's windows.
        if (args.size() != 1 || (args[0] != "off" && args[0] != "on")) return fail("windows off|on");
        m_windowsOff = args[0] == "off";
        return done;
    }
    if (command.op == "reset_camera") {
        resetCamera();
        return done;
    }
    if (command.op == "sheet") {
        // sheet none|sections|controls|about|settings|graphics: the compact layout's sheets; About, Settings and
        // Graphics also wide.
        if (args.size() != 1) return fail("sheet none|sections|controls|about|settings|graphics");
        m_aboutOpen = args[0] == "about";
        m_settingsOpen = args[0] == "settings";
        m_graphicsOpen = args[0] == "graphics";
        m_sheet = args[0] == "sections" ? Sheet::Sections : args[0] == "controls" ? Sheet::Controls : Sheet::None;
        if (compact() && m_aboutOpen) m_sheet = Sheet::About;
        if (compact() && m_settingsOpen) m_sheet = Sheet::Settings;
        if (compact() && m_graphicsOpen) m_sheet = Sheet::Graphics;
        return done;
    }
    if (command.op == "mode") {
        // mode off | tour [SECONDS] | checks | benchmark [SECONDS]
        if (args.empty()) return fail("mode off|tour|checks|benchmark [SECONDS]");
        const std::optional<float> seconds = args.size() > 1 ? parseFloat(args[1]) : std::nullopt;
        if (args[0] == "off")
            stopMode("stopped by the script");
        else if (args[0] == "tour")
            startTour(seconds.value_or(m_tourSeconds));
        else if (args[0] == "checks")
            startChecks();
        else if (args[0] == "benchmark")
            startBenchmark(seconds.value_or(5.0f), false);
        else
            return fail(std::format("mode {}: off, tour, checks or benchmark", args[0]));
        return done;
    }
    if (command.op == "check") {
        // check ok|problem|skip [NOTE...] | check next | check prev
        if (m_mode != Mode::Checks || args.empty()) return fail("check ok|problem|skip|next|prev in the check mode");
        if (args[0] == "next" || args[0] == "prev") {
            const std::size_t n = m_checks.size();
            showCheck(args[0] == "next" ? (m_checkIndex + 1) % n : (m_checkIndex + n - 1) % n);
            return done;
        }
        std::string note;
        for (std::size_t i = 1; i < args.size(); ++i) note += (i > 1 ? " " : "") + args[i];
        std::snprintf(m_checkNote.data(), m_checkNote.size(), "%s", note.c_str());
        if (args[0] == "ok")
            markCheck(CheckStatus::Ok);
        else if (args[0] == "problem")
            markCheck(CheckStatus::Problem);
        else if (args[0] == "skip")
            markCheck(CheckStatus::Skipped);
        else
            return fail("check ok|problem|skip|next|prev");
        return done;
    }
    if (command.op == "job_test") {
        startJobTest(app);
        return done;
    }
    if (command.op == "settings_save") {
        // Writes the settings now (they are written a moment after a change otherwise).
        m_settingsDirty = true;
        saveSettings(true);
        return done;
    }
    if (command.op == "settings_corrupt") {
        // A test of the fallback: overwrites the settings file with text that is not JSON.
        if (Result<void> r = fs::writeUserText(m_settingsFile, "{ not json"); !r) return fail(r.error());
        return done;
    }
    return app::ScenarioExec::Unknown;
}

std::string DemoGame::perfDetails(App& app) {
    (void)app;
    const render::RenderStats& s = m_renderer->stats();
    const render::ActiveEffects& e = m_renderer->effects();
    std::string details = std::format(
        "section={} render={}x{} draws={} instances={} shadow_draws={} lights={} aa={} cascades={} skinned={} "
        "sun_shadows={} ao={} upscaling={} ray_instances={} local_shadows={} reflections={} indirect={} denoiser={} "
        "path_tracer={} ray_mb={:.1f}",
        current().id(), s.renderWidth, s.renderHeight, s.draws, s.instances, s.shadowDraws, s.lights,
        antiAliasingName(e.antiAliasing), e.shadowCascades, s.skinnedInstances,
        e.sunShadows == render::SunShadows::RayTraced ? "rt" : "maps",
        e.ambientOcclusion == render::AmbientOcclusion::RayTraced ? "rt" : "none",
        e.upscaling == render::Upscaling::TemporalDenoised ? "denoised"
        : e.upscaling == render::Upscaling::Temporal       ? "temporal"
        : e.upscaling == render::Upscaling::Spatial        ? "spatial"
                                                           : "bilinear",
        s.rayInstances, e.localLightShadows, e.reflections == render::Reflections::RayTraced ? "rt" : "sky",
        e.indirectLight == render::IndirectLight::RayTraced ? "rt"
        : e.indirectLight == render::IndirectLight::Probes  ? "probes"
                                                            : "sky",
        e.rayDenoiser == render::RayDenoiser::Temporal ? "temporal" : "spatial", e.pathTracer ? 1 : 0,
        double(s.rayMemoryBytes) / (1024.0 * 1024.0));
    const std::string section = currentReady() ? current().perfDetails() : std::string();
    if (!section.empty()) details += ' ' + section;
    return details;
}

} // namespace demo

namespace {

constexpr const char* kHelp =
    "quench_demo options:\n"
    "  --section ID            start in a section: materials, shadows, sky, post, hdr, physics, input, assets,\n"
    "                          audio, animation, raytracing, tunnel (default: the last one, from the settings)\n"
    "  --tour [SECONDS]        show every section in turn, SECONDS each (default 8); any input stops it\n"
    "  --checks                the check mode: the checks waiting for a person, marked OK or problem with a note,\n"
    "                          saved to checks.json in the user directory\n"
    "  --benchmark-all [SECONDS]  measure every section for SECONDS (default 5) after a second of warm-up (SECONDS\n"
    "                          if shorter), write benchmark.json to the user directory and quit\n"
    "  --no-settings           neither read nor write settings.json (scripted runs never do, unless\n"
    "                          --settings-file names a file)\n"
    "  --settings-file NAME    the settings file in the user directory (default settings.json)\n"
    "  --checks-file NAME      the check results file (default checks.json)\n"
    "  --benchmark-file NAME   the benchmark report file (default benchmark.json)\n";

std::optional<float> optionalSeconds(std::span<const std::string_view> args, std::size_t& i) {
    if (i + 1 < args.size() && !args[i + 1].starts_with("--"))
        if (const std::optional<float> v = demo::parseFloat(args[i + 1]); v && *v > 0.0f) {
            ++i;
            return v;
        }
    return std::nullopt;
}

} // namespace

quench::Result<std::unique_ptr<quench::app::Game>> quench::app::createGame(AppDesc& desc,
                                                                           std::span<const std::string_view> args) {
    desc.name = QUENCH_GAME_NAME;
    desc.organization = "Quench";
    desc.version = QUENCH_GAME_VERSION;
    demo::DemoFlags flags;
    const auto name = [&](std::size_t& i, std::string& out) {
        if (i + 1 >= args.size() || args[i + 1].empty() || args[i + 1].starts_with("--")) return false;
        out = std::string(args[++i]);
        return true;
    };
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "--help") {
            std::fputs(kHelp, stdout);
        } else if (a == "--section" && name(i, flags.section)) {
        } else if (a == "--tour") {
            flags.tour = optionalSeconds(args, i).value_or(8.0f);
        } else if (a == "--checks") {
            flags.checks = true;
        } else if (a == "--benchmark-all") {
            flags.benchmark = optionalSeconds(args, i).value_or(5.0f);
        } else if (a == "--no-settings") {
            flags.noSettings = true;
        } else if (a == "--settings-file" && name(i, flags.settingsFile)) {
        } else if (a == "--checks-file" && name(i, flags.checksFile)) {
        } else if (a == "--benchmark-file" && name(i, flags.benchmarkFile)) {
        } else {
            return Error{std::format("demo: unknown or incomplete argument '{}' (--help lists the options)", a)};
        }
    }
    if (int(flags.checks) + int(flags.benchmark.has_value()) + int(flags.tour.has_value()) > 1)
        return Error{"demo: --tour, --checks and --benchmark-all exclude each other"};
    return std::unique_ptr<Game>(std::make_unique<demo::DemoGame>(std::move(flags)));
}
