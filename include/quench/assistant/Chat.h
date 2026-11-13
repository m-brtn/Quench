#pragma once

// The chat completion request and its streamed answer (docs/assistant.md, "The chat client"): the request body of
// POST /chat/completions with messages, tools and tool_choice, and an incremental parser of the answer, as
// server-sent events (data: lines, ':' comments as keep-alives, data: [DONE]) or as one JSON body (a server that does
// not stream). Pieces may end anywhere: in the middle of a line, of a UTF-8 character or of a JSON string.

#include <quench/assistant/Provider.h>
#include <quench/core/Json.h>
#include <quench/net/Http.h>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quench::assistant {

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments; // JSON text, gathered from the deltas
};

struct Usage {
    std::int64_t promptTokens = 0;
    std::int64_t completionTokens = 0;
    double cost = 0.0;      // in the provider's credits (US dollars on OpenRouter), when it says
    bool costKnown = false; // some answer carried a cost

    void add(const Usage& other);
};

struct ChatResult {
    std::string text;
    std::vector<ToolCall> toolCalls; // by their index in the stream
    std::string finishReason;        // "stop", "tool_calls", "length", "error", ...
    Usage usage;
    std::string model;    // as the answer names it
    std::string provider; // OpenRouter's "provider", when given
    bool sawDone = false; // data: [DONE] arrived
    Failure failure;      // StreamError (a mid-stream error, an early end) or BadResponse
};

class ChatStreamParser {
public:
    // Each piece of text as it arrives (already a whole UTF-8 sequence).
    std::function<void(std::string_view delta)> onText;
    // A tool call's first delta (its index, id and name).
    std::function<void(const ToolCall& call)> onToolCall;

    void feed(std::span<const std::uint8_t> bytes);
    void feed(std::string_view bytes);
    // The body ended: flushes a last line without a newline, checks that the answer is complete (a finish reason or
    // [DONE]), parses a non-streamed JSON body.
    void finish();

    const ChatResult& result() const { return m_result; }
    std::size_t events() const { return m_events; }

private:
    void line(std::string_view text);
    void event();
    void chunk(const json::Value& value);

    ChatResult m_result;
    std::string m_line;       // the bytes of the line being read
    std::string m_data;       // the data of the event being read
    std::string m_whole;      // a body that is one JSON document (not a stream)
    std::vector<int> m_index; // the stream's tool call index of each m_result.toolCalls entry
    std::size_t m_events = 0;
    int m_mode = 0; // 0: unknown, 1: events, 2: one JSON body
    bool m_finished = false;
};

enum class ToolChoice : std::uint8_t { Auto, None, Required };

struct ChatOptions {
    ToolChoice toolChoice = ToolChoice::Auto;
    std::string forceTool; // non-empty: tool_choice names this function
};

// POST <baseUrl>/chat/completions with the settings' model, `messages` (an array of {role, content, ...}), `tools`
// (ToolRegistry::chatTools(); left out when empty), tool_choice, parallel_tool_calls, stream and the options.
net::Request buildChatRequest(const ProviderSettings& settings, const json::Value& messages, const json::Value& tools,
                              const ChatOptions& options = {});

// One line, no spaces: what request bodies and tool arguments use (json::write indents).
std::string writeCompactJson(const json::Value& value);

} // namespace quench::assistant
