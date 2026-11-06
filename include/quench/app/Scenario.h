#pragma once

// Verification scenarios (--script FILE): one command per line, '#' starts a comment. The runner handles timing and
// checks; the app executes input and capture commands by pushing real SDL events, and the game adds its own
// commands and values (Game::scenarioCommand, Game::scenarioValue).
//
//   wait S                      wait S seconds of app time
//   until VAR OP VALUE [TIMEOUT] wait for a condition (default timeout 10 s); a timeout is a failure
//   expect VAR OP VALUE         check now; OP is < <= > >= == != (== and != with a relative tolerance of 1e-3)
//   mark VAR                    remember a value; d_VAR is then the change since the mark
//   log TEXT                    print a line
//   quit                        end the scenario; the app exits with 1 if a check failed
//
// App commands (src/app/AppScenario.cpp): down/up/tap KEY, text STRING, mouse X Y, click X Y [left|right|middle],
// wheel DY, touch X Y [DX DY STEPS], gamepad attach|detach|button NAME down|up|tap|axis NAME VALUE, resize W H,
// capture PATH, perf_begin LABEL, perf_end, audio_measure SECONDS [MIN_HZ MAX_HZ]. Values: time, frame, ticks, fps,
// frame_ms, cpu_ms, gpu_ms, window_w, window_h, pixel_w, pixel_h, gamepad, fingers, action:NAME, perf_* after
// perf_end, audio_* (audio_rms, audio_freq, ... after audio_measure), and the game's (docs/app.md).

#include <quench/core/Result.h>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quench::app {

struct ScenarioCommand {
    int line = 0;
    std::string op;
    std::vector<std::string> args; // for down, up, tap, text and log: the rest of the line as one argument
};

class Scenario {
public:
    static Result<Scenario> parse(std::string_view text, std::string name);

    enum class Exec { Done, Busy, Failed }; // Busy: try again next frame
    using ValueFn = std::function<std::optional<double>(std::string_view name)>;
    using ExecFn = std::function<Exec(const ScenarioCommand& command)>;

    // Runs every command that is ready at app time `now`. inputBusy: pushed events have not reached the app yet (the
    // scenario waits a frame). Returns false when the scenario has ended.
    bool update(double now, const std::function<bool()>& inputBusy, const ValueFn& value, const ExecFn& exec);

    int passed() const { return m_passed; }
    int failed() const { return m_failed; }
    const std::string& name() const { return m_name; }
    const std::vector<ScenarioCommand>& commands() const { return m_commands; }
    void fail(const ScenarioCommand& command, std::string_view what);

private:
    bool compare(const ScenarioCommand& command, const ValueFn& value, std::string& detail) const;

    std::string m_name;
    std::vector<ScenarioCommand> m_commands;
    std::map<std::string, double, std::less<>> m_marks;
    std::size_t m_next = 0;
    double m_resumeAt = -1.0; // wait
    double m_deadline = -1.0; // until
    int m_passed = 0;
    int m_failed = 0;
};

} // namespace quench::app
