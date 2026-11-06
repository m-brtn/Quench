#include "Test.h"

#include <quench/app/Scenario.h>

#include <map>
#include <string>
#include <vector>

using namespace quench;
using app::Scenario;
using app::ScenarioCommand;

namespace {

struct Harness {
    std::map<std::string, double, std::less<>> values;
    std::vector<std::string> executed;
    bool inputBusy = false;
    int busyFrames = 0; // the next executed command reports Busy this many times

    bool run(Scenario& scenario, double now) {
        return scenario.update(
            now, [&] { return inputBusy; },
            [&](std::string_view name) -> std::optional<double> {
                const auto it = values.find(name);
                if (it == values.end()) return std::nullopt;
                return it->second;
            },
            [&](const ScenarioCommand& command) {
                if (busyFrames > 0) {
                    --busyFrames;
                    return Scenario::Exec::Busy;
                }
                executed.push_back(command.op + (command.args.empty() ? "" : " " + command.args[0]));
                return command.op == "fail_me" ? Scenario::Exec::Failed : Scenario::Exec::Done;
            });
    }
};

} // namespace

QUENCH_TEST(scenario_parse) {
    const auto scenario = Scenario::parse("# comment\n\nwait 0.5\ndown Left Ctrl  # key names have spaces\n"
                                          "expect x >= 2\nuntil y == 1 3\nclick 10 20 right\n",
                                          "test");
    REQUIRE(scenario.ok(), "%s", scenario.error().c_str());
    const auto& commands = scenario->commands();
    REQUIRE(commands.size() == 5, "%zu", commands.size());
    CHECK(commands[0].op == "wait" && commands[0].line == 3, "wait on line %d", commands[0].line);
    CHECK(commands[1].op == "down" && commands[1].args.size() == 1 && commands[1].args[0] == "Left Ctrl",
          "rest of line: '%s'", commands[1].args.empty() ? "" : commands[1].args[0].c_str());
    CHECK(commands[4].args.size() == 3 && commands[4].args[2] == "right", "click args");

    CHECK(!Scenario::parse("wait", "t").ok(), "wait without seconds");
    CHECK(!Scenario::parse("wait soon", "t").ok(), "wait with a word");
    CHECK(!Scenario::parse("expect x >", "t").ok(), "expect without value");
    CHECK(!Scenario::parse("until x == 1 later", "t").ok(), "until with a bad timeout");
}

QUENCH_TEST(scenario_wait_expect_mark) {
    auto scenario = Scenario::parse("wait 1\nexpect x == 1\nmark x\nexpect d_x == 0\nlog hello world\n"
                                    "expect x < 0\nexpect missing > 0\nquit\nexpect x == 1\n",
                                    "t");
    REQUIRE(scenario.ok(), "%s", scenario.error().c_str());
    Harness h;
    h.values["x"] = 1.0;
    CHECK(h.run(*scenario, 0.0), "waiting");
    CHECK(h.run(*scenario, 0.99), "still waiting");
    CHECK(!h.run(*scenario, 1.0), "ends at quit");
    CHECK(scenario->passed() == 2, "%d passed", scenario->passed());
    CHECK(scenario->failed() == 2, "%d failed (x < 0, unknown variable)", scenario->failed());
}

QUENCH_TEST(scenario_until_and_timeout) {
    auto scenario = Scenario::parse("until x >= 3 2\nuntil x > 100 0.5\n", "t");
    REQUIRE(scenario.ok(), "%s", scenario.error().c_str());
    Harness h;
    h.values["x"] = 0.0;
    CHECK(h.run(*scenario, 0.0), "condition not met");
    h.values["x"] = 3.0;
    CHECK(h.run(*scenario, 1.0), "first until passes, second waits");
    CHECK(scenario->passed() == 1, "%d", scenario->passed());
    CHECK(h.run(*scenario, 1.4), "second until still within its timeout");
    CHECK(!h.run(*scenario, 1.6), "second until times out and the scenario ends");
    CHECK(scenario->failed() == 1, "%d", scenario->failed());
}

QUENCH_TEST(scenario_commands_busy_and_input) {
    auto scenario = Scenario::parse("press A\nfail_me\nexpect x == 0\n", "t");
    REQUIRE(scenario.ok(), "%s", scenario.error().c_str());
    Harness h;
    h.values["x"] = 0.0;
    h.busyFrames = 2;
    CHECK(h.run(*scenario, 0.0), "busy 1");
    CHECK(h.run(*scenario, 0.0), "busy 2");
    h.inputBusy = true;
    CHECK(h.run(*scenario, 0.0), "pushed input not seen yet");
    CHECK(h.executed.empty(), "nothing ran while input was busy");
    h.inputBusy = false;
    CHECK(!h.run(*scenario, 0.0), "the rest runs");
    REQUIRE(h.executed.size() == 2, "%zu", h.executed.size());
    CHECK(h.executed[0] == "press A", "%s", h.executed[0].c_str());
    CHECK(scenario->failed() == 1 && scenario->passed() == 1, "%d failed, %d passed", scenario->failed(),
          scenario->passed());
}

QUENCH_TEST(scenario_include_parts) {
    const std::map<std::string, std::string> files = {
        {"dir/a.txt", "expect time == 0\nwait 1\nexpect time == 1\nquit\nexpect never == 1\n"},
        {"dir/sub/b.txt", "include c.txt\n"},
        {"dir/sub/c.txt", "wait 0.5\nexpect time == 0.5\nfail_me\n"},
        {"dir/loop.txt", "include loop.txt\n"},
    };
    const Scenario::LoadFn load = [&](const std::string& path) -> Result<std::string> {
        const auto it = files.find(path);
        if (it == files.end()) return Error{"not found"};
        return it->second;
    };
    auto scenario = Scenario::parse("include a.txt\ninclude sub/b.txt\nexpect time == 2.5\n", "dir/batch.txt", load);
    REQUIRE(scenario.ok(), "%s", scenario.error().c_str());
    const auto& commands = scenario->commands();
    REQUIRE(commands.size() == 15, "%zu commands", commands.size());
    CHECK(commands[0].op == "include" && commands[0].args[0] == "dir/a.txt" && commands[0].end == 6,
          "include a: %s, end %zu", commands[0].args[0].c_str(), commands[0].end);
    CHECK(commands[6].op == "end_include" && commands[1].file == "dir/a.txt", "a's commands: %s",
          commands[1].file.c_str());
    CHECK(commands[8].args[0] == "dir/sub/c.txt" && commands[9].file == "dir/sub/c.txt", "nested include: %s",
          commands[8].args[0].c_str());

    Harness h;
    const auto run = [&](double now) {
        h.values["time"] = now;
        return h.run(*scenario, now);
    };
    CHECK(run(1.0), "part a waits");
    CHECK(run(2.0), "quit in part a ends only the part; part c waits");
    CHECK(!run(2.5), "the batch ends after part c");
    CHECK(scenario->passed() == 4, "%d passed (time from the part's start, the batch's own)", scenario->passed());
    CHECK(scenario->failed() == 1, "%d failed (part c's command)", scenario->failed());
    CHECK(h.executed.size() == 1 && h.executed[0] == "fail_me", "%zu executed", h.executed.size());

    CHECK(!Scenario::parse("include a.txt\n", "dir/batch.txt").ok(), "include without a loader");
    CHECK(!Scenario::parse("include missing.txt\n", "dir/batch.txt", load).ok(), "missing file");
    CHECK(!Scenario::parse("include loop.txt\n", "dir/batch.txt", load).ok(), "include cycle");
    CHECK(!Scenario::parse("end_include a.txt\n", "dir/batch.txt", load).ok(), "end_include in a file");
}

QUENCH_TEST(scenario_tolerance) {
    auto scenario = Scenario::parse("expect x == 1000\nexpect x != 1000\nexpect y == 0\n", "t");
    REQUIRE(scenario.ok(), "%s", scenario.error().c_str());
    Harness h;
    h.values["x"] = 1000.5; // relative tolerance 1e-3 of 1000
    h.values["y"] = 0.0005; // absolute tolerance 1e-3 near zero
    CHECK(!h.run(*scenario, 0.0), "ends");
    CHECK(scenario->passed() == 2 && scenario->failed() == 1, "%d passed, %d failed", scenario->passed(),
          scenario->failed());
}
