#pragma once

// What the assistant's cases share: an assistant on a fake network with a clock the case moves, canned answers in the
// shape of OpenRouter's, the log a case captures.

#include <quench/assistant/Assistant.h>
#include <quench/core/Json.h>
#include <quench/core/Log.h>
#include <quench/net/FakeNetwork.h>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quench::test {
std::span<const std::uint8_t> parallelToolsSse();
std::span<const std::uint8_t> textAnswerSse();
std::span<const std::uint8_t> toneMp3();
} // namespace quench::test

namespace assistant_test {

using namespace quench;

// Not a real key: what the cases search for in logs, errors and saves.
inline constexpr const char* kKey = "sk-or-v1-test0000key0000not0000real0000";
inline constexpr const char* kBase = "https://openrouter.ai/api/v1";
inline const std::string kChatUrl = std::string(kBase) + "/chat/completions";
inline const std::string kSttUrl = std::string(kBase) + "/audio/transcriptions";
inline const std::string kTtsUrl = std::string(kBase) + "/audio/speech";

inline std::string text(std::span<const std::uint8_t> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

inline std::string compact(const json::Value& v) { return assistant::writeCompactJson(v); }

inline std::string event(const json::Value& chunk) { return "data: " + compact(chunk) + "\n\n"; }

inline json::Value parse(std::string_view text) { return json::parse(std::string(text)).valueOr({}); }

// A chunk of a streamed answer with this delta (and finish reason, and usage).
inline json::Value chunk(json::Value delta, const char* finish = nullptr, int promptTokens = -1, int completion = 0,
                         double cost = -1.0) {
    json::Value choice = json::Value::makeObject();
    choice.object["index"] = json::Value::makeNumber(0);
    choice.object["delta"] = std::move(delta);
    choice.object["finish_reason"] = finish ? json::Value::makeString(finish) : json::Value();
    json::Value c = json::Value::makeObject();
    c.object["id"] = json::Value::makeString("gen-test");
    c.object["object"] = json::Value::makeString("chat.completion.chunk");
    c.object["model"] = json::Value::makeString("test/model");
    c.object["choices"] = json::Value::makeArray();
    c.object["choices"].array.push_back(std::move(choice));
    if (promptTokens >= 0) {
        json::Value u = json::Value::makeObject();
        u.object["prompt_tokens"] = json::Value::makeNumber(promptTokens);
        u.object["completion_tokens"] = json::Value::makeNumber(completion);
        if (cost >= 0.0) u.object["cost"] = json::Value::makeNumber(cost);
        c.object["usage"] = std::move(u);
    }
    return c;
}

inline json::Value contentDelta(std::string piece) {
    json::Value d = json::Value::makeObject();
    d.object["role"] = json::Value::makeString("assistant");
    d.object["content"] = json::Value::makeString(std::move(piece));
    return d;
}

inline json::Value toolDelta(int index, std::string id, std::string name, std::string arguments) {
    json::Value fn = json::Value::makeObject();
    if (!name.empty()) fn.object["name"] = json::Value::makeString(std::move(name));
    fn.object["arguments"] = json::Value::makeString(std::move(arguments));
    json::Value call = json::Value::makeObject();
    call.object["index"] = json::Value::makeNumber(index);
    if (!id.empty()) {
        call.object["id"] = json::Value::makeString(std::move(id));
        call.object["type"] = json::Value::makeString("function");
    }
    call.object["function"] = std::move(fn);
    json::Value d = json::Value::makeObject();
    d.object["role"] = json::Value::makeString("assistant");
    d.object["content"] = json::Value();
    d.object["tool_calls"] = json::Value::makeArray();
    d.object["tool_calls"].array.push_back(std::move(call));
    return d;
}

struct Call {
    std::string id;
    std::string name;
    std::string arguments;
};

// A streamed answer that writes `pieces` and stops.
inline std::string sseText(const std::vector<std::string>& pieces, int promptTokens = 100, int completion = 10,
                           double cost = 0.0001) {
    std::string out = ": OPENROUTER PROCESSING\n\n";
    for (const std::string& p : pieces) out += event(chunk(contentDelta(p)));
    out += event(chunk(contentDelta(""), "stop", promptTokens, completion, cost));
    out += "data: [DONE]\n\n";
    return out;
}

// A streamed answer that asks for these tool calls, each one's arguments in two deltas.
inline std::string sseTools(const std::vector<Call>& calls, int promptTokens = 100, int completion = 20,
                            double cost = 0.0002) {
    std::string out;
    for (std::size_t i = 0; i < calls.size(); ++i) {
        const Call& c = calls[i];
        const std::size_t half = c.arguments.size() / 2;
        out += event(chunk(toolDelta(int(i), c.id, c.name, "")));
        out += event(chunk(toolDelta(int(i), "", "", c.arguments.substr(0, half))));
        out += event(chunk(toolDelta(int(i), "", "", c.arguments.substr(half))));
    }
    out += event(chunk(contentDelta(""), "tool_calls", promptTokens, completion, cost));
    out += "data: [DONE]\n\n";
    return out;
}

// A 200 answer of server-sent events, cut into pieces of `piece` bytes that arrive `delay` seconds apart.
inline net::FakeResponse stream(const std::string& body, std::size_t piece = 64, double delay = 0.01) {
    net::FakeResponse r;
    r.status = 200;
    r.headers = {{"Content-Type", "text/event-stream"}};
    for (std::size_t at = 0; at < body.size(); at += piece) r.body.push_back({delay, body.substr(at, piece)});
    return r;
}

inline net::FakeResponse jsonAnswer(int status, std::string body) {
    return net::FakeResponse::text(status, std::move(body), {{"Content-Type", "application/json"}});
}

// A 16-bit mono WAV of `seconds` of silence at 16 kHz (what a recording uploads).
inline std::vector<std::uint8_t> wav(double seconds) {
    const std::uint32_t frames = std::uint32_t(seconds * 16000.0);
    const std::uint32_t dataBytes = frames * 2;
    std::vector<std::uint8_t> out;
    const auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i)));
    };
    const auto u16 = [&](std::uint16_t v) {
        out.push_back(std::uint8_t(v));
        out.push_back(std::uint8_t(v >> 8));
    };
    const auto tag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    tag("RIFF");
    u32(36 + dataBytes);
    tag("WAVE");
    tag("fmt ");
    u32(16);
    u16(1);
    u16(1);
    u32(16000);
    u32(32000);
    u16(2);
    u16(16);
    tag("data");
    u32(dataBytes);
    for (std::uint32_t i = 0; i < frames; ++i) u16(std::uint16_t(std::int16_t((i % 50) * 100 - 2500)));
    return out;
}

// The log lines written while it lives.
struct LogCapture {
    std::vector<std::string> lines;
    int id;
    LogCapture() {
        id = log::addSink([this](log::Level, std::string_view category, std::string_view message) {
            lines.push_back(std::string(category) + ": " + std::string(message));
        });
    }
    ~LogCapture() { log::removeSink(id); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    std::string all() const {
        std::string out;
        for (const std::string& l : lines) out += l + "\n";
        return out;
    }
};

struct Rig {
    net::FakeNetwork fake;
    double time = 0.0;
    assistant::Assistant a{assistant::AssistantDesc{.fake = &fake, .clock = [this] { return time; }}};
    std::vector<assistant::State> states; // each update()'s state

    Rig() {
        a.settings() = assistant::openRouterPreset();
        a.settings().apiKey = kKey;
        a.setSystemPrompt("You are the station's assistant.");
    }

    void step(double dt = 0.02) {
        time += dt;
        a.update();
        states.push_back(a.state());
    }
    // Steps until the assistant is no longer busy (or `limit` steps).
    bool run(int limit = 2000) {
        for (int i = 0; i < limit && a.busy(); ++i) step();
        return !a.busy();
    }
    bool runUntil(const std::function<bool()>& done, int limit = 2000) {
        for (int i = 0; i < limit; ++i) {
            if (done()) return true;
            step();
        }
        return done();
    }
    // The bodies the fake received at this URL, as JSON.
    std::vector<json::Value> bodies(const std::string& url) const {
        std::vector<json::Value> out;
        for (const net::FakeRecord& r : fake.requests()) {
            if (r.request.url == url) out.push_back(parse(text(r.request.body)));
        }
        return out;
    }
};

} // namespace assistant_test
