// Key material in a provider's own words (since 0.19.1): a provider that refuses a key echoes it, masked
// ("sk-or-v1****...****abcd") or whole, in its error body, its WebSocket error event or its close reason. None of it
// may reach a log line, a failure, an event or a history: the chat, transcription and speech requests of the
// Assistant and the LiveSession, each with a masked and a whole key.

#include "Rig.h"
#include "Test.h"

#include <quench/assistant/Live.h>
#include <quench/net/FakeWebSocket.h>

#include <format>
#include <string>
#include <vector>

using namespace quench;
using namespace quench::assistant;
using namespace assistant_test;

namespace {

// What OpenAI's API answers to a wrong key, the key masked as it does: its prefix, asterisks and an ellipsis, the
// last four characters.
const std::string kMasked = "sk-or-v1****\xE2\x80\xA6****0000";

std::string refusal(const std::string& key) {
    return std::format(R"({{"error":{{"message":"Incorrect API key provided: {}. You can find your API key at )"
                       R"(https://platform.openai.com/account/api-keys.","type":"invalid_request_error",)"
                       R"("param":null,"code":"invalid_api_key"}}}})",
                       key);
}

// Everything the assistant hands out or keeps that could carry a provider's words.
std::string everything(Assistant& a, std::vector<Event>& events) {
    std::string out = a.failure().message + "\n" + json::write(a.historyJson()) + "\n";
    for (const Event& e : a.takeEvents()) events.push_back(e);
    for (const Event& e : events) out += e.text + "\n";
    return out;
}

} // namespace

QUENCH_TEST(scrub_secrets_patterns) {
    const std::string key = kKey;
    CHECK(net::scrubSecrets("Incorrect API key provided: sk-or-v1****\xE2\x80\xA6****abcd. You can find it.") ==
              "Incorrect API key provided: <key>. You can find it.",
          "%s", net::scrubSecrets("Incorrect API key provided: sk-or-v1****\xE2\x80\xA6****abcd. You can").c_str());
    CHECK(net::scrubSecrets("a \"sk-proj-abc123\" b (sk-xyz) [sk-1] {sk-2} <sk-3>") ==
              "a \"<key>\" b (<key>) [<key>] {<key>} <<key>>",
          "%s", net::scrubSecrets("a \"sk-proj-abc123\" b (sk-xyz) [sk-1] {sk-2} <sk-3>").c_str());
    CHECK(net::scrubSecrets("a risk-free task-sk-1 sk- alone") == "a risk-free task-sk-1 sk- alone",
          "not inside a word, not a bare prefix");
    CHECK(net::scrubSecrets("Authorization: Bearer abc.def-123, then bearer\txyz") ==
              "Authorization: Bearer <key>, then bearer\t<key>",
          "%s", net::scrubSecrets("Authorization: Bearer abc.def-123, then bearer\txyz").c_str());
    CHECK(net::scrubSecrets("the key " + key + " and again" + key, key) == "the key <key> and again<key>",
          "the configured key itself, anywhere");
    CHECK(net::scrubSecrets("key: k3y-0000-not-sk", "k3y-0000-not-sk") == "key: <key>", "a key without sk-");
    CHECK(net::scrubSecrets("abc", "") == "abc" && net::scrubSecrets("ab", "a") == "ab", "a short key is not one");
    const std::string once = net::scrubSecrets("Bearer sk-1 sk-2 " + key, key);
    CHECK(net::scrubSecrets(once, key) == once && once == "Bearer <key> <key> <key>", "idempotent: %s", once.c_str());
    CHECK(redact("x " + key, key) == "x <key>", "redact() is the same function");
}

QUENCH_TEST(scrub_assistant_refused_key) {
    LogCapture log;
    std::string seen;
    std::vector<std::string> messages;
    for (const std::string& echoed : {kMasked, std::string(kKey)}) {
        // A turn that works first (a history to search), then each request refused with the key in the body.
        Rig rig;
        std::vector<Event> events;
        rig.fake.respond("POST", kChatUrl, stream(sseText({"Fine."})));
        REQUIRE(rig.a.ask("Hello") && rig.run() && rig.a.state() == State::Done, "a first turn");

        rig.fake.respond("POST", kChatUrl, jsonAnswer(401, refusal(echoed)));
        REQUIRE(rig.a.ask("Hello again"), "chat");
        rig.run();
        CHECK(rig.a.failure().kind == ErrorKind::BadKey, "chat: %s", errorKindName(rig.a.failure().kind));
        messages.push_back(rig.a.failure().message);
        seen += everything(rig.a, events);

        rig.fake.respond("POST", kSttUrl, jsonAnswer(401, refusal(echoed)));
        REQUIRE(rig.a.transcribe(wav(0.5)), "transcription");
        rig.run();
        CHECK(rig.a.failure().kind == ErrorKind::BadKey, "transcription: %s", errorKindName(rig.a.failure().kind));
        messages.push_back(rig.a.failure().message);
        seen += everything(rig.a, events);

        rig.fake.respond("POST", kTtsUrl, jsonAnswer(401, refusal(echoed)));
        REQUIRE(rig.a.speak("Say it"), "speech");
        rig.run();
        CHECK(rig.a.failure().kind == ErrorKind::BadKey, "speech: %s", errorKindName(rig.a.failure().kind));
        messages.push_back(rig.a.failure().message);
        seen += everything(rig.a, events);

        // An error in the middle of a stream, and a provider that quotes the header it received.
        const std::string midStream = event(chunk(contentDelta("Wo"))) + "data: " +
                                      std::format(R"x({{"error":{{"message":"upstream said: {} (Authorization: )x"
                                                  R"x(Bearer {})","code":401}}}})x",
                                                  echoed, echoed) +
                                      "\n\n";
        rig.fake.respond("POST", kChatUrl, stream(midStream));
        REQUIRE(rig.a.ask("Once more"), "stream");
        rig.run();
        CHECK(rig.a.failure().kind == ErrorKind::StreamError, "stream: %s", errorKindName(rig.a.failure().kind));
        messages.push_back(rig.a.failure().message);
        seen += everything(rig.a, events);

        std::size_t failed = 0;
        for (const Event& e : events) failed += e.kind == EventKind::Failed ? 1 : 0;
        CHECK(failed == 4, "a Failed event per refusal: %zu", failed);
    }
    for (const std::string& m : messages) {
        CHECK(m.find("<key>") != std::string::npos && m.find("sk-") == std::string::npos, "the message: %s", m.c_str());
    }
    CHECK(messages[0].find("Incorrect API key provided: <key>. You can find") != std::string::npos,
          "the provider's words are kept around the key: %s", messages[0].c_str());
    const std::string all = log.all();
    CHECK(all.find("assistant: thinking failed") != std::string::npos, "the failures were logged:\n%s", all.c_str());
    CHECK(all.find("sk-") == std::string::npos && all.find(kKey) == std::string::npos, "no key in the log:\n%s",
          all.c_str());
    CHECK(seen.find("sk-") == std::string::npos && seen.find(kKey) == std::string::npos,
          "no key in an error, an event or the history:\n%s", seen.c_str());
}

namespace {

constexpr const char* kLiveKey = "sk-proj-scrub0000test0000key0000not0000real";
const std::string kLiveUrl = "wss://api.openai.com/v1/live/sessions";

struct LiveScrubRig {
    net::FakeWebSocketServer server;
    double time = 0.0;
    LiveSession s{LiveDesc{.fake = &server, .clock = [this] { return time; }}};
    std::vector<LiveEvent> events;

    LiveScrubRig() {
        s.settings().apiKey = kLiveKey;
        server.answer(kLiveUrl, {.delay = 0.05});
    }
    void run(double seconds) {
        for (double t = 0.0; t < seconds; t += 0.02) {
            time += 0.02;
            s.update();
            for (LiveEvent& e : s.takeEvents()) events.push_back(std::move(e));
        }
    }
    std::string everything() const {
        std::string out = s.failure().message + "\n" + s.lastServerError() + "\n" + json::write(s.historyJson());
        for (const LiveEvent& e : events) out += "\n" + e.text;
        return out;
    }
};

std::string liveError(const std::string& code, const std::string& message) {
    json::Value err = json::Value::makeObject();
    err.object["type"] = json::Value::makeString("invalid_request_error");
    err.object["code"] = json::Value::makeString(code);
    err.object["message"] = json::Value::makeString(message);
    json::Value e = json::Value::makeObject();
    e.object["type"] = json::Value::makeString("error");
    e.object["event_id"] = json::Value::makeString("evt_e");
    e.object["error"] = std::move(err);
    return compact(e);
}

} // namespace

QUENCH_TEST(scrub_live_refused_key) {
    LogCapture log;
    std::string seen;
    for (const std::string& echoed : {kMasked, std::string(kLiveKey)}) {
        {
            // An error event in a running session: logged, kept in lastServerError(), handed out as an event and
            // to the event tap.
            LiveScrubRig rig;
            std::string tapped;
            rig.s.setEventTap([&](const json::Value& e) { tapped += compact(e) + "\n"; });
            rig.server.onMessage([](net::FakeWsPeer& peer, const net::WsMessage& m) {
                if (m.data.find("session.start") != std::string::npos)
                    peer.sendText(R"({"type":"session.started","event_id":"s","session":{"id":"live_scrub"}})");
            });
            REQUIRE(rig.s.start(), "start");
            rig.run(0.3);
            REQUIRE(rig.s.state() == LiveState::Listening, "%s", liveStateName(rig.s.state()));
            rig.server.last()->sendText(liveError("invalid_api_key", "Incorrect API key provided: " + echoed + "."));
            rig.run(0.1);
            CHECK(rig.s.lastServerError() == "invalid_api_key: Incorrect API key provided: <key>.", "'%s'",
                  rig.s.lastServerError().c_str());
            CHECK(tapped.find("Incorrect API key provided: <key>.") != std::string::npos, "tapped:\n%s",
                  tapped.c_str());
            seen += rig.everything() + "\n" + tapped;
        }
        {
            // Refused before session.started: the error event and then a close whose reason quotes the key.
            LiveScrubRig rig;
            rig.server.onMessage([echoed](net::FakeWsPeer& peer, const net::WsMessage&) {
                peer.sendText(liveError("invalid_api_key", "Incorrect API key provided: " + echoed));
                peer.close(1008, "bad key " + echoed, 0.01);
            });
            REQUIRE(rig.s.start(), "start");
            rig.run(0.3);
            CHECK(rig.s.state() == LiveState::Failed && rig.s.failure().kind == ErrorKind::BadKey, "%s, %s",
                  liveStateName(rig.s.state()), errorKindName(rig.s.failure().kind));
            CHECK(rig.s.failure().message.find("<key>") != std::string::npos, "%s", rig.s.failure().message.c_str());
            seen += rig.everything();
        }
        {
            // A clean close before session.started, its reason the only words.
            LiveScrubRig rig;
            rig.server.onMessage(
                [echoed](net::FakeWsPeer& peer, const net::WsMessage&) { peer.close(1008, "Bearer " + echoed, 0.01); });
            REQUIRE(rig.s.start(), "start");
            rig.run(0.3);
            CHECK(rig.s.state() == LiveState::Failed, "%s", liveStateName(rig.s.state()));
            CHECK(rig.s.failure().message.find("Bearer <key>") != std::string::npos, "%s",
                  rig.s.failure().message.c_str());
            seen += rig.everything();
        }
    }
    const std::string all = log.all();
    CHECK(all.find("live: server error") != std::string::npos && all.find("failed") != std::string::npos,
          "the errors were logged:\n%s", all.c_str());
    CHECK(all.find("sk-") == std::string::npos && all.find(kLiveKey) == std::string::npos, "no key in the log:\n%s",
          all.c_str());
    CHECK(seen.find("sk-") == std::string::npos && seen.find(kLiveKey) == std::string::npos,
          "no key in a failure, an error, an event or the history:\n%s", seen.c_str());
}
