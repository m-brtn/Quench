// What a game's own turns need (quench/assistant/Assistant.h): a turn the game starts (InputSource::Event) marked as
// not the player's words in the request and in the history, its per-turn round limit and cap of actions, cached
// tokens in the stats, the request's static part byte for byte the same from request to request (and the cache
// marker), the history by a budget of estimated tokens.

#include "Rig.h"
#include "Test.h"

#include <quench/assistant/Assistant.h>

#include <string>
#include <vector>

using namespace quench;
using namespace quench::assistant;
using namespace assistant_test;

namespace {

constexpr const char* kHeading = "Station event (not the player):";

struct Base {
    int statusCalls = 0;
    std::vector<std::string> orders;

    void add(Assistant& a) {
        Tool status;
        status.name = "get_status";
        status.description = "The base's power and rovers.";
        status.annotations.readOnly = true;
        status.handler = [this](const json::Value&) {
            ++statusCalls;
            return ToolResult::ok(parse(R"({"power_kw":12,"rovers":2})"));
        };
        (void)a.tools().add(std::move(status));
        Tool send;
        send.name = "send_rover";
        send.description = "Send a rover to a cell.";
        send.inputSchema = parse(R"({"type":"object","properties":{"rover":{"type":"string"},)"
                                 R"("cell":{"type":"string"}},"required":["rover","cell"]})");
        send.handler = [this](const json::Value& args) {
            orders.push_back(args["rover"].string + " -> " + args["cell"].string);
            return ToolResult::ok("ok");
        };
        (void)a.tools().add(std::move(send));
    }
};

// A whole turn: the first answer before ask(), each next one once the previous round's tool calls are under way.
bool turn(Rig& rig, std::string text, AskOptions options, const std::vector<std::string>& answers) {
    rig.fake.respond("POST", kChatUrl, stream(answers.front()));
    if (!rig.a.ask(std::move(text), options)) return false;
    for (std::size_t i = 1; i < answers.size(); ++i) {
        if (!rig.runUntil([&] { return rig.a.state() == State::CallingTool || !rig.a.busy(); })) return false;
        if (!rig.a.busy()) break;
        rig.fake.respond("POST", kChatUrl, stream(answers[i]));
        const std::size_t before = rig.fake.requests().size();
        if (!rig.runUntil([&] { return rig.fake.requests().size() > before || !rig.a.busy(); })) return false;
    }
    return rig.run() && rig.a.state() == State::Done;
}

std::string lastUserContent(const json::Value& body) {
    std::string out;
    for (const json::Value& m : body["messages"].array) {
        if (m["role"].stringOr("") == "user") out = m["content"].stringOr("");
    }
    return out;
}

std::vector<std::string> rawBodies(const Rig& rig) {
    std::vector<std::string> out;
    for (const net::FakeRecord& r : rig.fake.requests()) out.push_back(text(r.request.body));
    return out;
}

// A streamed text answer whose usage reports cached and written tokens.
std::string sseCached(const std::string& answer, int prompt, int cached, int written) {
    json::Value last = chunk(contentDelta(""), "stop", prompt, 5, 0.0001);
    json::Value details = json::Value::makeObject();
    details.object["cached_tokens"] = json::Value::makeNumber(cached);
    if (written >= 0) details.object["cache_write_tokens"] = json::Value::makeNumber(written);
    last.object["usage"].object["prompt_tokens_details"] = std::move(details);
    return event(chunk(contentDelta(answer))) + event(last) + "data: [DONE]\n\n";
}

} // namespace

QUENCH_TEST(event_turn_is_marked) {
    Rig rig;
    rig.a.setEventHeading(kHeading);
    rig.a.setStateSummary([] { return "power +12 kW"; });
    REQUIRE(turn(rig, "Rover 2 arrived at E7.", {.source = InputSource::Event}, {sseText({"Rover 2 is at E7."})}),
            "event turn: %s", rig.a.failure().message.c_str());
    std::vector<json::Value> bodies = rig.bodies(kChatUrl);
    REQUIRE(bodies.size() == 1, "%zu", bodies.size());
    const std::string sent = lastUserContent(bodies[0]);
    CHECK(sent == "Game state:\npower +12 kW\n\nStation event (not the player): Rover 2 arrived at E7.", "%s",
          sent.c_str());
    CHECK(sent.find("Player:") == std::string::npos, "no player heading in an event");
    // The history keeps the marking (not the state).
    const json::Value saved = rig.a.historyJson();
    REQUIRE(saved["turns"].array.size() == 1, "kept");
    CHECK(saved["turns"].array[0].array[0]["content"].stringOr("") ==
              "Station event (not the player): Rover 2 arrived at E7.",
          "%s", compact(saved).c_str());
    CHECK(rig.a.answer() == "Rover 2 is at E7." && rig.a.stats().rounds == 1, "answer and stats");

    // A player's turn afterwards sees the event in the history and is the player's.
    REQUIRE(turn(rig, "What happened?", {}, {sseText({"Rover 2 reached E7."})}), "player turn");
    bodies = rig.bodies(kChatUrl);
    const json::Value& m = bodies[1]["messages"];
    REQUIRE(m.array.size() == 4, "%s", compact(m).c_str());
    CHECK(m.array[1]["content"].stringOr("") == "Station event (not the player): Rover 2 arrived at E7." &&
              m.array[2]["content"].stringOr("") == "Rover 2 is at E7.",
          "%s", compact(m).c_str());
    CHECK(m.array[3]["content"].stringOr("") == "Game state:\npower +12 kW\n\nPlayer: What happened?", "%s",
          m.array[3]["content"].stringOr("").c_str());

    // Without a state summary, and with the default heading.
    Rig plain;
    REQUIRE(turn(plain, "The base can be upgraded.", {.source = InputSource::Event}, {sseText({"Upgrade ready."})}),
            "plain event");
    CHECK(lastUserContent(plain.bodies(kChatUrl)[0]) == "Game event (not the player): The base can be upgraded.", "%s",
          lastUserContent(plain.bodies(kChatUrl)[0]).c_str());
    // A player's turn is unchanged: the bare words.
    REQUIRE(turn(plain, "Thanks", {}, {sseText({"Welcome."})}), "plain player");
    CHECK(lastUserContent(plain.bodies(kChatUrl)[1]) == "Thanks", "%s",
          lastUserContent(plain.bodies(kChatUrl)[1]).c_str());
}

QUENCH_TEST(event_turn_limits) {
    // The round limit of this turn: 2 instead of 3, the second round asks for text.
    {
        Rig rig;
        Base base;
        base.add(rig.a);
        REQUIRE(turn(rig, "Rover 2 arrived.", {.source = InputSource::Event, .maxRounds = 2},
                     {sseTools({{"r1", "get_status", "{}"}}), sseText({"Noted."})}),
                "turn: %s", rig.a.failure().message.c_str());
        const std::vector<json::Value> bodies = rig.bodies(kChatUrl);
        REQUIRE(bodies.size() == 2, "%zu", bodies.size());
        CHECK(bodies[0]["tool_choice"].stringOr("") == "auto" && bodies[1]["tool_choice"].stringOr("") == "none", "%s",
              bodies[1]["tool_choice"].stringOr("").c_str());
        // Over it: the round limit's failure names this turn's limit.
        Rig over;
        Base b2;
        b2.add(over.a);
        over.fake.respond("POST", kChatUrl, stream(sseTools({{"x", "get_status", "{}"}})));
        REQUIRE(over.a.ask("Tick.", {.source = InputSource::Event, .maxRounds = 1}), "ask");
        REQUIRE(over.run(), "not finished");
        CHECK(over.a.failure().kind == ErrorKind::RoundLimit &&
                  over.a.failure().message.find("after 1 rounds") != std::string::npos && b2.statusCalls == 0,
              "%s", over.a.failure().message.c_str());
    }
    // The cap of actions: one call of a tool that is not read-only; read-only ones are not counted.
    {
        Rig rig;
        Base base;
        base.add(rig.a);
        REQUIRE(turn(rig, "Rover 2 is idle.", {.source = InputSource::Event, .maxActions = 1},
                     {sseTools({{"a", "get_status", "{}"},
                                {"b", "send_rover", R"({"rover":"Rover 2","cell":"E7"})"},
                                {"c", "send_rover", R"({"rover":"Rover 1","cell":"E9"})"}}),
                      sseTools({{"d", "send_rover", R"({"rover":"Rover 1","cell":"E9"})"}, {"e", "get_status", "{}"}}),
                      sseText({"Rover 2 is on its way."})}),
                "turn: %s", rig.a.failure().message.c_str());
        CHECK(base.orders.size() == 1 && base.orders[0] == "Rover 2 -> E7", "%zu orders", base.orders.size());
        CHECK(base.statusCalls == 2, "read-only calls are not capped: %d", base.statusCalls);
        const std::vector<ToolCallRecord>& calls = rig.a.toolCalls();
        REQUIRE(calls.size() == 5, "%zu", calls.size());
        CHECK(!calls[1].result.isError && calls[2].result.isError && calls[3].result.isError &&
                  !calls[4].result.isError,
              "which were refused");
        CHECK(calls[2].result.text.find("at most 1 change to the game") != std::string::npos, "%s",
              calls[2].result.text.c_str());
        const Stats& s = rig.a.stats();
        CHECK(s.toolCalls == 5 && s.refusedCalls == 2, "%d %d", s.toolCalls, s.refusedCalls);
        // The model reads the reason.
        const json::Value second = rig.bodies(kChatUrl)[1]["messages"];
        bool told = false;
        for (const json::Value& m : second.array) {
            if (m["tool_call_id"].stringOr("") == "c")
                told = m["content"].stringOr("").starts_with("Error: not run: this turn may make at most 1 change");
        }
        CHECK(told, "%s", compact(second).c_str());
        // A player's turn afterwards has no cap.
        REQUIRE(turn(rig, "Send rover one to E9", {},
                     {sseTools({{"f", "send_rover", R"({"rover":"Rover 1","cell":"E9"})"},
                                {"g", "send_rover", R"({"rover":"Rover 2","cell":"E1"})"}}),
                      sseText({"Both sent."})}),
                "player turn");
        CHECK(base.orders.size() == 3 && rig.a.stats().refusedCalls == 0 && rig.a.totals().refusedCalls == 2, "%zu",
              base.orders.size());
        // A cap of 0: no action at all.
        REQUIRE(turn(rig, "Night falls.", {.source = InputSource::Event, .maxActions = 0},
                     {sseTools({{"h", "send_rover", R"({"rover":"Rover 1","cell":"E2"})"}}), sseText({"Holding."})}),
                "cap 0");
        CHECK(base.orders.size() == 3 && rig.a.stats().refusedCalls == 1, "%zu", base.orders.size());
    }
}

QUENCH_TEST(event_turn_cancel_and_events) {
    Rig rig;
    Base base;
    base.add(rig.a);
    rig.fake.respond("POST", kChatUrl, stream(sseTools({{"a", "send_rover", R"({"rover":"Rover 1","cell":"E2"})"}})));
    REQUIRE(rig.a.ask("Dust storm.", {.source = InputSource::Event}), "ask");
    CHECK(rig.a.state() == State::Thinking, "%s", stateName(rig.a.state()));
    CHECK(!rig.a.ask("Hello"), "one operation at a time, events included");
    REQUIRE(rig.runUntil([&] { return rig.a.state() == State::CallingTool; }), "call");
    rig.a.cancel();
    rig.step();
    CHECK(rig.a.state() == State::Idle && rig.a.historyTurns() == 0 && base.orders.empty(), "cancelled");
    bool started = false;
    bool cancelled = false;
    for (const Event& e : rig.a.takeEvents()) {
        started = started || e.kind == EventKind::ToolStarted;
        cancelled = cancelled || e.kind == EventKind::Cancelled;
    }
    CHECK(started && cancelled, "the same events as a player's turn");
}

QUENCH_TEST(turn_cached_tokens) {
    Rig rig;
    LogCapture logs;
    REQUIRE(turn(rig, "One", {}, {sseCached("First.", 1200, 1024, 0)}), "turn 1");
    CHECK(rig.a.stats().usage.cachedTokens == 1024 && rig.a.stats().usage.cacheWriteTokens == 0, "%lld",
          (long long)rig.a.stats().usage.cachedTokens);
    REQUIRE(turn(rig, "Two", {}, {sseCached("Second.", 1300, 1024, 256)}), "turn 2");
    const Stats& s = rig.a.stats();
    CHECK(s.usage.promptTokens == 1300 && s.usage.cachedTokens == 1024 && s.usage.cacheWriteTokens == 256, "%lld %lld",
          (long long)s.usage.cachedTokens, (long long)s.usage.cacheWriteTokens);
    CHECK(rig.a.totals().usage.cachedTokens == 2048 && rig.a.totals().usage.cacheWriteTokens == 256, "totals");
    const std::string all = logs.all();
    CHECK(all.find("1200+5 tokens (1024 cached)") != std::string::npos, "%s", all.c_str());
    CHECK(all.find("1300+5 tokens (1024 cached, 256 written)") != std::string::npos, "%s", all.c_str());
    // Without the details: 0, and the round still reads.
    REQUIRE(turn(rig, "Three", {}, {sseText({"Third."})}), "turn 3");
    CHECK(rig.a.stats().usage.cachedTokens == 0, "no details");
    // Summed over the rounds of a turn.
    Rig two;
    Base base;
    base.add(two.a);
    std::string round1 = sseTools({{"a", "get_status", "{}"}});
    round1.insert(round1.rfind("\"usage\":{") + 9, R"("prompt_tokens_details":{"cached_tokens":600},)");
    REQUIRE(turn(two, "Status", {}, {round1, sseCached("Fine.", 900, 700, -1)}), "two rounds");
    CHECK(two.a.stats().usage.cachedTokens == 1300 && two.a.stats().rounds == 2, "%lld",
          (long long)two.a.stats().usage.cachedTokens);
}

QUENCH_TEST(turn_static_prefix_is_byte_stable) {
    Rig rig;
    Base base;
    base.add(rig.a);
    int tick = 0;
    rig.a.setStateSummary([&] { return "tick " + std::to_string(++tick); });
    REQUIRE(turn(rig, "Status?", {}, {sseTools({{"a", "get_status", "{}"}}), sseText({"Fine."})}), "turn 1");
    REQUIRE(turn(rig, "Rover 2 arrived.", {.source = InputSource::Event}, {sseText({"Noted."})}), "turn 2");
    REQUIRE(turn(rig, "And now?", {}, {sseText({"Still fine."})}), "turn 3");
    const std::vector<std::string> raw = rawBodies(rig);
    REQUIRE(raw.size() == 4, "%zu", raw.size());
    // The static part: up to the end of the system message, after the model, the settings and the tools.
    const std::string system = R"("messages":[{"content":"You are the station's assistant.","role":"system"})";
    const std::size_t end = raw[0].find(system);
    REQUIRE(end != std::string::npos, "%s", raw[0].c_str());
    const std::string prefix = raw[0].substr(0, end + system.size());
    for (const std::string& body : raw) CHECK(body.starts_with(prefix), "the static part differs: %s", body.c_str());
    CHECK(prefix.starts_with(R"({"model":"anthropic/claude-haiku-5.5",)") &&
              prefix.find(R"("tools":[)") != std::string::npos && prefix.find("tick") == std::string::npos,
          "%s", prefix.c_str());
    CHECK(raw[0].ends_with(R"(],"tool_choice":"auto"})"), "tool_choice last: %s", raw[0].c_str());
    // Within a turn, a round's body starts with the whole previous round's messages.
    const std::string firstMessages = raw[0].substr(0, raw[0].rfind("],"
                                                                    R"("tool_choice")"));
    CHECK(raw[1].starts_with(firstMessages), "round 2 extends round 1");

    // The cache marker: the system prompt as one text part with cache_control; off by default.
    Rig marked;
    marked.a.settings().cacheMarker = true;
    REQUIRE(turn(marked, "Hi", {}, {sseText({"Hello."})}), "marked");
    const json::Value sys = marked.bodies(kChatUrl)[0]["messages"].array[0];
    CHECK(compact(sys) ==
              R"({"content":[{"cache_control":{"type":"ephemeral"},"text":"You are the station's assistant.",)"
              R"("type":"text"}],"role":"system"})",
          "%s", compact(sys).c_str());
    CHECK(settingsFromJson(settingsToJson(marked.a.settings()), openRouterPreset()).cacheMarker &&
              !openRouterPreset().cacheMarker,
          "saved with the settings");
}

QUENCH_TEST(turn_history_budget) {
    Rig rig;
    rig.a.limits().historyTokens = 120;
    rig.a.limits().historyToolResultChars = 50;
    Tool big;
    big.name = "read_log";
    big.description = "The base's log.";
    big.annotations.readOnly = true;
    big.handler = [](const json::Value&) { return ToolResult::ok(std::string(5000, 'L')); };
    REQUIRE(rig.a.tools().add(std::move(big)), "add");

    // Turns of a known size: "Turn N" and a 60-character answer, about 2 * 4 + 2 + 20 = 30 tokens each.
    const std::string answer(60, 'a');
    for (int i = 1; i <= 8; ++i) {
        REQUIRE(turn(rig, "Turn " + std::to_string(i), {}, {sseText({answer})}), "turn %d", i);
        CHECK(rig.a.historyTokenEstimate() <= 120, "over the budget after turn %d: %lld", i,
              (long long)rig.a.historyTokenEstimate());
    }
    // The newest ones, oldest first, in their order.
    json::Value saved = rig.a.historyJson();
    const std::size_t kept = saved["turns"].array.size();
    CHECK(kept == 4, "%zu turns kept", kept);
    for (std::size_t i = 0; i < kept; ++i) {
        const std::string want = "Turn " + std::to_string(8 - kept + 1 + i);
        CHECK(saved["turns"].array[i].array[0]["content"].stringOr("") == want, "%zu: %s", i,
              saved["turns"].array[i].array[0]["content"].stringOr("").c_str());
    }

    // A turn bigger than the budget is kept whole while it is the newest: its tool result as sent in the turn.
    REQUIRE(turn(rig, "Read the log", {}, {sseTools({{"b", "read_log", "{}"}}), sseText({"Long."})}), "big turn");
    CHECK(rig.a.historyTurns() == 1, "only the newest: %zu", rig.a.historyTurns());
    saved = rig.a.historyJson();
    const std::string asSent = rig.bodies(kChatUrl).back()["messages"].array.back()["content"].stringOr("");
    std::string inHistory;
    for (const json::Value& m : saved["turns"].array[0].array) {
        if (m["role"].stringOr("") == "tool") inHistory = m["content"].stringOr("");
    }
    CHECK(inHistory == asSent && inHistory.size() > 4000, "nothing cut in the newest turn: %zu %zu", inHistory.size(),
          asSent.size());
    // Once older, its result is cut, and the count of what is left out is still the whole result's.
    rig.a.limits().historyTokens = 10000;
    REQUIRE(turn(rig, "Next", {}, {sseText({"Ok."})}), "next");
    saved = rig.a.historyJson();
    REQUIRE(saved["turns"].array.size() == 2, "%zu", saved["turns"].array.size());
    for (const json::Value& m : saved["turns"].array[0].array) {
        if (m["role"].stringOr("") == "tool") inHistory = m["content"].stringOr("");
    }
    CHECK(inHistory == std::string(50, 'L') + "... (4950 more characters)", "%s", inHistory.c_str());
    CHECK(saved["turns"].array[1].array[0]["content"].stringOr("") == "Next", "the order");

    // Old event turns: only their marked first line and their answer.
    Rig ev;
    Base base;
    base.add(ev.a);
    ev.a.limits().historyEventTurns = 1;
    REQUIRE(turn(ev, "Rover 2 arrived at E7.\nCargo: 3 cells.", {.source = InputSource::Event},
                 {sseTools({{"s", "get_status", "{}"}}), sseText({"Rover 2 is unloading."})}),
            "event");
    CHECK(ev.a.historyJson()["turns"].array[0].array.size() == 4, "the newest event turn whole");
    REQUIRE(turn(ev, "Status?", {}, {sseText({"All good."})}), "player");
    saved = ev.a.historyJson();
    const json::Value& old = saved["turns"].array[0];
    REQUIRE(old.array.size() == 2, "%s", compact(old).c_str());
    CHECK(old.array[0]["content"].stringOr("") == "Game event (not the player): Rover 2 arrived at E7." &&
              old.array[1]["content"].stringOr("") == "Rover 2 is unloading.",
          "%s", compact(old).c_str());
    // A player's turn as old is not compacted.
    REQUIRE(turn(ev, "Send nothing", {}, {sseTools({{"t", "get_status", "{}"}}), sseText({"Nothing sent."})}),
            "player 2");
    REQUIRE(turn(ev, "Bye", {}, {sseText({"Bye."})}), "player 3");
    saved = ev.a.historyJson();
    CHECK(saved["turns"].array.size() == 4 && saved["turns"].array[2].array.size() == 4, "%s", compact(saved).c_str());

    // A saved history from before the budget (six plain turns, 0.18.0's shape) loads, in order, within the budget.
    json::Value turns = json::Value::makeArray();
    for (int i = 1; i <= 6; ++i) {
        turns.array.push_back(parse(R"([{"role":"user","content":"Old )" + std::to_string(i) +
                                    R"("},{"role":"assistant","content":")" + std::string(60, 'o') + R"("}])"));
    }
    json::Value old18 = json::Value::makeObject();
    old18.object["turns"] = std::move(turns);
    Rig restored;
    restored.a.limits().historyTokens = 120;
    REQUIRE(restored.a.setHistory(old18), "setHistory");
    saved = restored.a.historyJson();
    CHECK(saved["turns"].array.size() == 4 && saved["turns"].array[0].array[0]["content"].stringOr("") == "Old 3" &&
              saved["turns"].array[3].array[0]["content"].stringOr("") == "Old 6",
          "%s", compact(saved).c_str());
    // historyTurns still caps by count.
    Rig counted;
    counted.a.limits().historyTurns = 2;
    REQUIRE(counted.a.setHistory(old18), "setHistory");
    CHECK(counted.a.historyTurns() == 2, "%zu", counted.a.historyTurns());
    CHECK(estimateTokens("abcdef") == 2 && estimateTokens("abcdefg") == 3 && estimateTokens("") == 0, "estimate");
}
