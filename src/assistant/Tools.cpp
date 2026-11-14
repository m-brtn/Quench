// The tool registry in the shape of MCP's tools and the schema check (quench/assistant/Tools.h).

#include <quench/assistant/Tools.h>

#include <quench/assistant/Chat.h> // writeCompactJson

#include "assistant/Text.h"

#include <cmath>
#include <exception>
#include <format>
#include <utility>

namespace quench::assistant {

ToolResult ToolResult::ok(std::string text) {
    ToolResult r;
    r.text = std::move(text);
    return r;
}

ToolResult ToolResult::ok(json::Value structured) {
    ToolResult r;
    r.structured = std::move(structured);
    return r;
}

ToolResult ToolResult::error(std::string message) {
    ToolResult r;
    r.isError = true;
    r.text = std::move(message);
    return r;
}

std::string ToolResult::contentText() const {
    if (!text.empty() || structured.isNull()) return text;
    return writeCompactJson(structured);
}

json::Value objectSchema() {
    json::Value v = json::Value::makeObject();
    v.object["type"] = json::Value::makeString("object");
    v.object["properties"] = json::Value::makeObject();
    return v;
}

namespace {

using Type = json::Value::Type;

const char* typeName(const json::Value& v) {
    switch (v.type) {
    case Type::Null: return "null";
    case Type::Bool: return "boolean";
    case Type::Number: return v.number == std::floor(v.number) ? "integer" : "number";
    case Type::String: return "string";
    case Type::Array: return "array";
    case Type::Object: return "object";
    }
    return "?";
}

bool hasType(const json::Value& v, const std::string& type) {
    if (type == "null") return v.isNull();
    if (type == "boolean") return v.type == Type::Bool;
    if (type == "number") return v.isNumber() && std::isfinite(v.number);
    if (type == "integer") return v.isNumber() && std::isfinite(v.number) && v.number == std::floor(v.number);
    if (type == "string") return v.type == Type::String;
    if (type == "array") return v.isArray();
    if (type == "object") return v.isObject();
    return true; // an unknown type name is not checked
}

bool equal(const json::Value& a, const json::Value& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
    case Type::Null: return true;
    case Type::Bool: return a.boolean == b.boolean;
    case Type::Number: return a.number == b.number;
    case Type::String: return a.string == b.string;
    case Type::Array:
        if (a.array.size() != b.array.size()) return false;
        for (std::size_t i = 0; i < a.array.size(); ++i) {
            if (!equal(a.array[i], b.array[i])) return false;
        }
        return true;
    case Type::Object:
        if (a.object.size() != b.object.size()) return false;
        for (const auto& [k, v] : a.object) {
            auto it = b.object.find(k);
            if (it == b.object.end() || !equal(v, it->second)) return false;
        }
        return true;
    }
    return false;
}

std::string shortJson(const json::Value& v) { return detail::trimText(writeCompactJson(v), 60); }

std::string place(const std::string& path) { return path.empty() ? "the arguments" : path; }

Result<void> check(const json::Value& schema, const json::Value& v, const std::string& path) {
    if (!schema.isObject()) return {}; // true, {} or a schema this subset does not read
    const json::Value& type = schema["type"];
    if (type.type == Type::String) {
        if (!hasType(v, type.string)) {
            return Error{std::format("{}: must be {} {}, not {}", place(path),
                                     type.string == "array" || type.string == "integer" ? "an" : "a", type.string,
                                     typeName(v))};
        }
    } else if (type.isArray()) {
        bool any = false;
        std::string names;
        for (const json::Value& t : type.array) {
            if (t.type != Type::String) continue;
            any = any || hasType(v, t.string);
            names += (names.empty() ? "" : " or ") + t.string;
        }
        if (!any && !names.empty())
            return Error{std::format("{}: must be {}, not {}", place(path), names, typeName(v))};
    }
    if (const json::Value& e = schema["enum"]; e.isArray()) {
        bool found = false;
        std::string options;
        for (const json::Value& option : e.array) {
            found = found || equal(option, v);
            if (options.size() < 200) options += (options.empty() ? "" : ", ") + writeCompactJson(option);
        }
        if (!found) return Error{std::format("{}: must be one of {} (got {})", place(path), options, shortJson(v))};
    }
    if (schema.has("const") && !equal(schema["const"], v)) {
        return Error{std::format("{}: must be {}", place(path), shortJson(schema["const"]))};
    }
    if (v.isNumber()) {
        const double n = v.number;
        if (const json::Value& m = schema["minimum"]; m.isNumber() && n < m.number) {
            return Error{std::format("{}: must be at least {}", place(path), writeCompactJson(m))};
        }
        if (const json::Value& m = schema["maximum"]; m.isNumber() && n > m.number) {
            return Error{std::format("{}: must be at most {}", place(path), writeCompactJson(m))};
        }
        if (const json::Value& m = schema["exclusiveMinimum"]; m.isNumber() && n <= m.number) {
            return Error{std::format("{}: must be more than {}", place(path), writeCompactJson(m))};
        }
        if (const json::Value& m = schema["exclusiveMaximum"]; m.isNumber() && n >= m.number) {
            return Error{std::format("{}: must be less than {}", place(path), writeCompactJson(m))};
        }
    }
    if (v.type == Type::String) {
        const std::size_t length = detail::codePoints(v.string);
        if (const json::Value& m = schema["minLength"]; m.isNumber() && double(length) < m.number) {
            return Error{std::format("{}: must have at least {} characters", place(path), writeCompactJson(m))};
        }
        if (const json::Value& m = schema["maxLength"]; m.isNumber() && double(length) > m.number) {
            return Error{std::format("{}: must have at most {} characters", place(path), writeCompactJson(m))};
        }
    }
    if (v.isArray()) {
        if (const json::Value& m = schema["minItems"]; m.isNumber() && double(v.array.size()) < m.number) {
            return Error{std::format("{}: must have at least {} items", place(path), writeCompactJson(m))};
        }
        if (const json::Value& m = schema["maxItems"]; m.isNumber() && double(v.array.size()) > m.number) {
            return Error{std::format("{}: must have at most {} items", place(path), writeCompactJson(m))};
        }
        if (const json::Value& items = schema["items"]; items.isObject()) {
            for (std::size_t i = 0; i < v.array.size(); ++i) {
                if (Result<void> r = check(items, v.array[i], std::format("{}[{}]", path.empty() ? "items" : path, i));
                    !r) {
                    return r;
                }
            }
        }
    }
    if (v.isObject()) {
        const json::Value& properties = schema["properties"];
        if (const json::Value& required = schema["required"]; required.isArray()) {
            for (const json::Value& name : required.array) {
                if (name.type == Type::String && !v.has(name.string)) {
                    return Error{
                        std::format("{}: missing (required)", path.empty() ? name.string : path + "." + name.string)};
                }
            }
        }
        for (const auto& [name, value] : v.object) {
            const std::string at = path.empty() ? name : path + "." + name;
            if (properties.has(name)) {
                if (Result<void> r = check(properties[name], value, at); !r) return r;
            } else if (schema["additionalProperties"].type == Type::Bool && !schema["additionalProperties"].boolean) {
                std::string known;
                if (properties.isObject()) {
                    for (const auto& p : properties.object) known += (known.empty() ? "" : ", ") + p.first;
                }
                return Error{std::format("{}: unknown property{}", at, known.empty() ? "" : " (known: " + known + ")")};
            }
        }
    }
    return {};
}

bool validName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (const char c : name) {
        const bool ok =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

json::Value annotationsJson(const ToolAnnotations& a) {
    json::Value v = json::Value::makeObject();
    if (!a.title.empty()) v.object["title"] = json::Value::makeString(a.title);
    if (a.readOnly) v.object["readOnlyHint"] = json::Value::makeBool(*a.readOnly);
    if (a.destructive) v.object["destructiveHint"] = json::Value::makeBool(*a.destructive);
    if (a.idempotent) v.object["idempotentHint"] = json::Value::makeBool(*a.idempotent);
    if (a.openWorld) v.object["openWorldHint"] = json::Value::makeBool(*a.openWorld);
    return v;
}

} // namespace

Result<void> validateSchema(const json::Value& schema, const json::Value& value) { return check(schema, value, ""); }

Result<void> ToolRegistry::add(Tool tool) {
    if (!validName(tool.name)) {
        return Error{std::format("'{}' is not a tool name (1 to 64 of a-z, A-Z, 0-9, _ and -)", tool.name)};
    }
    if (find(tool.name)) return Error{std::format("a tool named '{}' is already there", tool.name)};
    if (!tool.handler) return Error{std::format("the tool '{}' has no handler", tool.name)};
    if (tool.inputSchema.isNull()) tool.inputSchema = objectSchema();
    if (!tool.inputSchema.isObject() || tool.inputSchema["type"].stringOr("") != "object") {
        return Error{std::format("the input schema of '{}' must be a JSON Schema of type \"object\"", tool.name)};
    }
    m_tools.push_back(std::move(tool));
    return {};
}

bool ToolRegistry::remove(std::string_view name) {
    for (auto it = m_tools.begin(); it != m_tools.end(); ++it) {
        if (it->name == name) {
            m_tools.erase(it);
            return true;
        }
    }
    return false;
}

void ToolRegistry::clear() { m_tools.clear(); }

const Tool* ToolRegistry::find(std::string_view name) const {
    for (const Tool& t : m_tools) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

json::Value ToolRegistry::list() const {
    json::Value tools = json::Value::makeArray();
    for (const Tool& t : m_tools) {
        json::Value v = json::Value::makeObject();
        v.object["name"] = json::Value::makeString(t.name);
        if (!t.annotations.title.empty()) v.object["title"] = json::Value::makeString(t.annotations.title);
        v.object["description"] = json::Value::makeString(t.description);
        v.object["inputSchema"] = t.inputSchema;
        if (json::Value a = annotationsJson(t.annotations); !a.object.empty()) v.object["annotations"] = std::move(a);
        tools.array.push_back(std::move(v));
    }
    json::Value result = json::Value::makeObject();
    result.object["tools"] = std::move(tools);
    return result;
}

json::Value ToolRegistry::chatTools() const {
    json::Value tools = json::Value::makeArray();
    for (const Tool& t : m_tools) {
        json::Value fn = json::Value::makeObject();
        fn.object["name"] = json::Value::makeString(t.name);
        fn.object["description"] = json::Value::makeString(t.description);
        fn.object["parameters"] = t.inputSchema;
        json::Value v = json::Value::makeObject();
        v.object["type"] = json::Value::makeString("function");
        v.object["function"] = std::move(fn);
        tools.array.push_back(std::move(v));
    }
    return tools;
}

json::Value ToolRegistry::responsesTools() const {
    json::Value tools = json::Value::makeArray();
    for (const Tool& t : m_tools) {
        json::Value v = json::Value::makeObject();
        v.object["type"] = json::Value::makeString("function");
        v.object["name"] = json::Value::makeString(t.name);
        v.object["description"] = json::Value::makeString(t.description);
        v.object["parameters"] = t.inputSchema;
        v.object["strict"] = json::Value::makeBool(false);
        tools.array.push_back(std::move(v));
    }
    return tools;
}

ToolResult ToolRegistry::call(std::string_view name, const json::Value& arguments) const {
    const Tool* tool = find(name);
    if (!tool) {
        std::string names;
        for (const Tool& t : m_tools) names += (names.empty() ? "" : ", ") + t.name;
        return ToolResult::error(std::format("unknown tool '{}'; the tools are: {}", detail::trimText(name, 64),
                                             names.empty() ? "none" : names));
    }
    if (Result<void> valid = validateSchema(tool->inputSchema, arguments); !valid) {
        return ToolResult::error(std::format("invalid arguments for {}: {}", tool->name, valid.error()));
    }
    try {
        return tool->handler(arguments);
    } catch (const std::exception& e) {
        return ToolResult::error(std::format("the tool {} failed: {}", tool->name, e.what()));
    } catch (...) { return ToolResult::error(std::format("the tool {} failed", tool->name)); }
}

ToolResult ToolRegistry::callWithText(std::string_view name, std::string_view arguments) const {
    const std::string_view text = detail::trimSpace(arguments);
    if (text.empty()) return call(name, json::Value::makeObject());
    Result<json::Value> parsed = json::parse(std::string(text));
    if (!parsed) {
        return ToolResult::error(
            std::format("the arguments for {} are not valid JSON ({})", detail::trimText(name, 64), parsed.error()));
    }
    if (!parsed->isObject()) {
        return ToolResult::error(std::format("the arguments for {} must be a JSON object", detail::trimText(name, 64)));
    }
    return call(name, *parsed);
}

json::Value ToolRegistry::callResult(const ToolResult& result) {
    json::Value item = json::Value::makeObject();
    item.object["type"] = json::Value::makeString("text");
    item.object["text"] = json::Value::makeString(result.contentText());
    json::Value v = json::Value::makeObject();
    v.object["content"] = json::Value::makeArray();
    v.object["content"].array.push_back(std::move(item));
    if (result.structured.isObject()) v.object["structuredContent"] = result.structured;
    v.object["isError"] = json::Value::makeBool(result.isError);
    return v;
}

} // namespace quench::assistant
