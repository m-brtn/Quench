// The assistant's turn on a fake network (quench/assistant/Assistant.h): one tool, parallel tools, several rounds,
// the round limit, a handler's error, arguments that fail the schema, cancel at each stage, one operation at a time,
// HTTP failures mapped to errors, the key kept out of logs, errors and saves, the history and its trimming.

#include "Rig.h"
#include "Test.h"

#include <quench/assistant/Assistant.h>

#include <algorithm>
#include <memory>
#include <string>

using namespace quench;
using namespace quench::assistant;
using namespace assistant_test;

namespace {

// Two tools a colony game might give: a read-only status and an order.
struct Colony {
    int statusCalls = 0;
    std::vector<std::string> orders;

    void add(Assistant& a) {
        Tool status;
        status.name = "get_status";
        status.description = "The colony's power balance and stock.";
        status.annotations.readOnly = true;
        status.handler = [this](const json::Value&) {
            ++statusCalls;
            return ToolResult::ok(parse(R"({"power_kw":12,"cells":3})"));
        };
        (void)a.tools().add(std::move(status));
        Tool collect;
        collect.name = "order_collect";
        collect.description = "Send a walker to collect an item.";
        collect.inputSchema = parse(R"({"type":"object","properties":{"unit":{"type":"string"},)"
                                    R"("item":{"type":"string"}},"required":["unit","item"]})");
        collect.handler = [this](const json::Value& args) {
            if (args["unit"].string == "Walker 9") return ToolResult::error("there is no Walker 9; walkers: 1, 2");
            orders.push_back(args["unit"].string + " -> " + args["item"].string);
            return ToolResult::ok("ok: " + args["unit"].string + " is on its way to " + args["item"].string);
        };
        (void)a.tools().add(std::move(collect));
    }
};

// The messages of the chat request number `i` (0-based).
json::Value messagesOf(const std::vector<json::Value>& bodies, std::size_t i) {
    return i < bodies.size() ? bodies[i]["messages"] : json::Value();
}

const json::Value* lastWithRole(const json::Value& messages, const char* role) {
    const json::Value* found = nullptr;
    for (const json::Value& m : messages.array) {
        if (m["role"].stringOr("") == role) found = &m;
    }
    return found;
}

int count(const std::vector<Event>& events, EventKind kind) {
    return int(std::count_if(events.begin(), events.end(), [&](const Event& e) { return e.kind == kind; }));
}

} // namespace

QUENCH_TEST(turn_one_tool) {
    Rig rig;
    Colony colony;
    colony.add(rig.a);
    rig.fake.respond("POST", kChatUrl, stream(sseTools({{"call_1", "get_status", "{}"}})));
    REQUIRE(rig.a.ask("What is my power balance?"), "ask");
    CHECK(rig.a.state() == State::Thinking, "%s", stateName(rig.a.state()));
    REQUIRE(rig.runUntil([&] { return rig.a.state() == State::CallingTool; }), "no tool call");
    CHECK(rig.a.toolName() == "get_status" && colony.statusCalls == 0, "%s, %d", rig.a.toolName().c_str(),
          colony.statusCalls);
    // The next answer, for the second round.
    rig.fake.respond("POST", kChatUrl, stream(sseText({"Power is ", "+12 kW."})));
    REQUIRE(rig.run(), "not finished");
    CHECK(rig.a.state() == State::Done, "%s: %s", stateName(rig.a.state()), rig.a.failure().message.c_str());
    CHECK(rig.a.answer() == "Power is +12 kW.", "'%s'", rig.a.answer().c_str());
    CHECK(colony.statusCalls == 1, "%d", colony.statusCalls);

    const std::vector<json::Value> bodies = rig.bodies(kChatUrl);
    REQUIRE(bodies.size() == 2, "%zu chat requests", bodies.size());
    CHECK(bodies[0]["model"].stringOr("") == "anthropic/claude-haiku-5.5" && bodies[0]["stream"].boolOr(false),
          "model, stream");
    CHECK(bodies[0]["tool_choice"].stringOr("") == "auto" && bodies[0]["tools"].array.size() == 2, "tools");
    CHECK(bodies[1]["tools"].array.size() == 2, "the tools are sent again");
    CHECK(messagesOf(bodies, 0).array.front()["role"].stringOr("") == "system", "the system prompt first");
    const json::Value& second = messagesOf(bodies, 1);
    const json::Value* call = lastWithRole(second, "assistant");
    REQUIRE(call && (*call)["tool_calls"].array.size() == 1, "the assistant's tool call is sent back");
    CHECK((*call)["tool_calls"].array[0]["id"].stringOr("") == "call_1" &&
              (*call)["tool_calls"].array[0]["function"]["arguments"].stringOr("") == "{}",
          "%s", compact(*call).c_str());
    const json::Value* result = lastWithRole(second, "tool");
    REQUIRE(result, "no tool message");
    CHECK((*result)["tool_call_id"].stringOr("") == "call_1" &&
              (*result)["content"].stringOr("") == R"({"cells":3,"power_kw":12})",
          "%s", compact(*result).c_str());
    const net::FakeRecord* last = rig.fake.last();
    REQUIRE(last, "no request");
    const std::string* auth = net::findHeader(last->request.headers, "Authorization");
    CHECK(auth && *auth == std::string("Bearer ") + kKey, "Authorization");

    const Stats& s = rig.a.stats();
    CHECK(s.requests == 2 && s.rounds == 2 && s.toolCalls == 1, "%d %d %d", s.requests, s.rounds, s.toolCalls);
    CHECK(s.usage.promptTokens == 200 && s.usage.completionTokens == 30 && s.usage.costKnown, "usage");
    CHECK(s.firstTextTime > 0.0 && s.totalTime >= s.firstTextTime, "%f %f", s.firstTextTime, s.totalTime);
    CHECK(rig.a.historyTurns() == 1, "%zu", rig.a.historyTurns());
    const std::vector<Event> events = rig.a.takeEvents();
    CHECK(count(events, EventKind::ToolStarted) == 1 && count(events, EventKind::ToolFinished) == 1 &&
              count(events, EventKind::Answered) == 1,
          "events");
}

QUENCH_TEST(turn_two_parallel_tools) {
    Rig rig;
    Colony colony;
    colony.add(rig.a);
    rig.fake.respond("POST", kChatUrl,
                     stream(sseTools({{"c1", "order_collect", R"({"unit":"Walker 1","item":"E7"})"},
                                      {"c2", "order_collect", R"({"unit":"Walker 2","item":"E9"})"}})));
    REQUIRE(rig.a.ask("Send walkers one and two to the nearest cells"), "ask");
    REQUIRE(rig.runUntil([&] { return rig.a.state() == State::CallingTool; }), "no tool call");
    // Each call is visible for an update before it runs.
    CHECK(colony.orders.empty() && rig.a.toolCalls().size() == 2, "%zu orders", colony.orders.size());
    rig.step();
    CHECK(colony.orders.size() == 1 && rig.a.state() == State::CallingTool, "%zu after one update",
          colony.orders.size());
    rig.fake.respond("POST", kChatUrl, stream(sseText({"Both walkers are on their way."})));
    REQUIRE(rig.run(), "not finished");
    CHECK(rig.a.state() == State::Done, "%s", rig.a.failure().message.c_str());
    REQUIRE(colony.orders.size() == 2, "%zu", colony.orders.size());
    CHECK(colony.orders[0] == "Walker 1 -> E7" && colony.orders[1] == "Walker 2 -> E9", "in order");
    const json::Value& second = messagesOf(rig.bodies(kChatUrl), 1);
    int tools = 0;
    for (const json::Value& m : second.array) tools += m["role"].stringOr("") == "tool" ? 1 : 0;
    CHECK(tools == 2, "%d tool messages", tools);
    CHECK(second.array[second.array.size() - 2]["tool_call_id"].stringOr("") == "c1" &&
              second.array.back()["tool_call_id"].stringOr("") == "c2",
          "the results in the calls' order");
    const std::vector<Event> events = rig.a.takeEvents();
    std::vector<std::size_t> order;
    for (const Event& e : events) {
        if (e.kind == EventKind::ToolStarted || e.kind == EventKind::ToolFinished) order.push_back(e.tool);
    }
    CHECK(order == (std::vector<std::size_t>{0, 0, 1, 1}), "start and end of each call in turn");
}

QUENCH_TEST(turn_two_rounds) {
    // First the read-only tool, then the order, then the answer: three requests, the last one asking for text.
    Rig rig;
    Colony colony;
    colony.add(rig.a);
    rig.fake.respond("POST", kChatUrl, stream(sseTools({{"r1", "get_status", ""}})));
    REQUIRE(rig.a.ask("Collect the nearest cell if power is low"), "ask");
    // Each next answer is put in place while a call waits: the update that runs the last call sends the next round.
    REQUIRE(rig.runUntil([&] { return rig.a.state() == State::CallingTool; }), "round 1");
    rig.fake.respond("POST", kChatUrl,
                     stream(sseTools({{"r2", "order_collect", R"({"unit":"Walker 1","item":"E7"})"}})));
    REQUIRE(rig.runUntil([&] { return rig.a.state() == State::CallingTool && rig.a.toolCalls().size() == 2; }),
            "round 2");
    CHECK(colony.statusCalls == 1 && colony.orders.empty(), "round 1's call ran, round 2's waits");
    rig.fake.respond("POST", kChatUrl, stream(sseText({"Walker 1 is collecting E7."})));
    REQUIRE(rig.run(), "not finished");
    CHECK(rig.a.state() == State::Done && rig.a.answer() == "Walker 1 is collecting E7.", "%s",
          rig.a.failure().message.c_str());
    const std::vector<json::Value> bodies = rig.bodies(kChatUrl);
    REQUIRE(bodies.size() == 3, "%zu", bodies.size());
    CHECK(bodies[1]["tool_choice"].stringOr("") == "auto" && bodies[2]["tool_choice"].stringOr("") == "none", "%s %s",
          bodies[1]["tool_choice"].stringOr("").c_str(), bodies[2]["tool_choice"].stringOr("").c_str());
    CHECK(rig.a.toolCalls().size() == 2 && rig.a.toolCalls()[1].round == 2, "records");
    CHECK(rig.a.toolCalls()[0].arguments == "{}", "empty arguments become {}");
}

QUENCH_TEST(turn_round_limit) {
    Rig rig;
    Colony colony;
    colony.add(rig.a);
    rig.a.limits().maxRounds = 2;
    rig.fake.respond("POST", kChatUrl, stream(sseTools({{"x", "get_status", "{}"}})));
    REQUIRE(rig.a.ask("Loop forever"), "ask");
    REQUIRE(rig.run(), "not finished");
    CHECK(rig.a.state() == State::Failed && rig.a.failure().kind == ErrorKind::RoundLimit, "%s %s",
          stateName(rig.a.state()), errorKindName(rig.a.failure().kind));
    CHECK(colony.statusCalls == 1, "the last round's calls are not executed: %d", colony.statusCalls);
    const std::vector<json::Value> bodies = rig.bodies(kChatUrl);
    CHECK(bodies.size() == 2 && bodies[1]["tool_choice"].stringOr("") == "none", "%zu", bodies.size());
    CHECK(rig.a.historyTurns() == 0, "a failed turn is not kept");

    // With text beside the calls on the last round, the text is the answer.
    Rig other;
    Colony c2;
    c2.add(other.a);
    other.a.limits().maxRounds = 1;
    std::string body = event(chunk(contentDelta("I cannot check that now.")));
    body += sseTools({{"y", "get_status", "{}"}});
    other.fake.respond("POST", kChatUrl, stream(body));
    REQUIRE(other.a.ask("Status?"), "ask");
    REQUIRE(other.run(), "not finished");
    CHECK(other.a.state() == State::Done && other.a.answer() == "I cannot check that now." && c2.statusCalls == 0, "%s",
          other.a.failure().message.c_str());
}

QUENCH_TEST(turn_handler_error_goes_to_the_model) {
    Rig rig;
    Colony colony;
    colony.add(rig.a);
    rig.fake.respond("POST", kChatUrl,
                     stream(sseTools({{"e1", "order_collect", R"({"unit":"Walker 9","item":"E7"})"}})));
    REQUIRE(rig.a.ask("Send walker nine"), "ask");
    REQUIRE(rig.runUntil([&] { return rig.a.state() == State::CallingTool; }), "call");
    rig.fake.respond("POST", kChatUrl, stream(sseText({"There is no Walker 9."})));
    REQUIRE(rig.run(), "not finished");
    CHECK(rig.a.state() == State::Done, "%s", rig.a.failure().message.c_str());
    CHECK(rig.a.toolCalls()[0].result.isError, "the record is an error");
    const json::Value second = messagesOf(rig.bodies(kChatUrl), 1);
    const json::Value* tool = lastWithRole(second, "tool");
    REQUIRE(tool, "no tool message");
    CHECK((*tool)["content"].stringOr("") == "Error: there is no Walker 9; walkers: 1, 2", "%s",
          (*tool)["content"].stringOr("").c_str());
}

QUENCH_TEST(turn_arguments_fail_the_schema) {
    Rig rig;
    Colony colony;
    colony.add(rig.a);
    rig.fake.respond("POST", kChatUrl,
                     stream(sseTools({{"s1", "order_collect", R"({"unit":7})"},
                                      {"s2", "fly_away", "{}"},
                                      {"s3", "order_collect", R"({"unit":"Walker 1",)"}})));
    REQUIRE(rig.a.ask("Do odd things"), "ask");
    REQUIRE(rig.runUntil([&] { return rig.a.state() == State::CallingTool; }), "calls");
    rig.fake.respond("POST", kChatUrl, stream(sseText({"Sorry."})));
    REQUIRE(rig.runUntil([&] { return rig.a.toolCalls()[2].finished; }), "calls");
    CHECK(colony.orders.empty(), "a bad call ran the handler");
    const std::vector<ToolCallRecord>& calls = rig.a.toolCalls();
    CHECK(calls[0].result.isError &&
              calls[0].result.text.find("invalid arguments for order_collect") != std::string::npos,
          "%s", calls[0].result.text.c_str());
    CHECK(calls[1].result.isError && calls[1].result.text.find("unknown tool 'fly_away'") != std::string::npos, "%s",
          calls[1].result.text.c_str());
    CHECK(calls[2].result.isError && calls[2].result.text.find("not valid JSON") != std::string::npos, "%s",
          calls[2].result.text.c_str());
    REQUIRE(rig.run(), "not finished");
    CHECK(rig.a.state() == State::Done, "the turn goes on: %s", rig.a.failure().message.c_str());
}

QUENCH_TEST(turn_state_summary_and_history) {
    Rig rig;
    int summaries = 0;
    rig.a.setStateSummary([&] { return "power +" + std::to_string(++summaries) + " kW"; });
    rig.fake.respond("POST", kChatUrl, stream(sseText({"First."})));
    REQUIRE(rig.a.ask("One"), "ask");
    REQUIRE(rig.run(), "turn 1");
    rig.fake.respond("POST", kChatUrl, stream(sseText({"Second."})));
    REQUIRE(rig.a.ask("Two"), "ask");
    REQUIRE(rig.run(), "turn 2");
    CHECK(summaries == 2, "once per turn: %d", summaries);
    const json::Value& m = messagesOf(rig.bodies(kChatUrl), 1);
    // system, the first turn (user, assistant), this turn's user message with the state.
    REQUIRE(m.array.size() == 4, "%s", compact(m).c_str());
    CHECK(m.array[1]["content"].stringOr("") == "One" && m.array[2]["content"].stringOr("") == "First.",
          "the history without the state: %s", compact(m).c_str());
    CHECK(m.array[3]["content"].stringOr("") == "Game state:\npower +2 kW\n\nPlayer: Two", "%s",
          m.array[3]["content"].stringOr("").c_str());
}

QUENCH_TEST(turn_history_trimming) {
    Rig rig;
    rig.a.limits().historyTurns = 2;
    rig.a.limits().toolResultChars = 1000;
    rig.a.limits().historyToolResultChars = 50;
    Tool big;
    big.name = "read_log";
    big.description = "The colony's log.";
    big.handler = [](const json::Value&) { return ToolResult::ok(std::string(5000, 'L')); };
    REQUIRE(rig.a.tools().add(std::move(big)), "add");

    rig.fake.respond("POST", kChatUrl, stream(sseTools({{"b1", "read_log", "{}"}})));
    REQUIRE(rig.a.ask("Read the log"), "ask");
    REQUIRE(rig.runUntil([&] { return rig.a.state() == State::CallingTool; }), "tool");
    rig.fake.respond("POST", kChatUrl, stream(sseText({"The log is long."})));
    REQUIRE(rig.run(), "turn 1");
    const json::Value first = messagesOf(rig.bodies(kChatUrl), 1);
    const json::Value* sent = lastWithRole(first, "tool");
    REQUIRE(sent, "no tool message");
    CHECK(sent->operator[]("content").string.size() < 1100 &&
              sent->operator[]("content").string.find("(4000 more characters)") != std::string::npos,
          "the result as sent: %zu", sent->operator[]("content").string.size());

    for (const char* t : {"Two", "Three"}) {
        rig.fake.respond("POST", kChatUrl, stream(sseText({"Ok."})));
        REQUIRE(rig.a.ask(t), "ask");
        REQUIRE(rig.run(), "turn");
    }
    // The third turn's request still had the first turn, with its tool result trimmed for the history.
    const json::Value& third = messagesOf(rig.bodies(kChatUrl), 3);
    const json::Value* old = lastWithRole(third, "tool");
    REQUIRE(old, "no old tool message in %s", compact(third).c_str());
    CHECK((*old)["content"].string.size() < 100 &&
              (*old)["content"].string.find("(4950 more characters)") != std::string::npos,
          "%s", (*old)["content"].string.c_str());
    CHECK(rig.a.historyTurns() == 2, "%zu", rig.a.historyTurns());
    const json::Value saved = rig.a.historyJson();
    REQUIRE(saved["turns"].array.size() == 2, "saved");
    CHECK(saved["turns"].array[0].array[0]["content"].stringOr("") == "Two", "the oldest turns go first");

    Rig restored;
    REQUIRE(restored.a.setHistory(saved), "setHistory");
    CHECK(restored.a.historyTurns() == 2, "%zu", restored.a.historyTurns());
    CHECK(!restored.a.setHistory(parse(R"({"turns":[[{"role":"system","content":"x"}]]})")), "a bad role");
    CHECK(!restored.a.setHistory(parse(R"({"other":1})")), "no turns");
}

QUENCH_TEST(turn_one_at_a_time) {
    Rig rig;
    rig.fake.respond("POST", kChatUrl, net::FakeResponse::silence());
    REQUIRE(rig.a.ask("First"), "ask");
    const Result<void> second = rig.a.ask("Second");
    CHECK(!second && second.error().find("busy") != std::string::npos, "%s", second.error().c_str());
    CHECK(!rig.a.speak("hello") && !rig.a.transcribe(wav(0.5)), "every operation is refused");
    CHECK(rig.a.state() == State::Thinking && rig.fake.requests().size() == 1, "the first one goes on");
    CHECK(!rig.a.ask(""), "an empty text is refused");
}

QUENCH_TEST(turn_cancel_at_each_stage) {
    // Transcribing.
    {
        Rig rig;
        rig.fake.respond("POST", kSttUrl, net::FakeResponse::silence());
        REQUIRE(rig.a.askByVoice(wav(1.0)), "voice");
        rig.step();
        CHECK(rig.a.state() == State::Transcribing, "%s", stateName(rig.a.state()));
        rig.a.cancel();
        rig.step();
        CHECK(rig.a.state() == State::Idle && rig.fake.last()->cancelled && rig.fake.open() == 0, "transcribing");
        CHECK(rig.a.failure().kind == ErrorKind::Cancelled, "the reason");
    }
    // Thinking, before anything arrived and in the middle of the stream.
    for (int midStream = 0; midStream < 2; ++midStream) {
        Rig rig;
        net::FakeResponse slow = stream(sseText({"Part one, ", "part two, ", "part three."}), 40, 0.2);
        rig.fake.respond("POST", kChatUrl, midStream ? slow : net::FakeResponse::silence());
        REQUIRE(rig.a.ask("Talk"), "ask");
        if (midStream) REQUIRE(rig.runUntil([&] { return !rig.a.partialText().empty(); }), "no text");
        rig.a.cancel();
        for (int i = 0; i < 50; ++i) rig.step(0.1);
        CHECK(rig.a.state() == State::Idle && rig.a.answer().empty() && rig.a.historyTurns() == 0, "thinking %d",
              midStream);
        CHECK(rig.fake.open() == 0 && rig.fake.last()->cancelled, "the request is cancelled");
    }
    // Calling a tool: between the call's request and its result.
    {
        Rig rig;
        Colony colony;
        colony.add(rig.a);
        rig.fake.respond("POST", kChatUrl,
                         stream(sseTools({{"c", "order_collect",
                                           R"({"unit":"Walker 1",)"
                                           R"("item":"E7"})"}})));
        REQUIRE(rig.a.ask("Collect"), "ask");
        REQUIRE(rig.runUntil([&] { return rig.a.state() == State::CallingTool; }), "no call");
        rig.a.cancel();
        for (int i = 0; i < 10; ++i) rig.step();
        CHECK(colony.orders.empty() && rig.a.state() == State::Idle, "the call did not run");
        CHECK(rig.fake.requests().size() == 1, "no second round");
        // A new turn works afterwards.
        rig.fake.respond("POST", kChatUrl, stream(sseText({"Fine."})));
        REQUIRE(rig.a.ask("Again"), "ask after cancel");
        REQUIRE(rig.run(), "not finished");
        CHECK(rig.a.state() == State::Done && rig.a.answer() == "Fine.", "%s", rig.a.failure().message.c_str());
        // The cancelled turn left nothing in the history: system, user only.
        CHECK(messagesOf(rig.bodies(kChatUrl), 1).array.size() == 2, "%s",
              compact(messagesOf(rig.bodies(kChatUrl), 1)).c_str());
    }
    // Speaking.
    {
        Rig rig;
        rig.fake.respond("POST", kChatUrl, stream(sseText({"Hello."})));
        rig.fake.respond("POST", kTtsUrl, net::FakeResponse::silence());
        REQUIRE(rig.a.ask("Hi", {.speak = true}), "ask");
        REQUIRE(rig.runUntil([&] { return rig.a.state() == State::Speaking; }), "not speaking");
        CHECK(rig.a.answer() == "Hello.", "the answer is ready while speaking");
        rig.a.cancel();
        rig.step();
        CHECK(rig.a.state() == State::Idle && !rig.a.takeSpeech(), "speaking");
        CHECK(rig.a.historyTurns() == 1, "the answered turn stays in the history");
        const std::vector<Event> events = rig.a.takeEvents();
        CHECK(count(events, EventKind::Cancelled) == 1, "a Cancelled event");
    }
    // A cancel when idle does nothing.
    Rig idle;
    idle.a.cancel();
    CHECK(idle.a.state() == State::Idle && !idle.a.failure().failed(), "idle");
}

QUENCH_TEST(turn_cancel_from_a_handler) {
    Rig rig;
    Tool stop;
    stop.name = "stop";
    stop.description = "Stops the assistant.";
    stop.handler = [&](const json::Value&) {
        rig.a.cancel();
        return ToolResult::ok("stopped");
    };
    REQUIRE(rig.a.tools().add(std::move(stop)), "add");
    rig.fake.respond("POST", kChatUrl, stream(sseTools({{"a", "stop", "{}"}, {"b", "stop", "{}"}})));
    REQUIRE(rig.a.ask("Stop"), "ask");
    for (int i = 0; i < 100; ++i) rig.step();
    CHECK(rig.a.state() == State::Idle && rig.fake.requests().size() == 1, "%s, %zu requests", stateName(rig.a.state()),
          rig.fake.requests().size());
}

QUENCH_TEST(turn_http_failures) {
    struct Case {
        int status;
        ErrorKind kind;
        const char* body;
        const char* says;
    };
    const Case cases[] = {
        {401, ErrorKind::BadKey, R"({"error":{"message":"No auth credentials found","code":401}})",
         "No auth credentials found"},
        {402, ErrorKind::NoCredits, R"({"error":{"message":"Insufficient credits","code":402}})", "no credits"},
        {403, ErrorKind::Refused, R"({"error":{"message":"Input flagged by moderation","code":403}})", "flagged"},
        {404, ErrorKind::NotFound, R"({"error":{"message":"No endpoints found for x/y","code":404}})", "x/y"},
        {429, ErrorKind::RateLimited, R"({"error":{"message":"Rate limit exceeded","code":429}})", "rate limited"},
        {400, ErrorKind::BadRequest, R"({"error":{"message":"Bad tool schema","code":400}})", "Bad tool schema"},
        {500, ErrorKind::ProviderDown, "Internal Server Error", "Internal Server Error"},
        {503, ErrorKind::ProviderDown,
         R"({"error":{"message":"No instances available","code":503,)"
         R"("metadata":{"raw":"upstream overloaded"}}})",
         "upstream overloaded"},
    };
    for (const Case& c : cases) {
        for (int streamed = 0; streamed < 2; ++streamed) {
            Rig rig;
            rig.a.settings().streamChat = streamed != 0;
            rig.fake.respond("POST", kChatUrl, jsonAnswer(c.status, c.body));
            REQUIRE(rig.a.ask("Hello"), "ask");
            REQUIRE(rig.run(), "not finished");
            const Failure& f = rig.a.failure();
            CHECK(rig.a.state() == State::Failed && f.kind == c.kind && f.status == c.status, "%d: %s (%d)", c.status,
                  errorKindName(f.kind), f.status);
            CHECK(f.message.find(c.says) != std::string::npos, "%d: '%s'", c.status, f.message.c_str());
        }
    }
    // Transport failures.
    {
        Rig rig;
        rig.a.settings().chatTimeout = 1.0;
        rig.fake.respond("POST", kChatUrl, net::FakeResponse::silence());
        REQUIRE(rig.a.ask("Hello"), "ask");
        for (int i = 0; i < 100 && rig.a.busy(); ++i) rig.step(0.05);
        CHECK(rig.a.failure().kind == ErrorKind::Timeout, "%s", errorKindName(rig.a.failure().kind));
    }
    {
        Rig rig;
        rig.fake.respond("POST", kChatUrl, net::FakeResponse::failure(net::ErrorKind::NoNetwork, "offline"));
        REQUIRE(rig.a.ask("Hello"), "ask");
        REQUIRE(rig.run(), "not finished");
        CHECK(rig.a.failure().kind == ErrorKind::NoNetwork, "%s", errorKindName(rig.a.failure().kind));
    }
    {
        Rig rig;
        std::string body = event(chunk(contentDelta("Half")));
        body += R"(data: {"error":{"code":502,"message":"Upstream error"},"choices":[{"index":0,"delta":)"
                R"({"content":""},"finish_reason":"error"}]})"
                "\n\n";
        rig.fake.respond("POST", kChatUrl, stream(body));
        REQUIRE(rig.a.ask("Hello"), "ask");
        REQUIRE(rig.run(), "not finished");
        CHECK(rig.a.failure().kind == ErrorKind::StreamError &&
                  rig.a.failure().message.find("Upstream error") != std::string::npos,
              "%s", rig.a.failure().message.c_str());
    }
    {
        Rig rig;
        rig.a.settings().baseUrl = "http://example.com/v1"; // plain http elsewhere than the loopback
        REQUIRE(rig.a.ask("Hello"), "ask");
        CHECK(rig.a.state() == State::Failed && rig.a.failure().kind == ErrorKind::NotAllowed, "%s %s",
              stateName(rig.a.state()), errorKindName(rig.a.failure().kind));
    }
    {
        Rig rig;
        rig.a.settings().baseUrl.clear();
        CHECK(!rig.a.ask("Hello"), "no base URL");
        rig.a.settings() = localPreset();
        CHECK(!rig.a.ask("Hello"), "no model");
    }
}

QUENCH_TEST(turn_key_never_leaks) {
    LogCapture log;
    std::string errors;
    std::string saved;
    // A provider that echoes the key in its error, a full voice turn, a failure of each stage.
    {
        Rig rig;
        rig.fake.respond("POST", kChatUrl,
                         jsonAnswer(401, std::string(R"({"error":{"message":"Invalid key )") + kKey + R"("}})"));
        REQUIRE(rig.a.ask("Hello"), "ask");
        rig.run();
        errors += rig.a.failure().message + "\n";
        CHECK(rig.a.failure().message.find("[key]") != std::string::npos, "%s", rig.a.failure().message.c_str());
    }
    {
        Rig rig;
        Colony colony;
        colony.add(rig.a);
        rig.fake.respond("POST", kSttUrl, jsonAnswer(200, R"({"text":"What is my power?"})"));
        rig.fake.respond("POST", kChatUrl, stream(sseTools({{"k", "get_status", "{}"}})));
        rig.fake.respond("POST", kTtsUrl,
                         net::FakeResponse::text(200, std::string(4800, '\0'),
                                                 {{"Content-Type", "audio/pcm;rate=24000;channels=1"}}));
        REQUIRE(rig.a.askByVoice(wav(1.0)), "voice");
        REQUIRE(rig.runUntil([&] { return rig.a.state() == State::CallingTool; }), "tool");
        rig.fake.respond("POST", kChatUrl, stream(sseText({"Twelve."})));
        REQUIRE(rig.run(), "voice turn");
        CHECK(rig.a.state() == State::Done, "%s", rig.a.failure().message.c_str());
        saved = json::write(rig.a.historyJson()) + json::write(settingsToJson(rig.a.settings()));
        rig.fake.respond("POST", kTtsUrl, jsonAnswer(500, std::string(R"({"error":{"message":")") + kKey + "\"}}"));
        REQUIRE(rig.a.speak("Again"), "speak");
        rig.run();
        errors += rig.a.failure().message + "\n";
    }
    const std::string all = log.all();
    CHECK(all.find(kKey) == std::string::npos, "the key is in the log:\n%s", all.c_str());
    CHECK(all.find("sk-or-v1") == std::string::npos, "a part of the key is in the log");
    CHECK(errors.find(kKey) == std::string::npos, "the key is in an error: %s", errors.c_str());
    CHECK(saved.find(kKey) == std::string::npos && saved.find("apiKey") == std::string::npos,
          "the key is in a save: %s", saved.c_str());
    CHECK(all.find("assistant: round 1: 200") != std::string::npos, "the log has its lines:\n%s", all.c_str());
    CHECK(all.find("What is my power") == std::string::npos && all.find("Twelve") == std::string::npos,
          "the log has the conversation in it:\n%s", all.c_str());
}

QUENCH_TEST(turn_shared_client_outlived) {
    net::FakeNetwork fake;
    double time = 0.0;
    net::Client client(net::ClientDesc{.fake = &fake, .clock = [&] { return time; }});
    fake.respond("POST", kChatUrl, stream(sseText({"Late."}), 8, 0.1));
    {
        Assistant a(AssistantDesc{.client = &client, .clock = [&] { return time; }});
        a.settings().apiKey = kKey;
        REQUIRE(a.ask("Hello"), "ask");
        time += 0.1;
        client.update();
        a.update();
    }
    // The assistant is gone: its request's handlers must not touch it.
    for (int i = 0; i < 20; ++i) {
        time += 0.1;
        client.update();
    }
    CHECK(client.activeCount() == 0 && fake.last()->cancelled, "the request was cancelled");

    // With a shared client that the game updates, a tool call is still visible for an update before it runs.
    Assistant a(AssistantDesc{.client = &client, .clock = [&] { return time; }});
    a.settings().apiKey = kKey;
    int ran = 0;
    Tool t;
    t.name = "ping";
    t.description = "Ping.";
    t.handler = [&](const json::Value&) {
        ++ran;
        return ToolResult::ok("pong");
    };
    REQUIRE(a.tools().add(std::move(t)), "add");
    fake.respond("POST", kChatUrl, stream(sseTools({{"p", "ping", "{}"}}), 4096, 0.0));
    REQUIRE(a.ask("Ping"), "ask");
    for (int i = 0; i < 10 && a.state() != State::CallingTool; ++i) {
        time += 0.02;
        client.update();
        a.update();
    }
    REQUIRE(a.state() == State::CallingTool, "%s", stateName(a.state()));
    CHECK(ran == 0, "the call ran in the frame it was announced");
    fake.respond("POST", kChatUrl, stream(sseText({"Pong."}), 4096, 0.0));
    time += 0.02;
    client.update();
    a.update();
    CHECK(ran == 1, "the call did not run in the next frame");
    for (int i = 0; i < 20 && a.busy(); ++i) {
        time += 0.02;
        client.update();
        a.update();
    }
    CHECK(ran == 1 && a.state() == State::Done && a.answer() == "Pong.", "%d %s", ran, a.failure().message.c_str());
}

QUENCH_TEST(turn_not_streamed) {
    Rig rig;
    rig.a.settings().streamChat = false;
    rig.fake.respond("POST", kChatUrl,
                     jsonAnswer(200, R"({"choices":[{"message":{"role":"assistant","content":"Plain."},)"
                                     R"("finish_reason":"stop"}],"usage":{"prompt_tokens":3,"completion_tokens":1}})"));
    REQUIRE(rig.a.ask("Hi"), "ask");
    REQUIRE(rig.run(), "not finished");
    CHECK(rig.a.state() == State::Done && rig.a.answer() == "Plain.", "%s", rig.a.failure().message.c_str());
    const std::vector<json::Value> bodies = rig.bodies(kChatUrl);
    CHECK(!bodies[0]["stream"].boolOr(true) && !bodies[0].has("stream_options") && !bodies[0].has("tools"), "%s",
          compact(bodies[0]).c_str());
}
