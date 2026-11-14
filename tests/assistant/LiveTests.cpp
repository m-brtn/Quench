// assistant::LiveSession with the fake WebSocket server playing GPT-Live (every build, the Node.js one included): the
// start handshake, audio both ways, transcripts in pieces, a tool call and its result, parallel calls, a call the
// schema refuses, injected context, the uplink gate, the idle close, the server's close reasons, error events, a
// reconnect with the history, cancel, and the key in no log line. The server's events are written as the reference
// (api/reference/resources/live/primary-websocket, 2026-11-14) shows them.

#include "Test.h"

#include <quench/assistant/Chat.h>
#include <quench/assistant/Live.h>
#include <quench/assistant/Speech.h>
#include <quench/core/Json.h>
#include <quench/core/Log.h>
#include <quench/net/FakeWebSocket.h>

#include <cmath>
#include <format>
#include <string>
#include <vector>

using namespace quench;
using namespace quench::assistant;

namespace {

constexpr const char* kLiveKey = "sk-proj-live0000test0000key0000not0000real";
const std::string kLiveUrl = "wss://api.openai.com/v1/live/sessions";

json::Value parse(const std::string& text) { return json::parse(text).valueOr({}); }
std::string compact(const json::Value& v) { return writeCompactJson(v); }

std::string pcmBase64(const std::vector<std::int16_t>& pcm) {
    std::vector<std::uint8_t> bytes;
    for (std::int16_t s : pcm) {
        bytes.push_back(std::uint8_t(std::uint16_t(s) & 0xFF));
        bytes.push_back(std::uint8_t(std::uint16_t(s) >> 8));
    }
    return base64Encode(bytes);
}

std::vector<std::int16_t> pcmOf(const json::Value& append) {
    std::vector<std::int16_t> out;
    Result<std::vector<std::uint8_t>> bytes = base64Decode(append["audio"].stringOr(""));
    if (!bytes) return out;
    for (std::size_t i = 0; i + 1 < bytes->size(); i += 2)
        out.push_back(std::int16_t(std::uint16_t((*bytes)[i]) | (std::uint16_t((*bytes)[i + 1]) << 8)));
    return out;
}

struct LogCapture {
    std::string all;
    int id;
    LogCapture() {
        id = log::addSink([this](log::Level, std::string_view category, std::string_view message) {
            all += std::string(category) + ": " + std::string(message) + "\n";
        });
    }
    ~LogCapture() { log::removeSink(id); }
};

// A session on a fake server that answers session.start and session.close as GPT-Live does.
struct LiveRig {
    net::FakeWebSocketServer server;
    double time = 0.0;
    double unixStart = 1800000000.0;
    double expiresIn = 3600.0;
    bool answerStart = true;
    int started = 0;
    std::vector<std::string> ran; // tool calls the handlers saw
    LiveSession s{
        LiveDesc{.fake = &server, .clock = [this] { return time; }, .unixClock = [this] { return unixStart + time; }}};

    LiveRig() {
        s.settings().apiKey = kLiveKey;
        s.settings().instructions = "You are the station's computer. Speak English.";
        s.settings().backendInstructions = "Operate the station through its tools.";
        server.answer(kLiveUrl, {.delay = 0.05});
        server.onMessage([this](net::FakeWsPeer& peer, const net::WsMessage& m) {
            const json::Value e = parse(m.data);
            const std::string type = e["type"].stringOr("");
            if (type == "session.start" && answerStart) {
                ++started;
                peer.sendText(
                    std::format(R"({{"type":"session.started","event_id":"evt_s{}","client_event_id":"{}","session":)"
                                R"({{"id":"live_test{}","expires_at":{},"status":"active","model":"gpt-live-1"}}}})",
                                started, e["event_id"].stringOr(""), started,
                                (long long)(unixStart + time + expiresIn)),
                    0.1);
            } else if (type == "session.close") {
                peer.sendText(R"({"type":"session.closed","event_id":"evt_c","reason":"close_requested",)"
                              R"("session":{"id":"live_test","expires_at":0,"status":"active","model":"gpt-live-1"},)"
                              R"("usage":{"seconds":12.5}})",
                              0.1);
            }
        });
        Tool status;
        status.name = "get_status";
        status.description = "The station's power and its rovers.";
        status.handler = [this](const json::Value&) {
            ran.push_back("get_status");
            return ToolResult::ok(parse(R"({"power_kw":12,"rovers":["Rover 1","Rover 2"]})"));
        };
        (void)s.tools().add(std::move(status));
        Tool send;
        send.name = "send_rover";
        send.description = "Send a rover to a cell.";
        send.inputSchema = parse(R"({"type":"object","properties":{"rover":{"type":"string","enum":["Rover 1",)"
                                 R"("Rover 2"]},"cell":{"type":"string"}},"required":["rover","cell"]})");
        send.handler = [this](const json::Value& args) {
            ran.push_back("send_rover " + args["rover"].string);
            return ToolResult::ok(args["rover"].string + " is on its way to " + args["cell"].string);
        };
        (void)s.tools().add(std::move(send));
    }

    void step(double dt = 0.02) {
        time += dt;
        s.update();
    }
    void run(double seconds, double dt = 0.02) {
        for (double t = 0.0; t < seconds - 1e-9; t += dt) step(dt);
    }
    bool startAndWait() {
        if (!s.start()) return false;
        for (int i = 0; i < 100 && s.state() != LiveState::Listening; ++i) step();
        return s.state() == LiveState::Listening;
    }
    net::FakeWsPeer* peer() { return server.last(); }
    // The client's messages of the last connection, parsed, of one type ("" for all).
    std::vector<json::Value> sent(const std::string& type = "") {
        std::vector<json::Value> out;
        if (!peer()) return out;
        for (const net::WsMessage& m : peer()->received()) {
            json::Value v = parse(m.data);
            if (type.empty() || v["type"].stringOr("") == type) out.push_back(std::move(v));
        }
        return out;
    }
    void serverSends(const std::string& text, double delay = 0.0) { peer()->sendText(text, delay); }
    // A function call of the backend in a response.event envelope.
    std::string call(const std::string& delegation, const std::string& callId, const std::string& name,
                     const std::string& arguments) {
        json::Value item = json::Value::makeObject();
        item.object["type"] = json::Value::makeString("function_call");
        item.object["call_id"] = json::Value::makeString(callId);
        item.object["name"] = json::Value::makeString(name);
        item.object["arguments"] = json::Value::makeString(arguments);
        item.object["status"] = json::Value::makeString("completed");
        json::Value event = json::Value::makeObject();
        event.object["type"] = json::Value::makeString("response.output_item.done");
        event.object["output_index"] = json::Value::makeNumber(0);
        event.object["item"] = std::move(item);
        json::Value envelope = json::Value::makeObject();
        envelope.object["type"] = json::Value::makeString("response.event");
        envelope.object["event_id"] = json::Value::makeString("evt_r");
        envelope.object["delegation_id"] = json::Value::makeString(delegation);
        envelope.object["event"] = std::move(event);
        return compact(envelope);
    }
    std::string responseEvent(const std::string& delegation, const std::string& type, int input = 0, int output = 0) {
        return std::format(R"({{"type":"response.event","event_id":"evt_r","delegation_id":"{}","event":)"
                           R"({{"type":"{}","sequence_number":1,"response":{{"id":"resp_1","output":[],)"
                           R"("usage":{{"input_tokens":{},"input_tokens_details":{{"cached_tokens":0}},)"
                           R"("output_tokens":{}}}}}}}}})",
                           delegation, type, input, output);
    }
    std::string delegationCreated(const std::string& id) {
        return std::format(R"({{"type":"session.delegation.created","event_id":"evt_d","offset_ms":1000,)"
                           R"("delegation":{{"id":"{}","type":"delegation","target":"responses",)"
                           R"("response_id":"resp_1"}}}})",
                           id);
    }
};

bool has(const std::vector<LiveEvent>& events, LiveEventKind kind) {
    for (const LiveEvent& e : events)
        if (e.kind == kind) return true;
    return false;
}

} // namespace

QUENCH_TEST(live_start_handshake) {
    LiveRig rig;
    CHECK(!LiveSession().start(), "no key: refused");
    REQUIRE(rig.s.start(), "start");
    CHECK(rig.s.state() == LiveState::Connecting && rig.s.running(), "connecting");
    CHECK(!rig.s.start(), "one session at a time");
    rig.step(0.06);
    REQUIRE(rig.s.state() == LiveState::Starting, "starting: %s", liveStateName(rig.s.state()));
    rig.step(0.01); // what the client sent reaches the fake server in its next poll
    REQUIRE(rig.peer(), "connected");
    CHECK(rig.peer()->url() == kLiveUrl, "the URL without query parameters");
    const std::string* auth = net::findHeader(rig.peer()->headers(), "Authorization");
    CHECK(auth && *auth == std::string("Bearer ") + kLiveKey, "the key as a bearer token");
    CHECK(rig.peer()->protocols().empty(), "no subprotocol");
    const std::vector<json::Value> all = rig.sent();
    REQUIRE(all.size() == 1 && all[0]["type"].stringOr("") == "session.start", "session.start first");
    const json::Value& session = all[0]["session"];
    CHECK(session["model"].stringOr("") == "gpt-live-1", "model");
    CHECK(session["audio"]["format"]["type"].stringOr("") == "audio/pcm" &&
              session["audio"]["format"]["rate"].numberOr(0) == 24000,
          "PCM at 24 kHz");
    CHECK(session["audio"]["output"]["voice"].stringOr("") == "marin", "voice");
    const json::Value& responses = session["delegation"]["responses"];
    CHECK(session["delegation"]["type"].stringOr("") == "responses", "Responses delegation");
    CHECK(responses["model"].stringOr("") == "gpt-6-luna" && responses["tool_choice"].stringOr("") == "auto" &&
              responses["parallel_tool_calls"].boolOr(false),
          "backend settings");
    REQUIRE(responses["tools"].array.size() == 2, "two tools");
    CHECK(responses["tools"].array[1]["type"].stringOr("") == "function" &&
              responses["tools"].array[1]["name"].stringOr("") == "send_rover" &&
              responses["tools"].array[1]["parameters"]["required"].array.size() == 2,
          "a function tool in the Responses shape");
    const std::string instructions = session["instructions"].stringOr("");
    CHECK(instructions.starts_with("You are the station") &&
              instructions.find("- send_rover: Send a rover") != std::string::npos &&
              instructions.find("<|SpawnThinking|>") != std::string::npos,
          "the game's instructions and the delegation policy");
    CHECK(responses["instructions"].stringOr("") == "Operate the station through its tools.", "backend prompt");
    CHECK(compact(all[0]).find(kLiveKey) == std::string::npos, "no key in session.start");
    rig.step(0.05);
    CHECK(rig.s.state() == LiveState::Starting, "waits for session.started");
    rig.step(0.06);
    REQUIRE(rig.s.state() == LiveState::Listening, "listening: %s", liveStateName(rig.s.state()));
    CHECK(rig.s.stats().sessionId == "live_test1" && rig.s.stats().expiresAt > 0, "the session's id");
    CHECK(std::abs(rig.s.stats().startSeconds - 0.17) < 0.03, "handshake %.3f s", rig.s.stats().startSeconds);
    const std::vector<LiveEvent> events = rig.s.takeEvents();
    CHECK(has(events, LiveEventKind::Started), "a Started event");
}

QUENCH_TEST(live_audio_both_ways) {
    LiveRig rig;
    REQUIRE(rig.startAndWait(), "started");
    std::vector<float> mic(6000); // 0.25 s at 24 kHz
    for (std::size_t i = 0; i < mic.size(); ++i) mic[i] = 0.5f * float(std::sin(double(i) * 0.05));
    rig.s.sendAudio(mic);
    rig.step();
    std::vector<json::Value> appends = rig.sent("session.input_audio.append");
    REQUIRE(appends.size() == 2, "two 100 ms pieces sent: %zu", appends.size());
    const std::vector<std::int16_t> first = pcmOf(appends[0]);
    CHECK(first.size() == 2400, "2400 samples (4800 bytes) each: %zu", first.size());
    CHECK(!first.empty() && first[100] == std::int16_t(std::lround(mic[100] * 32767.0f)), "PCM16 of the input");
    rig.s.sendAudio(std::span(mic).subspan(0, 1200));
    rig.step();
    CHECK(rig.sent("session.input_audio.append").size() == 3, "the rest with the next samples");

    std::vector<std::int16_t> pcm(4800);
    for (std::size_t i = 0; i < pcm.size(); ++i) pcm[i] = std::int16_t(i * 7 - 16000);
    rig.serverSends(R"({"type":"session.output_audio.delta","delta":")" + pcmBase64(pcm) + "\"}");
    rig.step();
    std::vector<float> out;
    CHECK(rig.s.takeAudio(out) == 4800, "4800 samples received");
    CHECK(std::abs(out[10] - float(pcm[10]) / 32768.0f) < 1e-6f, "decoded");
    CHECK(rig.s.state() == LiveState::Speaking, "speaking while audio arrives: %s", liveStateName(rig.s.state()));
    CHECK(rig.s.takeAudio(out) == 0, "taken once");
    rig.run(0.4);
    CHECK(rig.s.state() == LiveState::Listening, "listening once it stops: %s", liveStateName(rig.s.state()));
    CHECK(std::abs(rig.s.stats().audioReceivedSeconds - 0.2) < 1e-9 && rig.s.stats().audioSentSeconds > 0.29,
          "counted");
}

QUENCH_TEST(live_audio_before_start_is_kept) {
    LiveRig rig;
    REQUIRE(rig.s.start(), "start");
    std::vector<float> words(7200, 0.25f); // 0.3 s spoken while connecting
    rig.s.sendAudio(words);
    for (int i = 0; i < 20 && rig.s.state() != LiveState::Listening; ++i) rig.step();
    REQUIRE(rig.s.state() == LiveState::Listening, "started");
    rig.step();
    const std::vector<json::Value> all = rig.sent();
    REQUIRE(all.size() == 4, "session.start, then the opening words: %zu", all.size());
    CHECK(all[0]["type"].stringOr("") == "session.start" && all[1]["type"].stringOr("") == "session.input_audio.append",
          "in order");
    CHECK(pcmOf(all[3]).size() == 2400 && pcmOf(all[3])[0] == std::int16_t(std::lround(0.25f * 32767.0f)),
          "the words, not silence");
}

QUENCH_TEST(live_transcripts_in_pieces) {
    LiveRig rig;
    REQUIRE(rig.startAndWait(), "started");
    rig.s.takeEvents();
    rig.serverSends(
        R"({"type":"session.input_transcript.delta","event_id":"e1","delta":"What is","start_ms":1000,"end_ms":1200})");
    rig.serverSends(
        R"({"type":"session.input_transcript.delta","event_id":"e2","delta":" the power","start_ms":1200,"end_ms":1600})");
    rig.serverSends(
        R"({"type":"session.output_transcript.delta","event_id":"e3","delta":"Twelve","start_ms":2000,"end_ms":2300})");
    rig.serverSends(
        R"({"type":"session.output_transcript.delta","event_id":"e4","delta":" kilowatts.","start_ms":2300,"end_ms":2900})");
    rig.serverSends(
        R"({"type":"session.input_transcript.delta","event_id":"e5","delta":"Thanks","start_ms":3000,"end_ms":3300})");
    rig.step();
    const std::vector<LiveTurn>& h = rig.s.history();
    REQUIRE(h.size() == 3, "three turns: %zu", h.size());
    CHECK(h[0].speaker == LiveSpeaker::Player && h[0].text == "What is the power" && h[0].startMs == 1000 &&
              h[0].endMs == 1600,
          "the player's pieces joined as received: '%s'", h[0].text.c_str());
    CHECK(h[1].speaker == LiveSpeaker::Station && h[1].text == "Twelve kilowatts.", "the station's");
    CHECK(h[2].text == "Thanks", "a new turn after the other side spoke");
    const std::vector<LiveEvent> events = rig.s.takeEvents();
    int input = 0, output = 0;
    for (const LiveEvent& e : events) {
        if (e.kind == LiveEventKind::InputTranscript) ++input;
        if (e.kind == LiveEventKind::OutputTranscript) ++output;
    }
    CHECK(input == 3 && output == 2, "an event per delta: %d, %d", input, output);
    const json::Value saved = rig.s.historyJson();
    CHECK(saved["turns"].array.size() == 3 && saved["turns"].array[1]["speaker"].stringOr("") == "station", "saved");
}

QUENCH_TEST(live_tool_call_and_result) {
    LiveRig rig;
    REQUIRE(rig.startAndWait(), "started");
    rig.s.takeEvents();
    rig.serverSends(rig.delegationCreated("del_1"));
    rig.serverSends(rig.responseEvent("del_1", "response.created"));
    rig.serverSends(rig.call("del_1", "call_a", "send_rover", R"({"rover":"Rover 2","cell":"E7"})"));
    rig.serverSends(rig.responseEvent("del_1", "response.completed", 900, 40));
    rig.step();
    CHECK(rig.s.state() == LiveState::CallingTool && rig.s.toolName() == "send_rover", "calling: %s %s",
          liveStateName(rig.s.state()), rig.s.toolName().c_str());
    CHECK(rig.ran.empty(), "visible for one update before it runs");
    rig.step();
    REQUIRE(rig.ran.size() == 1 && rig.ran[0] == "send_rover Rover 2", "ran");
    rig.step();
    const std::vector<json::Value> outputs = rig.sent("response.item.create");
    REQUIRE(outputs.size() == 1, "one result");
    CHECK(outputs[0]["item"]["type"].stringOr("") == "function_call_output" &&
              outputs[0]["item"]["call_id"].stringOr("") == "call_a" &&
              outputs[0]["item"]["output"].stringOr("") == "Rover 2 is on its way to E7",
          "the result: %s", compact(outputs[0]).c_str());
    CHECK(rig.sent("response.create").size() == 1, "then response.create");
    CHECK(rig.s.state() == LiveState::Listening, "listening again: %s", liveStateName(rig.s.state()));
    rig.time += 0.5;
    rig.serverSends(rig.responseEvent("del_1", "response.created"));
    rig.serverSends(rig.responseEvent("del_1", "response.completed", 1000, 20));
    rig.step();
    const std::vector<LiveEvent> events = rig.s.takeEvents();
    CHECK(has(events, LiveEventKind::ToolStarted) && has(events, LiveEventKind::ToolFinished) &&
              has(events, LiveEventKind::DelegationDone),
          "tool and delegation events");
    CHECK(rig.s.stats().lastDelegationSeconds > 0.5, "the delegation's time: %.3f",
          rig.s.stats().lastDelegationSeconds);
    const LiveUsage u = rig.s.usage();
    CHECK(u.backendInputTokens == 1900 && u.backendOutputTokens == 60, "backend tokens %lld, %lld",
          (long long)u.backendInputTokens, (long long)u.backendOutputTokens);
    CHECK(std::abs(u.backendCost - (1900 * 0.10 + 60 * 0.50) / 1e6) < 1e-12, "backend cost %g", u.backendCost);
}

QUENCH_TEST(live_parallel_calls) {
    LiveRig rig;
    REQUIRE(rig.startAndWait(), "started");
    rig.serverSends(rig.delegationCreated("del_2"));
    rig.serverSends(rig.responseEvent("del_2", "response.created"));
    rig.serverSends(rig.call("del_2", "call_1", "get_status", "{}"));
    rig.serverSends(rig.call("del_2", "call_2", "send_rover", R"({"rover":"Rover 1","cell":"B2"})"));
    rig.serverSends(rig.responseEvent("del_2", "response.completed"));
    rig.step();
    rig.step();
    CHECK(rig.ran.size() == 1, "one call per update");
    rig.step();
    CHECK(rig.sent("response.create").empty(), "no continuation before every result");
    rig.step();
    rig.step();
    CHECK(rig.ran.size() == 2, "both ran");
    const std::vector<json::Value> outputs = rig.sent("response.item.create");
    REQUIRE(outputs.size() == 2, "two results");
    CHECK(outputs[0]["item"]["call_id"].stringOr("") == "call_1" &&
              outputs[1]["item"]["call_id"].stringOr("") == "call_2",
          "in order");
    CHECK(parse(outputs[0]["item"]["output"].stringOr(""))["power_kw"].numberOr(0) == 12, "structured as JSON");
    CHECK(rig.sent("response.create").size() == 1, "one response.create after both");
    const std::vector<json::Value> all = rig.sent();
    CHECK(all.back()["type"].stringOr("") == "response.create", "and it comes last");
}

QUENCH_TEST(live_bad_call_refused_by_schema) {
    LiveRig rig;
    REQUIRE(rig.startAndWait(), "started");
    rig.serverSends(rig.responseEvent("del_3", "response.created"));
    rig.serverSends(rig.call("del_3", "call_x", "send_rover", R"({"rover":"Rover 9","cell":"E7"})"));
    rig.serverSends(rig.call("del_3", "call_y", "launch_missiles", "{}"));
    rig.serverSends(rig.responseEvent("del_3", "response.completed"));
    rig.run(0.2);
    CHECK(rig.ran.empty(), "no handler ran");
    const std::vector<json::Value> outputs = rig.sent("response.item.create");
    REQUIRE(outputs.size() == 2, "two error results");
    const std::string a = outputs[0]["item"]["output"].stringOr("");
    const std::string b = outputs[1]["item"]["output"].stringOr("");
    CHECK(a.starts_with("Error: ") && a.find("rover") != std::string::npos, "the schema's message: %s", a.c_str());
    CHECK(b.starts_with("Error: ") && b.find("unknown tool") != std::string::npos, "unknown: %s", b.c_str());
    CHECK(rig.sent("response.create").size() == 1, "the backend continues and can correct itself");
    CHECK(rig.s.toolCalls().size() == 2 && rig.s.toolCalls()[0].result.isError, "recorded");
}

QUENCH_TEST(live_injected_context) {
    LiveRig rig;
    REQUIRE(rig.s.start(), "start");
    Result<std::string> early = rig.s.appendThinking("Rover 2 has arrived at E7.");
    REQUIRE(early, "queued while starting");
    CHECK(!rig.s.appendCommentary(""), "empty refused");
    for (int i = 0; i < 20 && rig.s.state() != LiveState::Listening; ++i) rig.step();
    REQUIRE(rig.s.state() == LiveState::Listening, "started");
    rig.step();
    std::vector<json::Value> thinking = rig.sent("session.thinking.append");
    REQUIRE(thinking.size() == 1, "sent once the session started");
    CHECK(thinking[0]["content"].stringOr("") == "Rover 2 has arrived at E7." &&
              thinking[0]["delegation_id"].isNull() && thinking[0].has("delegation_id") &&
              thinking[0]["event_id"].stringOr("") == *early,
          "content, a null delegation_id, the event id");
    Result<std::string> spoken = rig.s.appendCommentary("Tell the player: the storm arrives in two minutes.");
    Result<std::string> rule = rig.s.appendInstructions("Keep answers under two sentences.");
    REQUIRE(spoken && rule, "sent");
    rig.step();
    CHECK(rig.sent("session.commentary.append").size() == 1 && rig.sent("session.instructions.append").size() == 1,
          "commentary and instructions");
    rig.s.takeEvents();
    rig.serverSends(std::format(R"({{"type":"session.commentary.appended","event_id":"e","client_event_id":"{}",)"
                                R"("start_ms":5200,"end_ms":5400}})",
                                *spoken));
    rig.step();
    const std::vector<LiveEvent> events = rig.s.takeEvents();
    bool acked = false;
    for (const LiveEvent& e : events) acked |= e.kind == LiveEventKind::Appended && e.text == *spoken;
    CHECK(acked, "the acknowledgement names the append");
    rig.s.cancel();
    CHECK(!rig.s.appendThinking("late"), "no session: refused");
}

QUENCH_TEST(live_uplink_gate) {
    LiveRig rig;
    rig.s.settings().gateHoldSeconds = 0.25;
    REQUIRE(rig.startAndWait(), "started");
    const std::vector<float> voice(2400, 0.5f);
    rig.s.setPlaybackActive(true);
    CHECK(rig.s.gateClosed(), "closed while the station plays");
    rig.s.sendAudio(voice);
    rig.step();
    rig.s.setPlaybackActive(false);
    rig.step(0.1);
    CHECK(rig.s.gateClosed(), "held after the playback (echo tail)");
    rig.s.sendAudio(voice);
    rig.step(0.2);
    CHECK(!rig.s.gateClosed(), "open again");
    rig.s.sendAudio(voice);
    rig.step();
    std::vector<json::Value> appends = rig.sent("session.input_audio.append");
    REQUIRE(appends.size() == 3, "audio keeps flowing: %zu", appends.size());
    CHECK(pcmOf(appends[0])[5] == 0 && pcmOf(appends[1])[5] == 0, "silence while closed");
    CHECK(pcmOf(appends[2])[5] != 0, "the microphone once open");
    // Headphones: no gate.
    rig.s.settings().uplinkGate = false;
    rig.s.setPlaybackActive(true);
    rig.s.sendAudio(voice);
    rig.step();
    appends = rig.sent("session.input_audio.append");
    CHECK(!rig.s.gateClosed() && pcmOf(appends.back())[5] != 0, "the microphone while the station plays");
    rig.s.takeEvents();
    rig.serverSends(
        R"({"type":"session.input_transcript.delta","event_id":"e","delta":"Stop","start_ms":0,"end_ms":100})");
    rig.step();
    CHECK(has(rig.s.takeEvents(), LiveEventKind::PlayerOverlap), "an overlap event for the game");
}

QUENCH_TEST(live_idle_close) {
    LiveRig rig;
    rig.s.settings().idleSeconds = 5.0;
    REQUIRE(rig.startAndWait(), "started");
    rig.run(3.0);
    rig.serverSends(
        R"({"type":"session.input_transcript.delta","event_id":"e","delta":"Hello","start_ms":0,"end_ms":100})");
    rig.run(4.0);
    CHECK(rig.s.state() == LiveState::Listening, "speech restarted the timer");
    rig.s.setPlaybackActive(true);
    rig.run(3.0);
    rig.s.setPlaybackActive(false);
    rig.run(4.0);
    CHECK(rig.s.state() == LiveState::Listening, "playback counts as speech");
    rig.run(1.2);
    CHECK(rig.sent("session.close").size() == 1, "session.close after 5 quiet seconds");
    rig.run(0.3);
    CHECK(rig.s.state() == LiveState::Closed && rig.s.closeReason() == LiveCloseReason::Idle, "closed: %s, %s",
          liveStateName(rig.s.state()), liveCloseReasonName(rig.s.closeReason()));
    const LiveUsage u = rig.s.usage();
    CHECK(u.confirmed && std::abs(u.sessionSeconds - 12.5) < 1e-9, "final usage from session.closed");
    CHECK(std::abs(u.sessionCost - 12.5 / 60.0 * 0.05) < 1e-12, "cost %g", u.sessionCost);
}

QUENCH_TEST(live_server_close_reasons) {
    const std::pair<const char*, LiveCloseReason> cases[] = {
        {"close_requested", LiveCloseReason::Requested},
        {"content", LiveCloseReason::Content},
        {"remote_hangup", LiveCloseReason::RemoteHangup},
        {"connection_lost", LiveCloseReason::ConnectionLost},
    };
    for (const auto& [reason, expected] : cases) {
        LiveRig rig;
        REQUIRE(rig.startAndWait(), "started");
        rig.serverSends(
            std::format(R"({{"type":"session.closed","event_id":"e","reason":"{}","session":{{"id":"x",)"
                        R"("expires_at":0,"status":"active","model":"gpt-live-1"}},"usage":{{"seconds":3}}}})",
                        reason));
        rig.run(0.1);
        CHECK(rig.s.state() == LiveState::Closed && rig.s.closeReason() == expected, "%s: %s", reason,
              liveCloseReasonName(rig.s.closeReason()));
        CHECK(rig.server.peers().size() == 1, "%s: no reconnect", reason);
    }
    // The connection lost without session.closed: closed, the usage unconfirmed.
    LiveRig rig;
    REQUIRE(rig.startAndWait(), "started");
    rig.serverSends(
        R"({"type":"session.usage.updated","event_id":"u","usage":{"seconds":7},"context_window":{"usage_ratio":0.1}})");
    rig.peer()->fail(net::ErrorKind::NoNetwork, "lost", 0.05);
    rig.run(0.2);
    CHECK(rig.s.state() == LiveState::Closed && rig.s.closeReason() == LiveCloseReason::ConnectionLost, "lost");
    CHECK(!rig.s.usage().confirmed && rig.s.usage().sessionSeconds == 7.0 && rig.s.usage().contextRatio == 0.1,
          "the last usage stands, unconfirmed");
}

QUENCH_TEST(live_expiry_reconnects_with_history) {
    LiveRig rig;
    rig.s.setStateSummary([] { return std::string("Day 3. Rover 2 is at E7."); });
    REQUIRE(rig.startAndWait(), "started");
    rig.serverSends(
        R"({"type":"session.input_transcript.delta","event_id":"a","delta":"Where is rover two?","start_ms":0,"end_ms":900})");
    rig.serverSends(
        R"({"type":"session.output_transcript.delta","event_id":"b","delta":"At E7.","start_ms":1000,"end_ms":1500})");
    rig.step();
    rig.serverSends(R"({"type":"session.closed","event_id":"e","reason":"expired","session":{"id":"x","expires_at":0,)"
                    R"("status":"active","model":"gpt-live-1"},"usage":{"seconds":3600}})");
    rig.s.takeEvents();
    rig.step();
    bool expired = false, reconnecting = false;
    for (const LiveEvent& e : rig.s.takeEvents()) {
        expired |= e.kind == LiveEventKind::Closed && e.reason == LiveCloseReason::Expired;
        reconnecting |= e.kind == LiveEventKind::Reconnecting;
    }
    CHECK(expired && reconnecting, "closed as expired, then reconnecting");
    for (int i = 0; i < 30 && rig.s.state() != LiveState::Listening; ++i) rig.step();
    CHECK(rig.s.state() == LiveState::Listening && rig.server.peers().size() == 2 && rig.s.stats().sessions == 2,
          "a second session: %s, %zu connections", liveStateName(rig.s.state()), rig.server.peers().size());
    const std::vector<json::Value> starts = rig.sent("session.start");
    REQUIRE(starts.size() == 1, "its session.start");
    const json::Value& input = starts[0]["session"]["input"];
    REQUIRE(input.array.size() == 3, "the state and two messages: %zu", input.array.size());
    CHECK(input.array[0]["role"].stringOr("") == "developer" &&
              input.array[0]["content"].array[0]["text"].stringOr("") == "Day 3. Rover 2 is at E7.",
          "the state summary first");
    CHECK(input.array[1]["role"].stringOr("") == "user" &&
              input.array[1]["content"].array[0]["type"].stringOr("") == "input_text" &&
              input.array[1]["content"].array[0]["text"].stringOr("") == "Where is rover two?",
          "the player's words");
    CHECK(input.array[2]["role"].stringOr("") == "assistant" &&
              input.array[2]["content"].array[0]["type"].stringOr("") == "output_text" &&
              input.array[2]["content"].array[0]["text"].stringOr("") == "At E7.",
          "the station's");
    CHECK(rig.s.usage().totalSeconds == 3600.0, "the first session's seconds count");
}

QUENCH_TEST(live_reconnects_before_expiry_when_quiet) {
    LiveRig rig;
    rig.expiresIn = 100.0;
    rig.s.settings().idleSeconds = 0.0;
    REQUIRE(rig.startAndWait(), "started");
    rig.run(65.0, 0.1);
    CHECK(rig.server.peers().size() == 1, "not yet");
    rig.run(10.0, 0.1);
    CHECK(rig.server.peers().size() == 2 && rig.s.state() == LiveState::Listening,
          "a new session about 30 s before the expiry: %zu", rig.server.peers().size());
    CHECK(rig.server.peers()[0]->clientCloseCode().has_value() || rig.server.peers()[0]->aborted(),
          "the old connection ended");
}

QUENCH_TEST(live_error_events) {
    {
        LiveRig rig;
        REQUIRE(rig.startAndWait(), "started");
        rig.s.takeEvents();
        rig.serverSends(
            R"({"type":"error","event_id":"e","error":{"type":"invalid_request_error",)"
            R"("code":"tool_outputs_required","message":"Submit every tool output.","client_event_id":"x"}})");
        rig.step();
        CHECK(rig.s.state() == LiveState::Listening, "the session goes on");
        CHECK(rig.s.lastServerError() == "tool_outputs_required: Submit every tool output.", "'%s'",
              rig.s.lastServerError().c_str());
        CHECK(has(rig.s.takeEvents(), LiveEventKind::ServerError) && rig.s.stats().serverErrors == 1, "an event");
    }
    {
        // Before session.started (a model the account may not use): the session fails with the server's message.
        LiveRig rig;
        rig.answerStart = false;
        rig.server.onMessage([](net::FakeWsPeer& peer, const net::WsMessage&) {
            peer.sendText(R"({"type":"error","event_id":"e","error":{"type":"invalid_request_error",)"
                          R"("code":"model_not_found","message":"The model gpt-live-1 does not exist or you do not )"
                          R"(have access to it."}})");
            peer.close(1008, "error", 0.01);
        });
        REQUIRE(rig.s.start(), "start");
        rig.run(0.3);
        CHECK(rig.s.state() == LiveState::Failed && rig.s.failure().kind == ErrorKind::Refused, "failed: %s, %s",
              liveStateName(rig.s.state()), errorKindName(rig.s.failure().kind));
        CHECK(rig.s.failure().message.find("does not exist or you do not have access") != std::string::npos,
              "the server's message: %s", rig.s.failure().message.c_str());
    }
    {
        LiveRig rig;
        rig.server.answer(kLiveUrl, {.status = 401});
        REQUIRE(rig.s.start(), "start");
        rig.run(0.1);
        CHECK(rig.s.state() == LiveState::Failed && rig.s.failure().kind == ErrorKind::BadKey &&
                  rig.s.failure().status == 401,
              "401 at the upgrade: %s", errorKindName(rig.s.failure().kind));
    }
    {
        LiveRig rig;
        rig.answerStart = false;
        rig.s.settings().startTimeout = 2.0;
        REQUIRE(rig.s.start(), "start");
        rig.run(2.2);
        CHECK(rig.s.state() == LiveState::Failed && rig.s.failure().kind == ErrorKind::Timeout, "no session.started");
    }
}

QUENCH_TEST(live_cancel_and_close) {
    {
        LiveRig rig;
        REQUIRE(rig.s.start(), "start");
        rig.step(0.06);
        rig.s.cancel();
        CHECK(rig.s.state() == LiveState::Closed && rig.s.closeReason() == LiveCloseReason::Cancelled, "cancelled");
        rig.step();
        CHECK(rig.peer()->aborted(), "the connection dropped");
        CHECK(!rig.s.usage().confirmed, "unconfirmed");
    }
    {
        LiveRig rig;
        REQUIRE(rig.startAndWait(), "started");
        rig.s.close();
        CHECK(rig.s.state() == LiveState::Closing, "closing");
        rig.run(0.2);
        CHECK(rig.s.state() == LiveState::Closed && rig.s.closeReason() == LiveCloseReason::Requested, "closed");
        CHECK(rig.s.usage().confirmed && rig.s.usage().sessionSeconds == 12.5, "confirmed");
        // A new start carries the (empty) history and works again.
        REQUIRE(rig.startAndWait(), "started again");
        CHECK(rig.server.peers().size() == 2, "a second connection");
    }
}

QUENCH_TEST(live_key_in_no_log_line) {
    LogCapture log;
    {
        LiveRig rig;
        REQUIRE(rig.startAndWait(), "started");
        rig.serverSends(
            std::string(R"({"type":"error","event_id":"e","error":{"type":"x","code":"y","message":"bad key )") +
            kLiveKey + "\"}}");
        rig.serverSends(rig.responseEvent("d", "response.created"));
        rig.serverSends(rig.call("d", "c", "get_status", "{}"));
        rig.serverSends(rig.responseEvent("d", "response.completed"));
        rig.run(0.2);
        rig.s.close();
        rig.run(0.3);
        CHECK(rig.s.lastServerError().find(kLiveKey) == std::string::npos, "redacted in the error");
        CHECK(compact(rig.s.historyJson()).find(kLiveKey) == std::string::npos, "not in the history");
        CHECK(compact(rig.s.startEvent()).find(kLiveKey) == std::string::npos, "not in session.start");
    }
    {
        LiveRig rig;
        rig.server.answer(kLiveUrl, {.status = 401});
        REQUIRE(rig.s.start(), "start");
        rig.run(0.1);
        CHECK(rig.s.failure().message.find(kLiveKey) == std::string::npos, "not in a failure");
    }
    CHECK(log.all.find("api.openai.com/v1/live/sessions") != std::string::npos, "the session was logged");
    CHECK(log.all.find(kLiveKey) == std::string::npos, "the key in no log line");
}

// GPT-Live sends audio all the time, silence (zeros) between answers (seen on 2026-11-14): silence is passed on to
// the game but is neither "speaking" nor activity for the idle close.
QUENCH_TEST(live_silent_audio_is_not_speech) {
    LiveRig rig;
    rig.s.settings().idleSeconds = 3.0;
    REQUIRE(rig.startAndWait(), "started");
    const std::string silence =
        R"({"type":"session.output_audio.delta","delta":")" + pcmBase64(std::vector<std::int16_t>(2400, 0)) + "\"}";
    std::vector<float> out;
    for (int i = 0; i < 30 && rig.s.state() == LiveState::Listening; ++i) {
        rig.serverSends(silence);
        rig.run(0.1);
        rig.s.takeAudio(out);
    }
    CHECK(out.size() >= 2400 * 25, "the silence reached the game: %zu samples", out.size());
    rig.step();
    CHECK(rig.sent("session.close").size() == 1, "closed for idleness although audio kept coming");
    CHECK(rig.s.stats().audibleReceivedSeconds == 0.0 && rig.s.stats().audioReceivedSeconds > 2.5, "counted apart");
}

// The fallback: the tool results are in but response.completed never comes: response.create goes out once, after
// completedFallbackSeconds, with one log line; a late response.completed sends nothing more.
QUENCH_TEST(live_completed_fallback) {
    LogCapture log;
    LiveRig rig;
    rig.s.settings().completedFallbackSeconds = 15.0;
    REQUIRE(rig.startAndWait(), "started");
    rig.serverSends(rig.delegationCreated("del_f"));
    rig.serverSends(rig.responseEvent("del_f", "response.created"));
    rig.serverSends(rig.call("del_f", "call_f", "get_status", "{}"));
    rig.run(0.2);
    REQUIRE(rig.sent("response.item.create").size() == 1, "the result went out");
    rig.run(14.0, 0.1);
    CHECK(rig.sent("response.create").empty(), "waits for response.completed");
    rig.run(1.2, 0.1);
    CHECK(rig.sent("response.create").size() == 1, "sent anyway after 15 s");
    rig.serverSends(rig.responseEvent("del_f", "response.completed"));
    rig.run(0.5, 0.1);
    CHECK(rig.sent("response.create").size() == 1, "only once");
    // A second delegation that also misses its response.completed: continued, no second log line.
    rig.serverSends(rig.responseEvent("del_g", "response.created"));
    rig.serverSends(rig.call("del_g", "call_g", "get_status", "{}"));
    rig.run(16.0, 0.1);
    CHECK(rig.sent("response.create").size() == 2, "the second one too");
    std::size_t lines = 0;
    for (std::size_t at = log.all.find("no response.completed"); at != std::string::npos;
         at = log.all.find("no response.completed", at + 1))
        ++lines;
    CHECK(lines == 1, "one log line: %zu", lines);
}
