// The tool registry (quench/assistant/Tools.h): MCP's tools/list and tools/call shapes, the chat request's tools, the
// schema check and the error results a bad call gets.

#include "Rig.h"
#include "Test.h"

#include "core/Platform.h"

#include <quench/assistant/Tools.h>

#include <stdexcept>
#include <string>

using namespace quench;
using namespace quench::assistant;
using namespace assistant_test;

namespace {

Tool moveTool(int* calls = nullptr) {
    Tool t;
    t.name = "order_move";
    t.description = "Send a unit to a place.";
    t.inputSchema = parse(R"({
        "type": "object",
        "properties": {
            "unit": {"type": "string", "enum": ["Walker 1", "Walker 2"]},
            "x": {"type": "number", "minimum": -100, "maximum": 100},
            "count": {"type": "integer", "minimum": 1},
            "urgent": {"type": "boolean"},
            "path": {"type": "array", "items": {"type": "integer", "maximum": 10}, "maxItems": 3},
            "label": {"type": "string", "maxLength": 4}
        },
        "required": ["unit", "x"],
        "additionalProperties": false
    })");
    t.annotations.destructive = false;
    t.annotations.title = "Move a unit";
    t.handler = [calls](const json::Value& args) {
        if (calls) ++*calls;
        return ToolResult::ok("moving " + args["unit"].string);
    };
    return t;
}

std::string error(const ToolResult& r) { return r.isError ? r.text : std::string("(not an error)"); }

} // namespace

QUENCH_TEST(tools_list_is_mcp_tools_list) {
    ToolRegistry reg;
    REQUIRE(reg.add(moveTool()), "add");
    Tool status;
    status.name = "get_status";
    status.description = "The colony's power and stock.";
    status.annotations.readOnly = true;
    status.handler = [](const json::Value&) { return ToolResult::ok(parse(R"({"power":12})")); };
    REQUIRE(reg.add(std::move(status)), "add");

    const json::Value list = reg.list();
    REQUIRE(list["tools"].isArray() && list["tools"].array.size() == 2, "%s", compact(list).c_str());
    const json::Value& first = list["tools"].array[0];
    CHECK(first["name"].stringOr("") == "order_move" && first["title"].stringOr("") == "Move a unit", "first");
    CHECK(first["inputSchema"]["type"].stringOr("") == "object", "inputSchema");
    CHECK(first["annotations"]["destructiveHint"].boolOr(true) == false && !first["annotations"].has("readOnlyHint"),
          "%s", compact(first["annotations"]).c_str());
    const json::Value& second = list["tools"].array[1];
    CHECK(second["annotations"]["readOnlyHint"].boolOr(false), "readOnlyHint");
    CHECK(second["inputSchema"]["type"].stringOr("") == "object" && second["inputSchema"]["properties"].isObject(),
          "the default schema: %s", compact(second["inputSchema"]).c_str());

    const json::Value chat = reg.chatTools();
    REQUIRE(chat.isArray() && chat.array.size() == 2, "chatTools");
    CHECK(chat.array[0]["type"].stringOr("") == "function" &&
              chat.array[0]["function"]["name"].stringOr("") == "order_move",
          "%s", compact(chat.array[0]).c_str());
    CHECK(chat.array[0]["function"]["parameters"]["required"].array.size() == 2, "parameters");
}

QUENCH_TEST(tools_call_result_is_mcp_call_result) {
    const json::Value text = ToolRegistry::callResult(ToolResult::ok("fine"));
    CHECK(compact(text) == R"({"content":[{"text":"fine","type":"text"}],"isError":false})", "%s",
          compact(text).c_str());
    const json::Value structured = ToolRegistry::callResult(ToolResult::ok(parse(R"({"power":12})")));
    CHECK(structured["structuredContent"]["power"].numberOr(0) == 12, "%s", compact(structured).c_str());
    CHECK(structured["content"].array[0]["text"].stringOr("") == R"({"power":12})", "text of structured");
    const json::Value err = ToolRegistry::callResult(ToolResult::error("no such unit"));
    CHECK(err["isError"].boolOr(false) && err["content"].array[0]["text"].stringOr("") == "no such unit", "%s",
          compact(err).c_str());
}

QUENCH_TEST(tools_add_refuses_bad_tools) {
    ToolRegistry reg;
    Tool t = moveTool();
    t.name = "has space";
    CHECK(!reg.add(t), "a name with a space");
    t.name = std::string(65, 'a');
    CHECK(!reg.add(t), "a name of 65 characters");
    t = moveTool();
    t.handler = nullptr;
    CHECK(!reg.add(t), "no handler");
    t = moveTool();
    t.inputSchema = parse(R"({"type":"string"})");
    CHECK(!reg.add(t), "a schema that is not an object schema");
    CHECK(reg.add(moveTool()), "the good one");
    CHECK(!reg.add(moveTool()), "a duplicate");
    CHECK(reg.size() == 1 && reg.find("order_move") && reg.remove("order_move") && reg.size() == 0, "remove");
}

QUENCH_TEST(tools_schema_checks) {
    int calls = 0;
    ToolRegistry reg;
    REQUIRE(reg.add(moveTool(&calls)), "add");
    const auto call = [&](const char* args) { return reg.callWithText("order_move", args); };

    CHECK(!call(R"({"unit":"Walker 1","x":5})").isError, "a good call");
    CHECK(!call(R"({"unit":"Walker 2","x":-100,"count":2,"urgent":true,"path":[1,2,3],"label":"ab"})").isError,
          "a good call with everything");
    CHECK(calls == 2, "%d calls", calls);

    CHECK(error(call(R"({"x":5})")).find("unit: missing (required)") != std::string::npos, "%s",
          error(call(R"({"x":5})")).c_str());
    CHECK(error(call(R"({"unit":"Walker 3","x":5})")).find("must be one of \"Walker 1\", \"Walker 2\"") !=
              std::string::npos,
          "%s", error(call(R"({"unit":"Walker 3","x":5})")).c_str());
    CHECK(error(call(R"({"unit":7,"x":5})")).find("unit: must be a string, not integer") != std::string::npos, "%s",
          error(call(R"({"unit":7,"x":5})")).c_str());
    CHECK(error(call(R"({"unit":"Walker 1","x":101})")).find("x: must be at most 100") != std::string::npos, "%s",
          error(call(R"({"unit":"Walker 1","x":101})")).c_str());
    CHECK(error(call(R"({"unit":"Walker 1","x":1,"count":1.5})")).find("count: must be an integer") !=
              std::string::npos,
          "%s", error(call(R"({"unit":"Walker 1","x":1,"count":1.5})")).c_str());
    CHECK(error(call(R"({"unit":"Walker 1","x":1,"count":0})")).find("count: must be at least 1") != std::string::npos,
          "count 0");
    CHECK(error(call(R"({"unit":"Walker 1","x":1,"urgent":"yes"})")).find("urgent: must be a boolean") !=
              std::string::npos,
          "urgent");
    CHECK(error(call(R"({"unit":"Walker 1","x":1,"path":[1,20]})")).find("path[1]: must be at most 10") !=
              std::string::npos,
          "%s", error(call(R"({"unit":"Walker 1","x":1,"path":[1,20]})")).c_str());
    CHECK(error(call(R"({"unit":"Walker 1","x":1,"path":[1,2,3,4]})")).find("path: must have at most 3 items") !=
              std::string::npos,
          "maxItems");
    CHECK(error(call(R"({"unit":"Walker 1","x":1,"label":"abcde"})")).find("at most 4 characters") != std::string::npos,
          "maxLength");
    CHECK(error(call(R"({"unit":"Walker 1","x":1,"speed":3})")).find("speed: unknown property") != std::string::npos,
          "%s", error(call(R"({"unit":"Walker 1","x":1,"speed":3})")).c_str());
    CHECK(calls == 2, "a bad call ran the handler: %d calls", calls);
}

QUENCH_TEST(tools_bad_calls_are_error_results) {
    int calls = 0;
    ToolRegistry reg;
    REQUIRE(reg.add(moveTool(&calls)), "add");
    CHECK(error(reg.callWithText("order_move", "{\"unit\":")).find("not valid JSON") != std::string::npos, "%s",
          error(reg.callWithText("order_move", "{\"unit\":")).c_str());
    CHECK(error(reg.callWithText("order_move", "[1]")).find("must be a JSON object") != std::string::npos, "array");
    CHECK(error(reg.callWithText("fly", "{}")).find("unknown tool 'fly'; the tools are: order_move") !=
              std::string::npos,
          "%s", error(reg.callWithText("fly", "{}")).c_str());
    CHECK(error(reg.callWithText("order_move", "")).find("unit: missing") != std::string::npos,
          "empty arguments are {}");
    CHECK(calls == 0, "%d", calls);
}

QUENCH_TEST(tools_schema_type_lists_and_const) {
    const json::Value schema =
        parse(R"({"type":"object","properties":{"v":{"type":["string","null"]},"k":{"const":"fixed"}}})");
    CHECK(validateSchema(schema, parse(R"({"v":null})")).ok(), "null allowed");
    CHECK(validateSchema(schema, parse(R"({"v":"s","k":"fixed"})")).ok(), "string allowed");
    const Result<void> bad = validateSchema(schema, parse(R"({"v":3})"));
    CHECK(!bad && bad.error().find("v: must be string or null") != std::string::npos, "%s", bad.error().c_str());
    CHECK(!validateSchema(schema, parse(R"({"k":"other"})")), "const");
    CHECK(!validateSchema(schema, parse("[]")), "not an object");
}

QUENCH_TEST(tools_handler_exception) {
    if (QUENCH_PLATFORM_WEB) QUENCH_SKIP("the web builds do not catch exceptions");
    ToolRegistry reg;
    Tool thrower;
    thrower.name = "explode";
    thrower.description = "Throws.";
    thrower.handler = [](const json::Value&) -> ToolResult { throw std::runtime_error("the game said no"); };
    REQUIRE(reg.add(std::move(thrower)), "add");
    CHECK(error(reg.callWithText("explode", "")).find("the tool explode failed: the game said no") != std::string::npos,
          "%s", error(reg.callWithText("explode", "")).c_str());
}
