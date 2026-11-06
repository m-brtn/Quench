#pragma once

// The app around the sections (Demo.h): the menu and the section panel in a wide and a compact layout (Ui.cpp), the
// camera controls every 3D section shares (Camera.cpp), the tour, check and benchmark modes (Modes.cpp), the settings
// (Settings.h) and the scenario hooks (Demo.cpp).

#include "Checks.h"
#include "Demo.h"
#include "Settings.h"

#include <quench/app/App.h>
#include <quench/core/Jobs.h>

#include <imgui.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace demo {

// The orbit camera's input: mouse (left drag orbits, right or middle drag pans, the wheel zooms), touch (one finger
// orbits, two pan and pinch), gamepad (right stick orbits, left stick pans, the triggers zoom), keyboard (arrows
// orbit, + and - zoom). Sections without a free camera (Section::freeCamera) keep only the zoom.
class CameraControls {
public:
    void init(quench::app::Input& input);
    // `sceneFocus`: the keyboard and the gamepad drive the scene, not the menu. A press and release on the scene
    // without a drag goes to `click`.
    void update(quench::app::App& app, Section& section, float dt, bool sceneFocus,
                const std::function<void(glm::vec2)>& click);
    void reset();                              // a new section: a drag in progress ends
    bool touched() const { return m_touched; } // this frame's input pressed on the scene or moved the camera

private:
    quench::app::ActionId m_orbitDrag = 0, m_panDrag = 0;
    quench::app::ActionId m_orbitLeft = 0, m_orbitRight = 0, m_orbitUp = 0, m_orbitDown = 0;
    quench::app::ActionId m_panLeft = 0, m_panRight = 0, m_panForward = 0, m_panBack = 0;
    quench::app::ActionId m_zoomIn = 0, m_zoomOut = 0;
    bool m_pressed = false, m_dragged = false, m_panning = false, m_touched = false;
    glm::vec2 m_pressPoint{0.0f};
    std::uint64_t m_touchId = 0;
    glm::vec2 m_touchStart{0.0f};
    bool m_touchMoved = true;
};

// The newest log lines for the Log panel: a log sink, called from any thread under the log's lock.
class LogLines {
public:
    LogLines();
    ~LogLines();
    LogLines(const LogLines&) = delete;
    LogLines& operator=(const LogLines&) = delete;
    std::vector<std::string> lines() const;
    int warnings() const { return m_warnings.load(std::memory_order_relaxed); }
    int errors() const { return m_errors.load(std::memory_order_relaxed); }

private:
    static constexpr std::size_t kLines = 200;
    mutable std::mutex m_mutex;
    std::deque<std::string> m_lines;
    std::atomic<int> m_warnings{0}, m_errors{0};
    int m_sink = -1;
};

// The demo's own command line (--help lists it; docs/demo.md).
struct DemoFlags {
    std::string section;
    std::optional<float> tour;      // --tour [SECONDS]: seconds per section
    bool checks = false;            // --checks
    std::optional<float> benchmark; // --benchmark-all [SECONDS]: seconds measured per section
    bool noSettings = false;        // --no-settings
    std::string settingsFile;       // --settings-file NAME; empty: settings.json, not in scripted runs
    std::string checksFile = "checks.json";
    std::string benchmarkFile = "benchmark.json";
};

class DemoGame final : public quench::app::Game {
public:
    explicit DemoGame(DemoFlags flags);
    ~DemoGame() override;

    quench::Result<void> init(quench::app::App& app) override;
    void fixedStep(quench::app::App& app, double dt) override;
    void update(quench::app::App& app, double frameDt) override;
    void ui(quench::app::App& app) override;
    void render(quench::app::App& app, quench::app::Frame& frame) override;
    void shutdown(quench::app::App& app) override;
    std::optional<double> scenarioValue(std::string_view name) const override;
    std::optional<glm::vec2> scenarioPoint(std::string_view name) const override;
    quench::app::ScenarioExec scenarioCommand(quench::app::App& app,
                                              const quench::app::ScenarioCommand& command) override;
    std::string perfDetails(quench::app::App& app) override;

private:
    // A section and how it was made: constructed at the start, initialized when first shown.
    struct Slot {
        SectionFactory make = nullptr;
        std::unique_ptr<Section> section;
        OrbitCamera home; // the camera it was made with (Reset camera)
        bool ready = false;
        std::string error; // init failed: the message, shown in its panel
        double initMs = -1.0;
    };
    enum class Sheet { None, Sections, Controls, About, Settings };
    enum class Mode { None, Tour, Checks, Benchmark };

    // ---- sections and scenario hooks (Demo.cpp)
    Section& current() { return *m_slots[m_current].section; }
    const Section& current() const { return *m_slots[m_current].section; }
    bool currentReady() const { return m_slots[m_current].ready; }
    quench::render::Scene& currentScene();
    std::optional<std::size_t> find(std::string_view id) const;
    void select(std::size_t index);
    bool prepare(Slot& slot);
    void resetSection();
    void resetCamera();
    bool setRenderSetting(std::string_view name, std::string_view value);
    // One line of a check item's setup: `set NAME VALUE`, `camera YAW PITCH DISTANCE`, `hdr off|on|auto`.
    bool runLine(std::string_view line);
    void click(glm::vec2 point);
    void updateFocus(quench::app::App& app);
    void loadSettings(quench::app::App& app);
    void saveSettings(bool now);
    void noteSettings();
    void updateJobTest(quench::app::App& app);
    void startJobTest(quench::app::App& app);

    // ---- windows (Ui.cpp)
    void applyStyle();
    bool compact() const;
    void wideLayout(quench::app::App& app);
    void compactLayout(quench::app::App& app);
    void sectionList(bool compactList);
    void modeButtons(quench::app::App& app);
    void performancePanel(quench::app::App& app);
    void logPanel();
    void sectionPanel(bool withAbout);
    void aboutContents(quench::app::App& app);
    void settingsContents(quench::app::App& app);
    void bottomWindows(quench::app::App& app);
    void bottomWindowPlaced();
    void dragScroll();
    ImGuiWindowFlags navFlags() const;
    void recordPoint(std::string name);

    // ---- modes (Modes.cpp)
    void startTour(float seconds);
    void startChecks();
    void startBenchmark(float seconds, bool quitWhenDone);
    void stopMode(std::string_view why);
    void updateMode(quench::app::App& app, double frameDt);
    void showCheck(std::size_t index);
    void markCheck(CheckStatus status);
    bool saveChecks();
    void checksWindow(quench::app::App& app);
    void benchmarkStep(quench::app::App& app);
    bool writeBenchmark(quench::app::App& app);
    CheckRun runInfo() const;

    DemoFlags m_flags;
    std::optional<quench::render::Renderer> m_renderer;
    std::optional<Context> m_context;
    std::vector<Slot> m_slots;
    std::optional<quench::render::Scene> m_emptyScene; // drawn for a section whose init failed
    std::size_t m_current = 0;
    bool m_entered = false;
    bool m_cut = true;
    std::uint32_t m_targetWidth = 0;
    int m_clicks = 0;
    double m_switchMs = -1.0; // the last section switch, with the init on a first visit

    // Input
    CameraControls m_camera;
    quench::app::ActionId m_next = 0, m_prev = 0, m_toggleUi = 0;
    std::vector<quench::app::ActionId> m_direct;
    bool m_menuFocus = false; // the keyboard and the gamepad drive the windows (Tab, the D-pad; Escape, B leave)
    bool m_focusMenuNow = false;

    // Windows
    bool m_showUi = true;
    bool m_uiReappears = false;
    Sheet m_sheet = Sheet::None;
    bool m_aboutOpen = false, m_settingsOpen = false;
    int m_aboutTab = 0;
    std::string m_licence, m_thirdParty; // read when the About window first shows them
    ImGuiStyle m_baseStyle;              // the engine's style for the display, before the demo's scale and touch sizes
    float m_appliedScale = 0.0f;
    bool m_appliedTouch = false;
    bool m_touchScreen = false;                              // a direct touch device was seen
    std::vector<std::pair<std::string, glm::vec2>> m_points; // named UI points of the last frame (scenario)
    float m_compactWidthEm = 40.0f, m_compactHeightEm = 26.0f;
    // The caption or the mode window at the bottom, last frame.
    float m_bottomTop = 1.0e9f, m_bottomLeft = 0.0f, m_bottomRight = 0.0f;
    bool m_sectionOpen = true; // top of the mode window at the bottom in the last frame (points)
    bool m_dragScrolling = false;
    float m_editScale = 1.0f; // the UI scale slider while it is dragged
    bool m_scaleEditing = false;
    int m_screenshots = 0;
    std::array<float, 120> m_frameTimes{};
    std::size_t m_frameTimeNext = 0;
    LogLines m_log;

    // Settings
    Settings m_settings;
    bool m_settingsEnabled = false; // read and written (not in scripted runs, unless a file is named)
    std::string m_settingsFile;
    int m_settingsLoaded = 0; // 1 read, 0 defaults (no file, or ignored), -1 a bad file: defaults
    bool m_settingsDirty = false;
    double m_settingsSavedAt = -1.0;
    int m_settingsWrites = 0;
    quench::app::HdrMode m_lastHdr = quench::app::HdrMode::Off;

    // Modes
    Mode m_mode = Mode::None;
    double m_modeTime = 0.0;
    float m_tourSeconds = 8.0f;
    std::size_t m_tourStart = 0;
    int m_toursDone = 0;
    std::vector<CheckResult> m_checks;
    std::size_t m_checkIndex = 0;
    std::array<char, 512> m_checkNote{};
    int m_checkWrites = 0;
    float m_benchmarkSeconds = 5.0f;
    bool m_benchmarkQuit = false;
    std::size_t m_benchmarkIndex = 0;
    bool m_benchmarkMeasuring = false;
    std::vector<std::pair<std::string, quench::app::PerfReport>> m_benchmarkReports;
    std::string m_modeMessage; // the last mode's result, shown in the menu
    int m_benchmarksWritten = 0;

    // The job test of the Performance panel: ranges run per thread.
    struct JobTest {
        quench::Job job;
        std::array<std::atomic<int>, 17> ranges{};
        std::chrono::steady_clock::time_point start;
        std::atomic<std::int64_t> endNs{0}; // when the last range ended
        double ms = -1.0;
        bool running = false;
    };
    std::unique_ptr<JobTest> m_jobTest;
};

} // namespace demo
