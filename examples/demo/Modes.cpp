// The demo's modes. Tour: every section in turn for a few seconds, until any input. Checks: the checks waiting for a
// person (Checks.h), one at a time with its section and settings, marked OK, problem or skipped with a note, saved
// to checks.json in the user directory after every mark. Benchmark: every section measured as the benchmark
// scenario measures it (a second of warm-up, or the measured time if shorter, then a perf region in real time), one
// report in benchmark.json.

#include "DemoGame.h"

#include <demo_build_info.h>

#include <quench/core/FileSystem.h>
#include <quench/core/Json.h>
#include <quench/core/Log.h>

#include <imgui.h>

#include <SDL3/SDL_platform.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>

namespace demo {

using namespace quench;
using app::App;

namespace {

const char* modeName(int mode) {
    switch (mode) {
    case 1: return "Tour";
    case 2: return "Checks";
    case 3: return "Benchmark";
    default: return "";
    }
}

// Sections that load assets report when they are done; the benchmark waits for that before it measures.
bool settled(const Section& section) {
    for (const char* name : {"assets_idle", "sounds_ready", "anim_ready", "rt_ready"})
        if (const std::optional<double> v = section.value(name); v && *v != 1.0) return false;
    return true;
}

} // namespace

CheckRun DemoGame::runInfo() const {
    const gpu::Caps& caps = m_context->app.device().caps();
    return {.app = "Quench Demo",
            .version = QUENCH_GAME_VERSION,
            .build = std::format("{} {}", QUENCH_DEMO_CONFIG, QUENCH_DEMO_COMMIT),
            .backend = gpu::backendName(caps.backend),
            .driver = caps.driver,
            .device = caps.deviceName,
            .platform = SDL_GetPlatform()};
}

void DemoGame::stopMode(std::string_view why) {
    if (m_mode == Mode::None) return;
    if (m_mode == Mode::Benchmark && m_context->app.perfRunning()) (void)m_context->app.endPerf();
    if (m_mode == Mode::Checks) saveChecks();
    m_modeMessage = std::format("{} {}", modeName(int(m_mode)), why);
    log::info("demo", "{}", m_modeMessage);
    m_mode = Mode::None;
}

void DemoGame::startTour(float seconds) {
    stopMode("ended");
    m_mode = Mode::Tour;
    m_tourSeconds = std::clamp(seconds, 0.1f, 600.0f);
    m_modeTime = 0.0;
    m_tourStart = m_current;
    m_modeMessage.clear();
    log::info("demo", "tour: {:.1f} s per section, from '{}'", m_tourSeconds, current().id());
}

void DemoGame::updateMode(App& app, double frameDt) {
    m_modeTime += frameDt;
    if (m_mode == Mode::Tour && m_modeTime >= m_tourSeconds) {
        m_modeTime = 0.0;
        select((m_current + 1) % m_slots.size());
        ++m_toursDone;
    } else if (m_mode == Mode::Benchmark) {
        benchmarkStep(app);
    }
}

// ---- checks

void DemoGame::startChecks() {
    stopMode("ended");
    m_checks = emptyCheckResults();
    if (fs::userExists(m_flags.checksFile)) {
        const Result<std::string> text = fs::readUserText(m_flags.checksFile);
        Result<std::vector<CheckResult>> parsed =
            text ? parseCheckResults(*text) : Result<std::vector<CheckResult>>(Error{text.error()});
        if (parsed)
            m_checks = std::move(*parsed);
        else
            log::warning("demo", "checks: {} not used ({}); starting empty", m_flags.checksFile, parsed.error());
    }
    m_mode = Mode::Checks;
    m_modeMessage.clear();
    // The first item not marked yet.
    std::size_t first = 0;
    while (first < m_checks.size() && m_checks[first].status != CheckStatus::Pending) ++first;
    showCheck(first < m_checks.size() ? first : 0);
    log::info("demo", "checks: {} items, results in {}", m_checks.size(), m_flags.checksFile);
}

void DemoGame::showCheck(std::size_t index) {
    m_checkIndex = std::min(index, m_checks.size() - 1);
    const CheckItem& item = checkItems()[m_checkIndex];
    if (const std::optional<std::size_t> section = find(item.section)) select(*section);
    std::string_view setup = item.setup;
    while (!setup.empty()) {
        const std::size_t end = std::min(setup.find('\n'), setup.size());
        if (!runLine(setup.substr(0, end)))
            log::warning("demo", "check {}: setup '{}' failed", item.id, setup.substr(0, end));
        setup = end < setup.size() ? setup.substr(end + 1) : std::string_view();
    }
    std::snprintf(m_checkNote.data(), m_checkNote.size(), "%s", m_checks[m_checkIndex].note.c_str());
}

void DemoGame::markCheck(CheckStatus status) {
    CheckResult& r = m_checks[m_checkIndex];
    const gpu::Caps& caps = m_context->app.device().caps();
    r.status = status;
    r.note = m_checkNote.data();
    r.backend = std::format("{} ({})", gpu::backendName(caps.backend), caps.driver);
    r.time = utcNow();
    saveChecks();
    if (m_checkIndex + 1 < m_checks.size()) showCheck(m_checkIndex + 1);
}

bool DemoGame::saveChecks() {
    if (Result<void> r = fs::writeUserText(m_flags.checksFile, writeCheckResults(runInfo(), m_checks)); !r) {
        log::warning("demo", "checks: {} not written: {}", m_flags.checksFile, r.error());
        return false;
    }
    ++m_checkWrites;
    return true;
}

void DemoGame::checksWindow(App& app) {
    (void)app;
    const std::span<const CheckItem> items = checkItems();
    const CheckItem& item = items[m_checkIndex];
    CheckResult& r = m_checks[m_checkIndex];
    int counts[4] = {};
    for (const CheckResult& c : m_checks) ++counts[int(c.status)];
    const std::string header = std::format("Check {} of {}: {}", m_checkIndex + 1, items.size(), item.title);
    ImGui::SeparatorText(header.c_str());
    ImGui::TextWrapped("%.*s", int(item.what.size()), item.what.data());
    if (r.status != CheckStatus::Pending)
        ImGui::TextDisabled("Marked %s on %s, %s", checkStatusName(r.status), r.backend.c_str(), r.time.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##note", "note (what is wrong, the device)", m_checkNote.data(), m_checkNote.size());
    recordPoint("ui:check_note");
    if (ImGui::Button("OK")) markCheck(CheckStatus::Ok);
    recordPoint("ui:check_ok");
    ImGui::SameLine();
    if (ImGui::Button("Problem")) markCheck(CheckStatus::Problem);
    recordPoint("ui:check_problem");
    ImGui::SameLine();
    if (ImGui::Button("Skip")) markCheck(CheckStatus::Skipped);
    recordPoint("ui:check_skip");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##checkprev", ImGuiDir_Left)) showCheck((m_checkIndex + items.size() - 1) % items.size());
    recordPoint("ui:check_prev");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##checknext", ImGuiDir_Right)) showCheck((m_checkIndex + 1) % items.size());
    recordPoint("ui:check_next");
    ImGui::SameLine();
    if (ImGui::Button("End")) stopMode("ended");
    recordPoint("ui:check_end");
    ImGui::TextDisabled("ok %d, problem %d, skipped %d, pending %d; saved to %s", counts[int(CheckStatus::Ok)],
                        counts[int(CheckStatus::Problem)], counts[int(CheckStatus::Skipped)],
                        counts[int(CheckStatus::Pending)], m_flags.checksFile.c_str());
}

// ---- benchmark

void DemoGame::startBenchmark(float seconds, bool quitWhenDone) {
    stopMode("ended");
    m_mode = Mode::Benchmark;
    m_benchmarkSeconds = std::clamp(seconds, 0.1f, 120.0f);
    m_benchmarkQuit = quitWhenDone;
    m_benchmarkIndex = 0;
    m_benchmarkMeasuring = false;
    m_benchmarkReports.clear();
    m_modeTime = 0.0;
    m_modeMessage.clear();
    select(0);
    log::info("demo", "benchmark: {} sections, {:.1f} s each", m_slots.size(), m_benchmarkSeconds);
}

void DemoGame::benchmarkStep(App& app) {
    if (!m_benchmarkMeasuring) {
        // A second after the switch (or the measured time, if shorter), once the section's assets are in (at most
        // 6 s).
        const double settle = std::min(1.0, double(m_benchmarkSeconds));
        if (m_modeTime < settle || (!settled(current()) && m_modeTime < 6.0)) return;
        app.beginPerf(std::string(current().id()));
        m_benchmarkMeasuring = true;
        m_modeTime = 0.0;
        return;
    }
    if (m_modeTime < m_benchmarkSeconds) return;
    if (std::optional<app::PerfReport> report = app.endPerf())
        m_benchmarkReports.emplace_back(std::string(current().id()), std::move(*report));
    m_benchmarkMeasuring = false;
    m_modeTime = 0.0;
    if (++m_benchmarkIndex < m_slots.size()) {
        select(m_benchmarkIndex);
        return;
    }
    const bool written = writeBenchmark(app);
    m_mode = Mode::None;
    m_modeMessage = written
                        ? std::format("Benchmark: {} sections in {}", m_benchmarkReports.size(), m_flags.benchmarkFile)
                        : std::string("Benchmark: the report could not be written (see the log)");
    log::info("demo", "{}", m_modeMessage);
    if (m_benchmarkQuit) app.quit(written ? 0 : 1);
}

bool DemoGame::writeBenchmark(App& app) {
    const CheckRun run = runInfo();
    json::Value v = json::Value::makeObject();
    v.object["version"] = json::Value::makeNumber(1);
    v.object["app"] = json::Value::makeString(run.app);
    v.object["app_version"] = json::Value::makeString(run.version);
    v.object["build"] = json::Value::makeString(run.build);
    v.object["backend"] = json::Value::makeString(run.backend);
    v.object["driver"] = json::Value::makeString(run.driver);
    v.object["device"] = json::Value::makeString(run.device);
    v.object["platform"] = json::Value::makeString(run.platform);
    v.object["written"] = json::Value::makeString(utcNow());
    v.object["seconds"] = json::Value::makeNumber(m_benchmarkSeconds);
    v.object["window"] = json::Value::makeString(std::format("{}x{}", app.pixelWidth(), app.pixelHeight()));
    json::Value sections = json::Value::makeArray();
    for (const auto& [id, r] : m_benchmarkReports) {
        json::Value s = json::Value::makeObject();
        s.object["section"] = json::Value::makeString(id);
        s.object["frames"] = json::Value::makeNumber(double(r.frames));
        const auto ms = [](double value) { return json::Value::makeNumber(std::round(value * 1000.0) / 1000.0); };
        s.object["frame_avg_ms"] = ms(r.frameAvg);
        s.object["frame_p50_ms"] = ms(r.frameP50);
        s.object["frame_p99_ms"] = ms(r.frameP99);
        s.object["frame_max_ms"] = ms(r.frameMax);
        s.object["cpu_avg_ms"] = ms(r.cpuAvg);
        s.object["cpu_p50_ms"] = ms(r.cpuP50);
        s.object["cpu_p99_ms"] = ms(r.cpuP99);
        s.object["gpu_avg_ms"] = ms(r.gpuAvg);
        s.object["gpu_p50_ms"] = ms(r.gpuP50);
        s.object["gpu_samples"] = json::Value::makeNumber(double(r.gpuSamples));
        json::Value passes = json::Value::makeObject();
        for (const auto& [name, p50] : r.passP50) passes.object[name] = ms(p50);
        s.object["pass_p50_ms"] = std::move(passes);
        s.object["details"] = json::Value::makeString(r.details);
        s.object["thermal_start"] = json::Value::makeString(app::thermalStateName(r.thermalStart));
        s.object["thermal_end"] = json::Value::makeString(app::thermalStateName(r.thermalEnd));
        s.object["thermal_max"] = json::Value::makeString(app::thermalStateName(r.thermalMax));
        sections.array.push_back(std::move(s));
    }
    v.object["sections"] = std::move(sections);
    if (Result<void> r = fs::writeUserText(m_flags.benchmarkFile, json::write(v) + "\n"); !r) {
        log::error("demo", "benchmark: {} not written: {}", m_flags.benchmarkFile, r.error());
        return false;
    }
    ++m_benchmarksWritten;
    log::info("demo", "benchmark: {} sections written to {}", m_benchmarkReports.size(), m_flags.benchmarkFile);
    return true;
}

} // namespace demo
