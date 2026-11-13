// The assistant's manual check against a real provider (docs/assistant.md, "The manual check"), a command line
// program without a window: one typed turn with two invented tools of a test colony, optionally the answer as speech
// and that speech transcribed back, each stage printed with its milliseconds, the text as it streams, the tool calls
// and the answer. The key comes from the environment variable OPENROUTER_API_KEY only and is never printed. Never run
// by the automated checks: it sends requests to a provider and costs a little.
//
//   OPENROUTER_API_KEY=... build/release/examples/assistant_check/quench-assistant-check "Send walker one to E7"
//       [--speak] [--transcribe] [--wav FILE] [--chat-model M] [--no-stream]

#include <quench/assistant/Assistant.h>
#include <quench/audio/Wav.h>
#include <quench/core/Json.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace qa = quench::assistant;
namespace json = quench::json;

namespace {

double g_start = 0.0;

double seconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point origin = clock::now();
    return std::chrono::duration<double>(clock::now() - origin).count();
}

int msSinceStart() { return int((seconds() - g_start) * 1000.0 + 0.5); }

json::Value parseJson(const char* text) { return json::parse(text).valueOr({}); }

void addTools(qa::Assistant& a) {
    qa::Tool status;
    status.name = "get_status";
    status.description = "The test colony's power balance in kW, its stock of energy cells and its walkers.";
    status.annotations.readOnly = true;
    status.handler = [](const json::Value&) {
        return qa::ToolResult::ok(parseJson(R"({"power_kw":12,"cells_in_stock":3,"walkers":["Walker 1","Walker 2"],)"
                                            R"("nearest_cells":{"Walker 1":"E7","Walker 2":"E9"}})"));
    };
    (void)a.tools().add(std::move(status));
    qa::Tool collect;
    collect.name = "order_collect";
    collect.description = "Send a walker to collect an energy cell by its id (E1 .. E20).";
    collect.inputSchema =
        parseJson(R"({"type":"object","properties":{"unit":{"type":"string","enum":)"
                  R"(["Walker 1","Walker 2"]},"item":{"type":"string"}},"required":["unit","item"]})");
    collect.handler = [](const json::Value& args) {
        return qa::ToolResult::ok("ok: " + args["unit"].string + " is on its way to " + args["item"].string);
    };
    (void)a.tools().add(std::move(collect));
}

// Runs the assistant's operation to its end, printing what changes. Returns false on a failure or after 90 s.
bool run(qa::Assistant& a) {
    std::size_t shown = 0;
    int deltas = 0;
    const double limit = seconds() + 90.0;
    while (seconds() < limit) {
        a.update();
        if (a.partialText().size() < shown) shown = 0;
        if (a.partialText().size() > shown) {
            ++deltas;
            std::printf("%6d ms  text +%zu bytes: \"%s\"\n", msSinceStart(), a.partialText().size() - shown,
                        a.partialText().substr(shown).c_str());
            shown = a.partialText().size();
        }
        for (const qa::Event& e : a.takeEvents()) {
            switch (e.kind) {
            case qa::EventKind::StateChanged:
                std::printf("%6d ms  state: %s\n", msSinceStart(), qa::stateName(e.state));
                if (e.state == qa::State::Thinking) shown = 0;
                break;
            case qa::EventKind::Transcribed:
                std::printf("%6d ms  transcript: \"%s\"\n", msSinceStart(), e.text.c_str());
                break;
            case qa::EventKind::ToolStarted: {
                const qa::ToolCallRecord& c = a.toolCalls()[e.tool];
                std::printf("%6d ms  tool call %s %s (round %d)\n", msSinceStart(), c.name.c_str(), c.arguments.c_str(),
                            c.round);
                break;
            }
            case qa::EventKind::ToolFinished: {
                const qa::ToolCallRecord& c = a.toolCalls()[e.tool];
                std::printf("%6d ms  tool result%s: %s\n", msSinceStart(), c.result.isError ? " (error)" : "",
                            c.result.contentText().c_str());
                break;
            }
            case qa::EventKind::Answered:
                std::printf("%6d ms  answer: \"%s\" (%d text deltas seen)\n", msSinceStart(), e.text.c_str(), deltas);
                break;
            case qa::EventKind::SpeechReady: std::printf("%6d ms  speech ready\n", msSinceStart()); break;
            case qa::EventKind::Failed: std::printf("%6d ms  FAILED: %s\n", msSinceStart(), e.text.c_str()); break;
            case qa::EventKind::Cancelled: std::printf("%6d ms  cancelled\n", msSinceStart()); break;
            }
        }
        if (!a.busy()) return a.state() == qa::State::Done;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::printf("timed out\n");
    a.cancel();
    return false;
}

void printStats(const char* what, const qa::Stats& s) {
    std::printf("%s: %d requests, %d rounds, %d tool calls, %lld+%lld tokens, cost %s%.6f, stages: transcribe %.0f "
                "ms, think %.0f ms, tools %.2f ms, speak %.0f ms, first text %.0f ms, total %.0f ms\n",
                what, s.requests, s.rounds, s.toolCalls, (long long)s.usage.promptTokens,
                (long long)s.usage.completionTokens, s.usage.costKnown ? "$" : "(unknown) $", s.usage.cost,
                s.transcribeTime * 1000, s.thinkTime * 1000, s.toolTime * 1000, s.speakTime * 1000,
                s.firstTextTime * 1000, s.totalTime * 1000);
}

// Mono samples at `rate` resampled linearly to 16 kHz, as a 16-bit WAV (what a recording uploads).
std::vector<std::uint8_t> toWav16k(const qa::SpeechAudio& audio) {
    std::vector<float> out;
    const double step = double(audio.sampleRate) / 16000.0;
    for (double t = 0.0; t + 1.0 < double(audio.samples.size()); t += step) {
        const std::size_t i = std::size_t(t);
        const float f = float(t - double(i));
        out.push_back(audio.samples[i] * (1.0f - f) + audio.samples[i + 1] * f);
    }
    return quench::audio::encodeWav(out, 1, 16000);
}

} // namespace

int main(int argc, char** argv) {
    std::string text;
    std::string wavPath;
    std::string chatModel;
    bool speak = false;
    bool transcribe = false;
    bool stream = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--speak") {
            speak = true;
        } else if (arg == "--transcribe") {
            speak = transcribe = true;
        } else if (arg == "--no-stream") {
            stream = false;
        } else if (arg == "--wav" && i + 1 < argc) {
            wavPath = argv[++i];
        } else if (arg == "--chat-model" && i + 1 < argc) {
            chatModel = argv[++i];
        } else if (arg == "--help" || arg == "-h" || arg.starts_with("--")) {
            std::printf("usage: %s \"TEXT\" [--speak] [--transcribe] [--wav FILE] [--chat-model M] [--no-stream]\n"
                        "The key comes from OPENROUTER_API_KEY. Sends requests to OpenRouter (costs a little).\n",
                        argv[0]);
            return arg == "--help" || arg == "-h" ? 0 : 2;
        } else {
            text = arg;
        }
    }
    const char* key = std::getenv("OPENROUTER_API_KEY");
    if (!key || !*key) {
        std::printf("assistant-check: OPENROUTER_API_KEY is not set\n");
        return 2;
    }
    if (text.empty() && wavPath.empty()) {
        std::printf("assistant-check: no text (and no --wav)\n");
        return 2;
    }

    qa::Assistant a;
    a.settings() = qa::openRouterPreset();
    a.settings().apiKey = key;
    a.settings().streamChat = stream;
    a.settings().extraHeaders = {{"X-Title", "Quench assistant check"}};
    if (!chatModel.empty()) a.settings().chatModel = chatModel;
    a.setSystemPrompt("You are the assistant of a small test colony game. Use the tools to look things up and to give "
                      "orders. Answer in one short sentence.");
    a.setStateSummary([] { return std::string("Day 3, 14:20. Two walkers idle near the base."); });
    addTools(a);
    std::printf("assistant-check: %s backend, chat %s, transcription %s, speech %s (%s)\n", a.client().backendName(),
                a.settings().chatModel.c_str(), a.settings().transcriptionModel.c_str(),
                a.settings().speechModel.c_str(), a.settings().voice.c_str());
    qa::Stats total;
    bool ok = true;

    if (!wavPath.empty()) {
        std::ifstream file(wavPath, std::ios::binary);
        const std::vector<std::uint8_t> wav((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (wav.empty()) {
            std::printf("assistant-check: cannot read %s\n", wavPath.c_str());
            return 2;
        }
        std::printf("\n== voice turn from %s (%zu bytes)\n", wavPath.c_str(), wav.size());
        g_start = seconds();
        if (quench::Result<void> r = a.askByVoice(wav, {.speak = speak}); !r) {
            std::printf("refused: %s\n", r.error().c_str());
            return 1;
        }
        ok = run(a);
        printStats("voice turn", a.stats());
    } else {
        std::printf("\n== typed turn: \"%s\"\n", text.c_str());
        g_start = seconds();
        if (quench::Result<void> r = a.ask(text, {.speak = speak}); !r) {
            std::printf("refused: %s\n", r.error().c_str());
            return 1;
        }
        ok = run(a);
        printStats("turn", a.stats());
    }
    std::optional<qa::SpeechAudio> speech = a.takeSpeech();
    if (speech) {
        std::printf("speech: %zu samples at %u Hz, %.2f s\n", speech->samples.size(), speech->sampleRate,
                    speech->seconds());
    }
    if (ok && transcribe && speech) {
        const std::vector<std::uint8_t> wav = toWav16k(*speech);
        std::printf("\n== the speech transcribed back (%zu bytes of 16 kHz WAV)\n", wav.size());
        g_start = seconds();
        if (quench::Result<void> r = a.transcribe(wav); !r) {
            std::printf("refused: %s\n", r.error().c_str());
            return 1;
        }
        ok = run(a);
        printStats("transcription", a.stats());
    }
    total = a.totals();
    printStats("\nall", total);
    return ok ? 0 : 1;
}
