// The live session's manual check against OpenAI's GPT-Live (docs/live.md, "The manual check"), a command line
// program without a window: it opens one session with two invented tools of a test station, feeds a spoken question
// from a WAV file through the microphone's fake source in its streaming mode (at real-time pace, as a microphone
// would), optionally speaks over the answer with a second file, and prints the handshake's time, the time from the
// end of the question to the answer's first audio, how far ahead of real time the audio arrives, both transcripts,
// the tool calls and the delegation's time, every server event type, the seconds billed and the session's expiry; the
// answer's audio goes to a WAV file. The key comes from the environment variable OPENAI_API_KEY only and is never
// printed. Never run by the automated checks: a session costs money for every second it is open.
//
//   OPENAI_API_KEY=... build/release/examples/live_check/quench-live-check --wav question.wav
//       [--barge-in FILE] [--barge-after SECONDS | --barge-after-delegation SECONDS] [--russian] [--out DIR]
//       [--max-seconds 30] [--voice V]
//       [--backend-model M]
//   OPENAI_API_KEY=... build/release/examples/live_check/quench-live-check --talk [--gate off|on] [--seconds 60]
//       a person talks through the Mac's default microphone and output (Talk.cpp; opens the real microphone)
//
// A question can be made with macOS's speech synthesis, for example:
//   say -v Samantha -o question.wav --file-format=WAVE --data-format=LEI16@24000 "What is our power level?"

#include "Talk.h"

#include <quench/assistant/Chat.h>
#include <quench/assistant/Live.h>
#include <quench/audio/Microphone.h>
#include <quench/audio/Sound.h>
#include <quench/audio/Wav.h>
#include <quench/core/Json.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace qa = quench::assistant;
namespace audio = quench::audio;
namespace json = quench::json;

namespace {

double seconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point origin = clock::now();
    return std::chrono::duration<double>(clock::now() - origin).count();
}

json::Value parseJson(const char* text) { return json::parse(text).valueOr({}); }

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void addTools(qa::LiveSession& s) {
    qa::Tool status;
    status.name = "get_status";
    status.description = "The test station's power balance in kW, its stock of energy cells and its rovers.";
    status.annotations.readOnly = true;
    status.handler = [](const json::Value&) {
        return qa::ToolResult::ok(parseJson(R"({"power_kw":12,"cells_in_stock":3,"rovers":["Rover 1","Rover 2"],)"
                                            R"("rover_positions":{"Rover 1":"B2","Rover 2":"base"}})"));
    };
    (void)s.tools().add(std::move(status));
    qa::Tool send;
    send.name = "send_rover";
    send.description = "Send a rover to a cell of the map (A1 .. H8) to collect what is there.";
    send.inputSchema = parseJson(R"({"type":"object","properties":{"rover":{"type":"string","enum":)"
                                 R"(["Rover 1","Rover 2"]},"cell":{"type":"string"}},"required":["rover","cell"]})");
    send.handler = [](const json::Value& args) {
        return qa::ToolResult::ok(args["rover"].string + " is on its way to " + args["cell"].string);
    };
    (void)s.tools().add(std::move(send));
}

} // namespace

int main(int argc, char** argv) {
    std::string wavPath, bargePath, outDir = "/tmp", voice, backendModel;
    double bargeAfter = 1.5, maxSeconds = 30.0;
    double bargeAfterDelegation = -1.0; // >= 0: the barge-in that long after the delegation ended instead
    bool russian = false;
    bool talk = false, gate = false;
    double talkSeconds = 60.0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (arg == "--wav")
            wavPath = value();
        else if (arg == "--barge-in")
            bargePath = value();
        else if (arg == "--barge-after")
            bargeAfter = std::atof(value().c_str());
        else if (arg == "--barge-after-delegation")
            bargeAfterDelegation = std::atof(value().c_str());
        else if (arg == "--out")
            outDir = value();
        else if (arg == "--max-seconds")
            maxSeconds = std::atof(value().c_str());
        else if (arg == "--voice")
            voice = value();
        else if (arg == "--backend-model")
            backendModel = value();
        else if (arg == "--russian")
            russian = true;
        else {
            std::printf("usage: %s --wav FILE [--barge-in FILE] [--barge-after S] [--russian] [--out DIR] "
                        "[--max-seconds S] [--voice V] [--backend-model M]\n       %s --talk [--gate off|on] "
                        "[--seconds N] [--russian]\nThe key comes from OPENAI_API_KEY. "
                        "Opens a GPT-Live session (billed per second).\n",
                        argv[0], argv[0]);
            return arg == "--help" || arg == "-h" ? 0 : 2;
        }
    }
    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key || !*key) {
        std::printf("live-check: OPENAI_API_KEY is not set\n");
        return 2;
    }
    if (talk) {
        qa::LiveSession s;
        s.settings().apiKey = key;
        s.settings().instructions = russian ? "Ты бортовой компьютер небольшой исследовательской станции. Говори "
                                              "спокойно и коротко. Говори по-русски, если игрок не попросит другой "
                                              "язык."
                                            : "You are the onboard computer of a small research station. Speak calmly "
                                              "and briefly. Speak English unless the player asks to switch.";
        s.settings().backendInstructions = "You operate a test station through its tools. Look up the state with "
                                           "get_status before answering questions about it.";
        addTools(s);
        s.setStateSummary([] { return std::string("Day 3, 14:20. Both rovers are idle."); });
        return runTalk(s, gate, talkSeconds);
    }
    quench::Result<audio::SoundData> question = audio::decodeSound(readFile(wavPath));
    if (!question) {
        std::printf("live-check: cannot read %s: %s\n", wavPath.c_str(), question.error().c_str());
        return 2;
    }
    std::optional<audio::SoundData> barge;
    if (!bargePath.empty()) {
        quench::Result<audio::SoundData> b = audio::decodeSound(readFile(bargePath));
        if (!b) {
            std::printf("live-check: cannot read %s\n", bargePath.c_str());
            return 2;
        }
        barge = std::move(*b);
    }
    const double questionSeconds =
        double(question->samples.size()) / question->format.channels / question->format.sampleRate;

    qa::LiveSession s;
    qa::LiveSettings& st = s.settings();
    st.apiKey = key;
    st.idleSeconds = 0.0;  // this check closes the session itself
    st.uplinkGate = false; // nothing is played here: no echo to keep out
    if (!voice.empty()) st.voice = voice;
    if (!backendModel.empty()) st.backendModel = backendModel;
    st.instructions = russian ? "Ты бортовой компьютер небольшой исследовательской станции. Говори спокойно и "
                                "коротко, одним-двумя предложениями. Говори по-русски, если игрок не попросит "
                                "другой язык."
                              : "You are the onboard computer of a small research station. Speak calmly and briefly, "
                                "in one or two sentences. Speak English unless the player asks to switch.";
    st.backendInstructions = "You operate a test station through its tools. Look up the state with get_status before "
                             "answering questions about it. Report an action as done only after its tool confirms "
                             "it. Answer in one short sentence.";
    addTools(s);
    s.setStateSummary([] { return std::string("Day 3, 14:20. Both rovers are idle."); });

    // The microphone's fake source in the streaming mode: the question at real-time pace, then silence.
    audio::declareMicrophoneUsage("the live check");
    audio::Microphone mic;
    mic.setFakePermission(audio::MicPermission::Granted);
    if (quench::Result<void> r = mic.start({.sampleRate = st.sampleRate, .stream = true}); !r) {
        std::printf("live-check: microphone: %s\n", r.error().c_str());
        return 1;
    }

    // What arrives, for the report.
    std::map<std::string, int> types;
    std::vector<std::pair<double, double>> audioArrivals; // (time, seconds of audio)
    double firstAudio = -1.0, lastAudio = -1.0;
    double firstAudible = -1.0, lastAudible = -1.0; // pieces that are not silence
    double questionStart = -1.0, questionEnd = -1.0, bargeStart = -1.0;
    double aheadMax = 0.0, audioTotal = 0.0;
    double bargeLastAudible = -1.0; // the last audible piece after the barge-in started
    double lastToolTime = -1.0;
    double delegationDone = -1.0;
    std::vector<float> answer;
    const double t0 = seconds();
    auto ms = [&](double t) { return int((t - t0) * 1000.0 + 0.5); };
    s.setEventTap([&](const json::Value& e) {
        const std::string type = e["type"].stringOr("");
        const double t = seconds();
        if (type == "response.event") {
            types["response.event/" + e["event"]["type"].stringOr("")] += 1;
            const std::string inner = e["event"]["type"].stringOr("");
            if (inner == "response.created" || inner == "response.completed" || inner == "response.output_item.done")
                std::printf("%6d ms  backend %s%s\n", ms(t), inner.c_str(),
                            inner == "response.output_item.done"
                                ? (" " + e["event"]["item"]["type"].stringOr("")).c_str()
                                : "");
            return;
        }
        types[type] += 1;
        if (type == "session.output_audio.delta") {
            const double sec = double(e["delta"].stringOr("").size()) * 3.0 / 4.0 / 2.0 / st.sampleRate;
            audioArrivals.emplace_back(t, sec);
            if (firstAudio < 0.0) {
                firstAudio = t;
                std::printf("%6d ms  first audio piece (silence or not)\n", ms(t));
            }
            audioTotal += sec;
            // As if played from the first piece on at real time: how much was there ahead of the playhead.
            aheadMax = std::max(aheadMax, audioTotal - (t - firstAudio));
            lastAudio = t;
        } else if (type != "session.input_transcript.delta" && type != "session.output_transcript.delta") {
            std::printf("%6d ms  %s %s\n", ms(t), type.c_str(),
                        type == "error" || type == "info" || type == "session.closed" || type == "session.usage.updated"
                            ? quench::assistant::writeCompactJson(type == "session.closed" ? e["usage"] : e).c_str()
                            : "");
        }
    });

    std::printf("live-check: %s, voice %s, backend %s, question %.2f s (%s)%s\n", st.model.c_str(), st.voice.c_str(),
                st.backendModel.c_str(), questionSeconds, wavPath.c_str(), barge ? ", with a barge-in" : "");
    if (quench::Result<void> r = s.start(); !r) {
        std::printf("live-check: start refused: %s\n", r.error().c_str());
        return 1;
    }
    std::vector<float> chunk;
    double last = seconds();
    bool closing = false;
    double closeAt = 0.0;
    while (true) {
        const double t = seconds();
        const double dt = t - last;
        last = t;
        mic.update(dt);
        chunk.clear();
        mic.takeStream(chunk);
        s.sendAudio(chunk);
        s.update();
        const std::size_t before = answer.size();
        s.takeAudio(answer);
        for (std::size_t i = before; i < answer.size() && firstAudible < 0.0 && questionEnd > 0.0; ++i)
            if (std::abs(answer[i]) > 0.002f && t > questionStart) {
                firstAudible = t;
                std::printf("%6d ms  first audible audio after the question started (%.0f ms after it ended)\n", ms(t),
                            (t - questionEnd) * 1000.0);
            }
        for (std::size_t i = before; i < answer.size(); ++i)
            if (std::abs(answer[i]) > 0.002f) {
                lastAudible = t;
                if (bargeStart > 0.0) bargeLastAudible = t;
            }
        for (const qa::LiveEvent& e : s.takeEvents()) {
            switch (e.kind) {
            case qa::LiveEventKind::StateChanged:
                std::printf("%6d ms  state: %s%s\n", ms(t), qa::liveStateName(e.state),
                            e.state == qa::LiveState::CallingTool ? (" " + s.toolName()).c_str() : "");
                break;
            case qa::LiveEventKind::Started:
                std::printf("%6d ms  started %s (connect %.0f ms, handshake %.0f ms), expires in %lld s\n", ms(t),
                            e.text.c_str(), s.stats().connectSeconds * 1000, s.stats().startSeconds * 1000,
                            (long long)(s.stats().expiresAt - std::chrono::duration_cast<std::chrono::seconds>(
                                                                  std::chrono::system_clock::now().time_since_epoch())
                                                                  .count()));
                questionStart = t + 1.0;
                break;
            case qa::LiveEventKind::InputTranscript:
                std::printf("%6d ms  player: \"%s\"\n", ms(t), e.text.c_str());
                break;
            case qa::LiveEventKind::OutputTranscript:
                std::printf("%6d ms  station: \"%s\"\n", ms(t), e.text.c_str());
                break;
            case qa::LiveEventKind::ToolStarted: {
                const qa::LiveToolCall& c = s.toolCalls()[e.tool];
                std::printf("%6d ms  tool call %s %s\n", ms(t), c.name.c_str(), c.arguments.c_str());
                break;
            }
            case qa::LiveEventKind::ToolFinished: {
                const qa::LiveToolCall& c = s.toolCalls()[e.tool];
                lastToolTime = t;
                std::printf("%6d ms  tool result%s: %s\n", ms(t), c.result.isError ? " (error)" : "",
                            c.result.contentText().c_str());
                break;
            }
            case qa::LiveEventKind::DelegationDone:
                delegationDone = t;
                std::printf("%6d ms  delegation done in %.0f ms\n", ms(t), e.seconds * 1000);
                break;
            case qa::LiveEventKind::ServerError:
                std::printf("%6d ms  server error: %s\n", ms(t), e.text.c_str());
                break;
            case qa::LiveEventKind::Closed:
                std::printf("%6d ms  closed: %s\n", ms(t), qa::liveCloseReasonName(e.reason));
                break;
            case qa::LiveEventKind::Failed:
                std::printf("%6d ms  FAILED: %s (%s, status %d)\n", ms(t), e.text.c_str(),
                            qa::errorKindName(s.failure().kind), s.failure().status);
                break;
            default: break;
            }
        }
        if (questionStart > 0.0 && t >= questionStart && questionEnd < 0.0) {
            mic.feed(*question);
            questionEnd = t + questionSeconds;
            std::printf("%6d ms  the question starts (%.2f s)\n", ms(t), questionSeconds);
        }
        const bool bargeNow = bargeAfterDelegation >= 0.0
                                  ? delegationDone > 0.0 && t >= delegationDone + bargeAfterDelegation
                                  : firstAudible > 0.0 && t >= firstAudible + bargeAfter;
        if (barge && bargeStart < 0.0 && bargeNow) {
            mic.feed(*barge);
            bargeStart = t;
            std::printf("%6d ms  the barge-in starts, over the answer\n", ms(t));
        }
        if (s.state() == qa::LiveState::Closed || s.state() == qa::LiveState::Failed) break;
        // Ended: 3 s of silence after speech that came after the last tool call or delegation.
        const bool answered = firstAudible > 0.0 && t - lastAudible > 3.0 && (!barge || bargeStart > 0.0) &&
                              s.state() != qa::LiveState::CallingTool && s.stats().delegations > 0 &&
                              s.toolCalls().size() >= 1 && lastAudible > lastToolTime;
        if (!closing && (answered || t - t0 > maxSeconds)) {
            std::printf("%6d ms  closing (%s)\n", ms(t), answered ? "the answer ended" : "the time limit");
            s.close();
            closing = true;
            closeAt = t;
        }
        if (closing && t - closeAt > 20.0) {
            s.cancel();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::printf("\n== report\n");
    std::printf("handshake: connect %.0f ms, session.started %.0f ms after start()\n", s.stats().connectSeconds * 1000,
                s.stats().startSeconds * 1000);
    if (firstAudible > 0.0 && questionEnd > 0.0)
        std::printf("first audible audio of the answer: %.0f ms after the end of the question\n",
                    (firstAudible - questionEnd) * 1000);
    std::printf("audible audio received: %.2f s\n", s.stats().audibleReceivedSeconds);
    if (!audioArrivals.empty()) {
        std::printf("audio: %.2f s in %zu pieces over %.2f s of arrival; at most %.2f s ahead of real-time playback\n",
                    audioTotal, audioArrivals.size(), lastAudio - firstAudio, aheadMax);
        double maxPiece = 0.0;
        for (const auto& a : audioArrivals) maxPiece = std::max(maxPiece, a.second);
        std::printf("audio pieces: the largest %.0f ms, the average %.0f ms\n", maxPiece * 1000,
                    audioTotal / double(audioArrivals.size()) * 1000);
    }
    if (bargeStart > 0.0)
        std::printf("barge-in: started %.0f ms after the first audible audio; audible audio went on until %.0f ms "
                    "after it started\n",
                    (bargeStart - firstAudible) * 1000,
                    bargeLastAudible > 0 ? (bargeLastAudible - bargeStart) * 1000 : -1.0);
    std::printf("transcripts:\n");
    for (const qa::LiveTurn& turn : s.history())
        std::printf("  %s [%d .. %d ms]: %s\n", turn.speaker == qa::LiveSpeaker::Player ? "player " : "station",
                    turn.startMs, turn.endMs, turn.text.c_str());
    for (const qa::LiveToolCall& c : s.toolCalls())
        std::printf("tool call: %s %s -> %s%s\n", c.name.c_str(), c.arguments.c_str(),
                    c.result.isError ? "error: " : "", c.result.contentText().c_str());
    std::printf("delegations: %d, the last took %.0f ms\n", s.stats().delegations,
                s.stats().lastDelegationSeconds * 1000);
    const qa::LiveUsage u = s.usage();
    std::printf("billed: %.2f s (%s), backend %lld in + %lld cached + %lld out tokens; cost $%.4f session + $%.6f "
                "backend\n",
                u.totalSeconds, u.confirmed ? "confirmed by session.closed" : "unconfirmed",
                (long long)u.backendInputTokens, (long long)u.backendCachedTokens, (long long)u.backendOutputTokens,
                u.sessionCost, u.backendCost);
    std::printf("server events:\n");
    for (const auto& [type, count] : types) std::printf("  %-50s %d\n", type.c_str(), count);
    if (!answer.empty()) {
        const std::string path = outDir + "/live_answer" + (russian ? "_ru" : "") + ".wav";
        const auto wav = audio::encodeWav(answer, 1, st.sampleRate, audio::WavSampleFormat::Pcm16);
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(wav.data()), std::streamsize(wav.size()));
        std::printf("answer audio: %s (%.2f s)\n", path.c_str(), double(answer.size()) / st.sampleRate);
    }
    return s.state() == qa::LiveState::Failed ? 1 : 0;
}
