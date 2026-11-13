// The chat completion request and the parser of its answer (quench/assistant/Chat.h).

#include <quench/assistant/Chat.h>

#include "assistant/Text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <numeric>
#include <utility>

namespace quench::assistant {

void Usage::add(const Usage& other) {
    promptTokens += other.promptTokens;
    completionTokens += other.completionTokens;
    cost += other.cost;
    costKnown = costKnown || other.costKnown;
}

// ---------------------------------------------------------------------------------------------------- compact JSON

namespace {

void writeString(std::string& out, const std::string& s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void writeNumber(std::string& out, double v) {
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    char buf[40];
    if (v == std::floor(v) && std::abs(v) < 9.0e15) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        out += buf;
        return;
    }
    for (int precision = 6; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    out += buf;
}

void writeValue(std::string& out, const json::Value& v) {
    switch (v.type) {
    case json::Value::Type::Null: out += "null"; break;
    case json::Value::Type::Bool: out += v.boolean ? "true" : "false"; break;
    case json::Value::Type::Number: writeNumber(out, v.number); break;
    case json::Value::Type::String: writeString(out, v.string); break;
    case json::Value::Type::Array:
        out += '[';
        for (std::size_t i = 0; i < v.array.size(); ++i) {
            if (i) out += ',';
            writeValue(out, v.array[i]);
        }
        out += ']';
        break;
    case json::Value::Type::Object: {
        out += '{';
        bool first = true;
        for (const auto& [key, value] : v.object) {
            if (!first) out += ',';
            first = false;
            writeString(out, key);
            out += ':';
            writeValue(out, value);
        }
        out += '}';
        break;
    }
    }
}

} // namespace

std::string writeCompactJson(const json::Value& value) {
    std::string out;
    writeValue(out, value);
    return out;
}

// ---------------------------------------------------------------------------------------------------- the parser

void ChatStreamParser::feed(std::span<const std::uint8_t> bytes) {
    feed(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

void ChatStreamParser::feed(std::string_view bytes) {
    if (m_finished) return;
    if (m_mode == 0) {
        // A body that starts with '{' is one JSON document (a server that does not stream, an error); anything else
        // is a stream of events. Leading blank lines decide nothing.
        std::size_t i = 0;
        while (i < bytes.size() && (bytes[i] == ' ' || bytes[i] == '\t' || bytes[i] == '\r' || bytes[i] == '\n')) ++i;
        if (i == bytes.size()) return;
        bytes.remove_prefix(i);
        m_mode = bytes.front() == '{' ? 2 : 1;
    }
    if (m_mode == 2) {
        m_whole.append(bytes);
        return;
    }
    const std::size_t scanFrom = m_line.size();
    m_line.append(bytes);
    std::size_t start = 0;
    for (std::size_t at = m_line.find('\n', scanFrom); at != std::string::npos; at = m_line.find('\n', start)) {
        std::string_view text(m_line.data() + start, at - start);
        if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
        line(text);
        start = at + 1;
    }
    m_line.erase(0, start);
}

void ChatStreamParser::line(std::string_view text) {
    if (text.empty()) {
        event();
        return;
    }
    if (text.front() == ':') return; // a comment: OpenRouter's ": OPENROUTER PROCESSING" keep-alive
    const std::size_t colon = text.find(':');
    const std::string_view field = text.substr(0, colon);
    std::string_view value = colon == std::string_view::npos ? std::string_view() : text.substr(colon + 1);
    if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    if (field != "data") return; // event:, id:, retry: are not used by chat completions
    if (!m_data.empty()) m_data += '\n';
    m_data.append(value);
    if (m_data.empty()) m_data = " "; // an empty data line still makes an event
}

void ChatStreamParser::event() {
    if (m_data.empty()) return;
    std::string data = std::move(m_data);
    m_data.clear();
    const std::string_view text = detail::trimSpace(data);
    if (text.empty()) return;
    ++m_events;
    if (text == "[DONE]") {
        m_result.sawDone = true;
        return;
    }
    Result<json::Value> value = json::parse(std::string(text));
    if (!value) {
        if (!m_result.failure.failed()) {
            m_result.failure = {ErrorKind::BadResponse, 0,
                                std::format("an event of the answer is not JSON ({})", value.error())};
        }
        return;
    }
    chunk(*value);
}

void ChatStreamParser::chunk(const json::Value& v) {
    if (!v.isObject()) return;
    if (const json::Value& error = v["error"]; !error.isNull()) {
        std::string message = error.isObject() ? error["message"].stringOr("") : error.stringOr("");
        std::string code;
        if (error["code"].isNumber()) code = std::format(" ({})", writeCompactJson(error["code"]));
        if (error["code"].type == json::Value::Type::String) code = " (" + error["code"].string + ")";
        m_result.failure = {ErrorKind::StreamError, 0,
                            std::format("the provider reported an error during the answer{}: {}", code,
                                        detail::trimText(message.empty() ? "no message" : message, 300))};
    }
    if (v["model"].type == json::Value::Type::String) m_result.model = v["model"].string;
    if (v["provider"].type == json::Value::Type::String) m_result.provider = v["provider"].string;
    if (const json::Value& usage = v["usage"]; usage.isObject()) {
        // The last chunk's usage covers the whole answer: replace, do not add.
        m_result.usage.promptTokens = std::int64_t(usage["prompt_tokens"].numberOr(0.0));
        m_result.usage.completionTokens = std::int64_t(usage["completion_tokens"].numberOr(0.0));
        if (usage["cost"].isNumber()) {
            m_result.usage.cost = usage["cost"].number;
            m_result.usage.costKnown = true;
        }
    }
    const json::Value& choices = v["choices"];
    if (!choices.isArray() || choices.array.empty()) return;
    const json::Value& choice = choices.array.front();
    const bool streamed = choice.has("delta");
    const json::Value& delta = streamed ? choice["delta"] : choice["message"];
    if (delta.isObject()) {
        if (delta["content"].type == json::Value::Type::String && !delta["content"].string.empty()) {
            const std::string& piece = delta["content"].string;
            m_result.text += piece;
            if (onText) onText(piece);
        }
        if (const json::Value& calls = delta["tool_calls"]; calls.isArray()) {
            for (std::size_t i = 0; i < calls.array.size(); ++i) {
                const json::Value& c = calls.array[i];
                const int index = c["index"].isNumber() ? int(c["index"].number) : int(i);
                std::size_t slot = 0;
                while (slot < m_index.size() && m_index[slot] != index) ++slot;
                // A whole message (not streamed) lists each call once, even without an index.
                if (!streamed) slot = m_index.size();
                const bool fresh = slot == m_index.size();
                if (fresh) {
                    m_index.push_back(index);
                    m_result.toolCalls.emplace_back();
                }
                ToolCall& call = m_result.toolCalls[slot];
                if (call.id.empty()) call.id = c["id"].stringOr("");
                const json::Value& fn = c["function"];
                if (call.name.empty()) call.name = fn["name"].stringOr("");
                const json::Value& args = fn["arguments"];
                if (args.type == json::Value::Type::String) {
                    call.arguments += args.string;
                } else if (args.isObject()) {
                    call.arguments += writeCompactJson(args); // a server that sends the object itself
                }
                if (fresh && onToolCall) onToolCall(call);
            }
        }
    }
    if (choice["finish_reason"].type == json::Value::Type::String) {
        m_result.finishReason = choice["finish_reason"].string;
        if (m_result.finishReason == "error" && !m_result.failure.failed()) {
            m_result.failure = {ErrorKind::StreamError, 0, "the provider ended the answer with an error"};
        }
    }
}

void ChatStreamParser::finish() {
    if (m_finished) return;
    if (m_mode == 1) {
        if (!m_line.empty()) {
            std::string last = std::move(m_line);
            m_line.clear();
            if (!last.empty() && last.back() == '\r') last.pop_back();
            line(last);
        }
        event();
    } else if (m_mode == 2) {
        Result<json::Value> value = json::parse(m_whole);
        if (!value) {
            m_result.failure = {ErrorKind::BadResponse, 0, std::format("the answer is not JSON ({})", value.error())};
        } else {
            chunk(*value);
            if (!value->has("choices") && !m_result.failure.failed()) {
                m_result.failure = {ErrorKind::BadResponse, 0, "the answer has no choices"};
            }
        }
    }
    m_finished = true;
    // The calls in the order of their index.
    std::vector<std::size_t> order(m_index.size());
    std::iota(order.begin(), order.end(), std::size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return m_index[a] < m_index[b]; });
    std::vector<ToolCall> sorted;
    sorted.reserve(order.size());
    for (const std::size_t i : order) sorted.push_back(std::move(m_result.toolCalls[i]));
    m_result.toolCalls = std::move(sorted);
    if (m_result.failure.failed()) return;
    if (m_mode == 0) {
        m_result.failure = {ErrorKind::BadResponse, 0, "the answer is empty"};
    } else if (m_mode == 1 && !m_result.sawDone && m_result.finishReason.empty()) {
        m_result.failure = {ErrorKind::StreamError, 0, "the answer ended before it was complete"};
    }
}

// ---------------------------------------------------------------------------------------------------- the request

namespace {

std::string endpoint(const ProviderSettings& s, std::string_view path) {
    std::string base = s.baseUrl;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + std::string(path);
}

} // namespace

namespace detail {

net::Headers requestHeaders(const ProviderSettings& s, std::string contentType) {
    net::Headers headers;
    headers.push_back({"Content-Type", std::move(contentType)});
    if (!s.apiKey.empty()) headers.push_back({"Authorization", "Bearer " + s.apiKey});
    for (const net::Header& h : s.extraHeaders) headers.push_back(h);
    return headers;
}

std::string endpointUrl(const ProviderSettings& s, std::string_view path) { return endpoint(s, path); }

} // namespace detail

net::Request buildChatRequest(const ProviderSettings& s, const json::Value& messages, const json::Value& tools,
                              const ChatOptions& options) {
    json::Value body = json::Value::makeObject();
    auto& o = body.object;
    o["model"] = json::Value::makeString(s.chatModel);
    o["messages"] = messages;
    if (tools.isArray() && !tools.array.empty()) {
        o["tools"] = tools;
        if (!options.forceTool.empty()) {
            json::Value fn = json::Value::makeObject();
            fn.object["name"] = json::Value::makeString(options.forceTool);
            json::Value choice = json::Value::makeObject();
            choice.object["type"] = json::Value::makeString("function");
            choice.object["function"] = std::move(fn);
            o["tool_choice"] = std::move(choice);
        } else {
            o["tool_choice"] = json::Value::makeString(options.toolChoice == ToolChoice::None       ? "none"
                                                       : options.toolChoice == ToolChoice::Required ? "required"
                                                                                                    : "auto");
        }
        o["parallel_tool_calls"] = json::Value::makeBool(s.parallelToolCalls);
    }
    o["stream"] = json::Value::makeBool(s.streamChat);
    if (s.streamChat && s.streamUsage) {
        json::Value so = json::Value::makeObject();
        so.object["include_usage"] = json::Value::makeBool(true);
        o["stream_options"] = std::move(so);
    }
    if (s.maxTokens > 0) o["max_tokens"] = json::Value::makeNumber(s.maxTokens);
    if (s.temperature >= 0.0) o["temperature"] = json::Value::makeNumber(s.temperature);

    net::Request r;
    r.method = "POST";
    r.url = endpoint(s, "/chat/completions");
    r.headers = detail::requestHeaders(s, "application/json");
    if (s.streamChat) r.headers.push_back({"Accept", "text/event-stream"});
    r.setBody(writeCompactJson(body));
    r.timeoutSeconds = s.chatTimeout;
    r.streamBody = s.streamChat;
    return r;
}

} // namespace quench::assistant
