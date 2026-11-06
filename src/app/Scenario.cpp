#include <quench/app/Scenario.h>

#include <quench/core/Log.h>
#include <quench/core/Parse.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <sstream>

namespace quench::app {
namespace {

// Commands whose argument is the rest of the line (key names have spaces: "Left Ctrl").
bool takesRestOfLine(std::string_view op) {
    return op == "down" || op == "up" || op == "tap" || op == "text" || op == "log";
}

std::string trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos) return {};
    const auto end = text.find_last_not_of(" \t\r");
    return std::string(text.substr(begin, end - begin + 1));
}

// Deep enough for a batch of batches; an include cycle stops here.
constexpr int kMaxIncludeDepth = 8;

} // namespace

Result<Scenario> Scenario::parse(std::string_view text, std::string name, const LoadFn& load) {
    Scenario scenario;
    scenario.m_name = std::move(name);
    if (Result<void> parsed = parseFile(scenario, text, scenario.m_name, load, 0); !parsed)
        return Error{parsed.error()};
    return scenario;
}

Result<void> Scenario::parseFile(Scenario& scenario, std::string_view text, const std::string& file, const LoadFn& load,
                                 int depth) {
    std::istringstream in{std::string(text)};
    std::string line;
    for (int number = 1; std::getline(in, line); ++number) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        std::istringstream words(line);
        ScenarioCommand command;
        command.line = number;
        command.file = file;
        if (!(words >> command.op)) continue;
        if (takesRestOfLine(command.op)) {
            std::string rest;
            std::getline(words, rest);
            command.args.push_back(trim(rest));
        } else {
            for (std::string arg; words >> arg;) command.args.push_back(arg);
        }
        if (command.op == "wait" && (command.args.size() != 1 || !parseDouble(command.args[0])))
            return Error{std::format("{}:{}: expected `wait SECONDS`", file, number)};
        if ((command.op == "expect" || command.op == "until") &&
            (command.args.size() < 3 || !parseDouble(command.args[2]) ||
             (command.op == "until" && command.args.size() > 3 && !parseDouble(command.args[3]))))
            return Error{std::format("{}:{}: expected `{} VAR OP VALUE{}`", file, number, command.op,
                                     command.op == "until" ? " [TIMEOUT]" : "")};
        if (command.op == "end_include") return Error{std::format("{}:{}: end_include is internal", file, number)};
        if (command.op != "include") {
            scenario.m_commands.push_back(std::move(command));
            continue;
        }
        if (command.args.size() != 1) return Error{std::format("{}:{}: expected `include FILE`", file, number)};
        if (!load) return Error{std::format("{}:{}: include: no files can be read here", file, number)};
        if (depth >= kMaxIncludeDepth)
            return Error{std::format("{}:{}: include deeper than {} files (a cycle?)", file, number, kMaxIncludeDepth)};
        const std::size_t slash = file.find_last_of('/');
        std::string path = slash == std::string::npos ? command.args[0] : file.substr(0, slash + 1) + command.args[0];
        Result<std::string> included = load(path);
        if (!included) return Error{std::format("{}:{}: include {}: {}", file, number, path, included.error())};
        const std::size_t begin = scenario.m_commands.size();
        command.args[0] = path;
        scenario.m_commands.push_back(std::move(command));
        if (Result<void> parsed = parseFile(scenario, *included, path, load, depth + 1); !parsed) return parsed;
        scenario.m_commands[begin].end = scenario.m_commands.size();
        scenario.m_commands.push_back(
            ScenarioCommand{.line = number, .op = "end_include", .args = {path}, .file = file});
    }
    return {};
}

void Scenario::fail(const ScenarioCommand& command, std::string_view what) {
    ++m_failed;
    log::error("scenario", "FAIL line {} of {}: {}", command.line, command.file, what);
}

bool Scenario::compare(const ScenarioCommand& command, const ValueFn& value, std::string& detail) const {
    const std::string& var = command.args[0];
    const std::string& op = command.args[1];
    std::optional<double> actual;
    if (var.starts_with("d_")) {
        const auto mark = m_marks.find(std::string_view(var).substr(2));
        const auto current = value(std::string_view(var).substr(2));
        if (mark != m_marks.end() && current) actual = *current - mark->second;
    } else {
        actual = value(var);
    }
    if (!actual) {
        detail = std::format("unknown variable '{}'", var);
        return false;
    }
    const double expected = *parseDouble(command.args[2]);
    const double tolerance = 1e-3 * std::max(1.0, std::abs(expected));
    bool ok = false;
    if (op == "<")
        ok = *actual < expected;
    else if (op == "<=")
        ok = *actual <= expected;
    else if (op == ">")
        ok = *actual > expected;
    else if (op == ">=")
        ok = *actual >= expected;
    else if (op == "==")
        ok = std::abs(*actual - expected) <= tolerance;
    else if (op == "!=")
        ok = std::abs(*actual - expected) > tolerance;
    else {
        detail = std::format("unknown operator '{}'", op);
        return false;
    }
    detail = std::format("{} {} {} (actual {:.6g})", var, op, command.args[2], *actual);
    return ok;
}

bool Scenario::update(double now, const std::function<bool()>& inputBusy, const ValueFn& gameValue,
                      const ExecFn& exec) {
    // Inside an included part, `time` counts from the part's start.
    const ValueFn value = [&](std::string_view name) -> std::optional<double> {
        if (name == "time" && !m_parts.empty()) return now - m_parts.back().origin;
        return gameValue(name);
    };
    while (m_next < m_commands.size()) {
        if (inputBusy()) return true;
        const ScenarioCommand& command = m_commands[m_next];
        if (command.op == "include") {
            m_parts.push_back({.origin = now, .end = command.end, .passed = m_passed, .failed = m_failed});
            log::info("scenario", "part {}", command.args[0]);
        } else if (command.op == "end_include") {
            const Part part = m_parts.back();
            m_parts.pop_back();
            log::info("scenario", "part {}: {} passed, {} failed", command.args[0], m_passed - part.passed,
                      m_failed - part.failed);
        } else if (command.op == "quit" && !m_parts.empty()) {
            m_next = m_parts.back().end;
            continue;
        } else if (command.op == "wait") {
            if (m_resumeAt < 0.0) m_resumeAt = now + *parseDouble(command.args[0]);
            if (now < m_resumeAt) return true;
            m_resumeAt = -1.0;
        } else if (command.op == "until") {
            if (m_deadline < 0.0) m_deadline = now + (command.args.size() > 3 ? *parseDouble(command.args[3]) : 10.0);
            std::string detail;
            const bool ok = compare(command, value, detail);
            if (!ok && now < m_deadline) return true;
            m_deadline = -1.0;
            if (ok) {
                ++m_passed;
                log::info("scenario", "ok   line {}: until {}", command.line, detail);
            } else {
                fail(command, "timeout: " + detail);
            }
        } else if (command.op == "expect") {
            std::string detail;
            if (compare(command, value, detail)) {
                ++m_passed;
                log::info("scenario", "ok   line {}: {}", command.line, detail);
            } else {
                fail(command, detail);
            }
        } else if (command.op == "mark") {
            const auto current = command.args.empty() ? std::nullopt : value(command.args[0]);
            if (current)
                m_marks[command.args[0]] = *current;
            else
                fail(command, "mark: unknown variable");
        } else if (command.op == "log") {
            log::info("scenario", "{}", command.args.empty() ? std::string() : command.args[0]);
        } else if (command.op == "quit") {
            m_next = m_commands.size();
            return false;
        } else {
            const Exec result = exec(command);
            if (result == Exec::Busy) return true;
            if (result == Exec::Failed) fail(command, std::format("command '{}' failed", command.op));
        }
        ++m_next;
    }
    return false;
}

} // namespace quench::app
