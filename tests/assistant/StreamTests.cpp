// The chat answer's parser (quench/assistant/Chat.h): server-sent events cut at every byte, tool calls gathered from
// their deltas, keep-alive comments, errors in the middle, an early end, an answer that is one JSON body.

#include "Rig.h"
#include "Test.h"

#include <quench/assistant/Chat.h>

#include <cmath>
#include <string>

using namespace quench;
using namespace quench::assistant;
using namespace assistant_test;

namespace {

ChatResult parseAll(std::string_view body, std::size_t piece = 0) {
    ChatStreamParser p;
    if (piece == 0) {
        p.feed(body);
    } else {
        for (std::size_t at = 0; at < body.size(); at += piece) p.feed(body.substr(at, piece));
    }
    p.finish();
    return p.result();
}

bool same(const ChatResult& a, const ChatResult& b) {
    if (a.text != b.text || a.finishReason != b.finishReason || a.toolCalls.size() != b.toolCalls.size()) return false;
    for (std::size_t i = 0; i < a.toolCalls.size(); ++i) {
        if (a.toolCalls[i].id != b.toolCalls[i].id || a.toolCalls[i].name != b.toolCalls[i].name ||
            a.toolCalls[i].arguments != b.toolCalls[i].arguments) {
            return false;
        }
    }
    return a.usage.promptTokens == b.usage.promptTokens && a.usage.completionTokens == b.usage.completionTokens &&
           a.usage.cost == b.usage.cost && a.sawDone == b.sawDone && a.failure.kind == b.failure.kind;
}

} // namespace

QUENCH_TEST(stream_parallel_tools_fixture) {
    const ChatResult r = parseAll(text(quench::test::parallelToolsSse()));
    REQUIRE(!r.failure.failed(), "failure: %s", r.failure.message.c_str());
    REQUIRE(r.toolCalls.size() == 2, "%zu tool calls", r.toolCalls.size());
    CHECK(r.toolCalls[0].name == "get_status" && r.toolCalls[0].arguments == "{}", "%s %s", r.toolCalls[0].name.c_str(),
          r.toolCalls[0].arguments.c_str());
    CHECK(r.toolCalls[0].id == "call_fixture0000000000000001", "%s", r.toolCalls[0].id.c_str());
    CHECK(r.toolCalls[1].name == "list_units" && r.toolCalls[1].arguments == R"({"kind":"walker"})", "%s %s",
          r.toolCalls[1].name.c_str(), r.toolCalls[1].arguments.c_str());
    CHECK(r.text.empty(), "text '%s'", r.text.c_str());
    CHECK(r.finishReason == "tool_calls", "%s", r.finishReason.c_str());
    CHECK(r.sawDone, "no [DONE]");
    CHECK(r.usage.promptTokens == 1291 && r.usage.completionTokens == 45, "%lld %lld", (long long)r.usage.promptTokens,
          (long long)r.usage.completionTokens);
    CHECK(r.usage.costKnown && std::abs(r.usage.cost - 3.8095e-05) < 1e-12, "cost %g", r.usage.cost);
    CHECK(r.model == "openai/gpt-6-luna" && r.provider == "OpenAI", "%s %s", r.model.c_str(), r.provider.c_str());
}

QUENCH_TEST(stream_text_fixture) {
    std::string seen;
    ChatStreamParser p;
    int pieces = 0;
    p.onText = [&](std::string_view d) {
        seen += d;
        ++pieces;
    };
    p.feed(text(quench::test::textAnswerSse()));
    p.finish();
    const ChatResult& r = p.result();
    REQUIRE(!r.failure.failed(), "failure: %s", r.failure.message.c_str());
    const std::string expected = "Walker 1 is collecting E7 \xE2\x80\x94 the nearest energy cell, 40 m north. Power: "
                                 "+12 kW (net).";
    CHECK(r.text == expected, "'%s'", r.text.c_str());
    CHECK(seen == expected && pieces == 5, "%d pieces '%s'", pieces, seen.c_str());
    CHECK(r.finishReason == "stop" && r.sawDone && r.toolCalls.empty(), "%s", r.finishReason.c_str());
    CHECK(r.usage.promptTokens == 1356 && r.usage.completionTokens == 17, "usage");
}

// Every split of each fixture in two, and every piece size from 1 to 17 bytes: the same result as the whole body.
QUENCH_TEST(stream_every_byte_boundary) {
    for (const std::string& body : {text(quench::test::parallelToolsSse()), text(quench::test::textAnswerSse())}) {
        const ChatResult whole = parseAll(body);
        int bad = 0;
        for (std::size_t cut = 1; cut < body.size(); ++cut) {
            ChatStreamParser p;
            std::string fromText;
            p.onText = [&](std::string_view d) { fromText += d; };
            p.feed(std::string_view(body).substr(0, cut));
            p.feed(std::string_view(body).substr(cut));
            p.finish();
            if (!same(p.result(), whole) || fromText != whole.text) {
                if (++bad < 4) CHECK(false, "a cut at byte %zu of %zu differs", cut, body.size());
            }
        }
        CHECK(bad == 0, "%d of %zu cuts differ", bad, body.size() - 1);
        for (std::size_t piece = 1; piece <= 17; ++piece) {
            CHECK(same(parseAll(body, piece), whole), "pieces of %zu bytes differ", piece);
        }
    }
}

QUENCH_TEST(stream_arguments_split_across_deltas) {
    // A call's arguments cut in the middle of a JSON string and of a UTF-8 character, in deltas of 1 to 5 bytes.
    const std::string args = R"({"unit":"Walker 1","note":"caf)"
                             "\xC3\xA9"
                             R"( — \"quoted\"","count":3})";
    std::string body = event(chunk(toolDelta(0, "call_1", "order_move", "")));
    std::size_t at = 0, n = 1;
    while (at < args.size()) {
        // Arguments travel as JSON strings: a fragment may end inside an escape of the outer string but not inside
        // a UTF-8 sequence of the decoded text, so cut the decoded text on character starts.
        std::size_t len = std::min(n, args.size() - at);
        while (at + len < args.size() && (static_cast<unsigned char>(args[at + len]) & 0xC0) == 0x80) ++len;
        body += event(chunk(toolDelta(0, "", "", args.substr(at, len))));
        at += len;
        n = n % 5 + 1;
    }
    body += event(chunk(contentDelta(""), "tool_calls"));
    body += "data: [DONE]\n\n";
    const ChatResult r = parseAll(body, 7);
    REQUIRE(r.toolCalls.size() == 1, "%zu calls", r.toolCalls.size());
    CHECK(r.toolCalls[0].arguments == args, "'%s'", r.toolCalls[0].arguments.c_str());
    const json::Value parsed = parse(r.toolCalls[0].arguments);
    CHECK(parsed["unit"].stringOr("") == "Walker 1" && parsed["count"].numberOr(0) == 3, "arguments do not parse");
}

QUENCH_TEST(stream_parallel_calls_interleaved) {
    // Two calls whose deltas alternate (index 1 before index 0 ends), then the indices out of order.
    std::string body;
    body += event(chunk(toolDelta(0, "a", "first", R"({"x":)")));
    body += event(chunk(toolDelta(1, "b", "second", R"({"y":)")));
    body += event(chunk(toolDelta(0, "", "", "1}")));
    body += event(chunk(toolDelta(1, "", "", "2}")));
    body += event(chunk(contentDelta(""), "tool_calls"));
    body += "data: [DONE]\n\n";
    const ChatResult r = parseAll(body);
    REQUIRE(r.toolCalls.size() == 2, "%zu", r.toolCalls.size());
    CHECK(r.toolCalls[0].name == "first" && r.toolCalls[0].arguments == R"({"x":1})", "%s",
          r.toolCalls[0].arguments.c_str());
    CHECK(r.toolCalls[1].name == "second" && r.toolCalls[1].arguments == R"({"y":2})", "%s",
          r.toolCalls[1].arguments.c_str());

    std::string reversed;
    reversed += event(chunk(toolDelta(1, "b", "second", "{}")));
    reversed += event(chunk(toolDelta(0, "a", "first", "{}")));
    reversed += event(chunk(contentDelta(""), "tool_calls"));
    const ChatResult s = parseAll(reversed);
    REQUIRE(s.toolCalls.size() == 2, "%zu", s.toolCalls.size());
    CHECK(s.toolCalls[0].name == "first" && s.toolCalls[1].name == "second", "not sorted by index");
}

QUENCH_TEST(stream_keep_alive_comments_and_crlf) {
    std::string body = ": OPENROUTER PROCESSING\r\n\r\n";
    body += ":\r\n\r\n";
    body += "data: " + compact(chunk(contentDelta("Hel"))) + "\r\n\r\n";
    body += ": OPENROUTER PROCESSING\r\n\r\n";
    body += "event: message\r\nid: 7\r\ndata: " + compact(chunk(contentDelta("lo"))) + "\r\n\r\n";
    body += "data: " + compact(chunk(contentDelta(""), "stop")) + "\r\n\r\n";
    body += "data: [DONE]\r\n\r\n";
    for (std::size_t piece : {std::size_t(0), std::size_t(1), std::size_t(3)}) {
        const ChatResult r = parseAll(body, piece);
        CHECK(!r.failure.failed() && r.text == "Hello" && r.sawDone, "piece %zu: '%s' %s", piece, r.text.c_str(),
              r.failure.message.c_str());
    }
}

QUENCH_TEST(stream_mid_stream_error) {
    std::string body = event(chunk(contentDelta("Partial")));
    json::Value err = chunk(contentDelta(""), "error");
    json::Value e = json::Value::makeObject();
    e.object["code"] = json::Value::makeString("server_error");
    e.object["message"] = json::Value::makeString("Provider disconnected unexpectedly");
    err.object["error"] = std::move(e);
    body += event(err);
    body += "data: [DONE]\n\n";
    const ChatResult r = parseAll(body);
    CHECK(r.failure.kind == ErrorKind::StreamError, "%s", errorKindName(r.failure.kind));
    CHECK(r.failure.message.find("Provider disconnected unexpectedly") != std::string::npos &&
              r.failure.message.find("server_error") != std::string::npos,
          "%s", r.failure.message.c_str());
    CHECK(r.text == "Partial", "%s", r.text.c_str());
}

QUENCH_TEST(stream_missing_done) {
    // A finish reason without [DONE] is a complete answer; neither of them is an early end.
    std::string complete = event(chunk(contentDelta("Done.")));
    complete += event(chunk(contentDelta(""), "stop"));
    const ChatResult a = parseAll(complete);
    CHECK(!a.failure.failed() && a.text == "Done." && !a.sawDone, "%s", a.failure.message.c_str());

    const ChatResult b = parseAll(event(chunk(contentDelta("Half an ans"))));
    CHECK(b.failure.kind == ErrorKind::StreamError, "%s", errorKindName(b.failure.kind));
    CHECK(b.text == "Half an ans", "%s", b.text.c_str());

    // The last event without its blank line (the connection closed after it) still counts.
    const ChatResult c = parseAll("data: " + compact(chunk(contentDelta("Last"), "stop")));
    CHECK(!c.failure.failed() && c.text == "Last", "%s", c.failure.message.c_str());

    const ChatResult empty = parseAll("");
    CHECK(empty.failure.kind == ErrorKind::BadResponse, "%s", errorKindName(empty.failure.kind));
}

QUENCH_TEST(stream_bad_event) {
    const ChatResult r = parseAll("data: {not json\n\n" + event(chunk(contentDelta("x"), "stop")));
    CHECK(r.failure.kind == ErrorKind::BadResponse, "%s", errorKindName(r.failure.kind));
}

QUENCH_TEST(stream_whole_json_body) {
    // A server that does not stream answers with one chat.completion object.
    const std::string body = R"({"id":"x","model":"local-model","choices":[{"index":0,"message":{"role":"assistant",)"
                             R"("content":null,"tool_calls":[{"id":"c1","type":"function","function":{"name":"a",)"
                             R"("arguments":"{\"n\":1}"}},{"id":"c2","type":"function","function":{"name":"b",)"
                             R"("arguments":{"m":2}}}]},"finish_reason":"tool_calls"}],)"
                             R"("usage":{"prompt_tokens":5,"completion_tokens":6}})";
    const ChatResult r = parseAll(body, 10);
    REQUIRE(!r.failure.failed(), "%s", r.failure.message.c_str());
    REQUIRE(r.toolCalls.size() == 2, "%zu", r.toolCalls.size());
    CHECK(r.toolCalls[0].arguments == R"({"n":1})" && r.toolCalls[1].arguments == R"({"m":2})", "%s %s",
          r.toolCalls[0].arguments.c_str(), r.toolCalls[1].arguments.c_str());
    CHECK(r.usage.promptTokens == 5 && !r.usage.costKnown, "usage");

    const ChatResult answer = parseAll(R"({"choices":[{"message":{"role":"assistant","content":"Hi."},)"
                                       R"("finish_reason":"stop"}]})");
    CHECK(answer.text == "Hi." && !answer.failure.failed(), "%s", answer.text.c_str());
    CHECK(parseAll(R"({"object":"something else"})").failure.kind == ErrorKind::BadResponse, "no choices");
}

QUENCH_TEST(stream_surrogate_pairs_repaired) {
    // A server that writes JSON with ASCII escapes sends an emoji as a surrogate pair.
    const std::string body = "data: " + std::string(R"({"choices":[{"delta":{"content":"ok 🚀 \ud800 x"},)") +
                             R"("finish_reason":"stop"}]})" + "\n\n";
    const ChatResult r = parseAll(body);
    CHECK(r.text == "ok \xF0\x9F\x9A\x80 \xEF\xBF\xBD x", "'%s'", r.text.c_str());
}

QUENCH_TEST(stream_compact_json) {
    json::Value v = parse(R"({"b":[1,2.5,true,null],"a":"x\"y\n","c":{}})");
    CHECK(writeCompactJson(v) == R"({"a":"x\"y\n","b":[1,2.5,true,null],"c":{}})", "%s", writeCompactJson(v).c_str());
}
