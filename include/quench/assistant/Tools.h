#pragma once

// Tools a game gives a language model (docs/assistant.md, "Tools"), in the shape of the Model Context Protocol's
// tools: a name, a description, an input schema (JSON Schema as JSON) and annotations; list() returns what MCP's
// tools/list returns and callResult() what tools/call returns, so an MCP transport can later serve the same registry
// without touching a game's tools. Arguments are checked against the schema before a handler runs; a bad call (an
// unknown tool, arguments that are not JSON or fail the schema, a handler's error) is an error result the model
// reads, never a crash. Handlers run on the main thread (from Assistant::update()).

#include <quench/core/Json.h>
#include <quench/core/Result.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quench::assistant {

// MCP's ToolAnnotations: hints for a client (and a player's UI), not promises; unset hints are left out of list().
struct ToolAnnotations {
    std::string title;
    std::optional<bool> readOnly;    // readOnlyHint: the tool changes nothing
    std::optional<bool> destructive; // destructiveHint: it may undo or destroy something
    std::optional<bool> idempotent;  // idempotentHint: calling it twice does no more than once
    std::optional<bool> openWorld;   // openWorldHint: it reaches outside the game
};

// What a tool returns: text, or JSON (structured content; the model reads its compact text), or an error with a
// message the model can read and act on ("no walker 3; walkers: 1, 2").
struct ToolResult {
    bool isError = false;
    std::string text;
    json::Value structured; // null when the result is text only

    static ToolResult ok(std::string text);
    static ToolResult ok(json::Value structured);
    static ToolResult error(std::string message);

    // What the model receives: the text, or the structured content as compact JSON.
    std::string contentText() const;
};

using ToolHandler = std::function<ToolResult(const json::Value& arguments)>;

struct Tool {
    std::string name;        // 1 to 64 of a-z A-Z 0-9 _ - (what every provider accepts)
    std::string description; // for the model: what it does and when to use it
    // A JSON Schema of type "object". Checked: type (also a list of types), properties, required,
    // additionalProperties: false, enum, const, minimum, maximum, exclusiveMinimum, exclusiveMaximum, minLength,
    // maxLength, items, minItems, maxItems. Other keywords are sent to the model but not checked.
    json::Value inputSchema;
    ToolAnnotations annotations;
    ToolHandler handler;
};

// An empty object schema (a tool without arguments).
json::Value objectSchema();

// Checks `value` against the subset of JSON Schema above; the error names the place ("unit: must be one of ...",
// "items[2]: must be at most 10").
Result<void> validateSchema(const json::Value& schema, const json::Value& value);

class ToolRegistry {
public:
    // Refuses a bad name, a duplicate, a schema that is not an object schema, or no handler.
    Result<void> add(Tool tool);
    bool remove(std::string_view name);
    void clear();
    const Tool* find(std::string_view name) const;
    std::size_t size() const { return m_tools.size(); }
    const std::vector<Tool>& tools() const { return m_tools; }

    // MCP tools/list's result: {"tools":[{"name","description","inputSchema","annotations"?}...]} in add order.
    json::Value list() const;
    // The chat request's "tools": [{"type":"function","function":{"name","description","parameters"}}...].
    json::Value chatTools() const;
    // The Responses API's function tools (a live session's delegation.responses.tools):
    // [{"type":"function","name","description","parameters","strict":false}...]. Not strict: strict mode would
    // demand every property in "required" and additionalProperties false in every schema; the registry's own check
    // runs before a handler either way.
    json::Value responsesTools() const;

    // Runs a tool: an unknown name or arguments that fail the schema return an error result without running it; a
    // handler's exception becomes an error result too (where exceptions are caught: not in the web builds).
    ToolResult call(std::string_view name, const json::Value& arguments) const;
    // The same with the arguments as the model sent them (JSON text; empty means {}).
    ToolResult callWithText(std::string_view name, std::string_view arguments) const;
    // MCP tools/call's result: {"content":[{"type":"text","text":...}],"structuredContent"?,"isError"}.
    static json::Value callResult(const ToolResult& result);

private:
    std::vector<Tool> m_tools;
};

} // namespace quench::assistant
