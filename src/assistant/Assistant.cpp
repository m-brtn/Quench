// The assistant's operations: a turn of chat rounds and tool calls, transcription and speech
// (quench/assistant/Assistant.h).

#include <quench/assistant/Assistant.h>

#include "assistant/Text.h"

#include <quench/core/Log.h>
#include <quench/net/FakeNetwork.h>

#include <chrono>
#include <format>
#include <utility>

namespace quench::assistant {
namespace {

constexpr const char* kLog = "assistant";
constexpr std::size_t kMaxErrorBody = 16 * 1024;

json::Value message(const char* role, std::string content) {
    json::Value m = json::Value::makeObject();
    m.object["role"] = json::Value::makeString(role);
    m.object["content"] = json::Value::makeString(std::move(content));
    return m;
}

std::string costText(const Usage& usage) {
    return usage.costKnown ? std::format(", ${:.6f}", usage.cost) : std::string();
}

// "120+30 tokens (64 cached)", "(64 cached, 32 written)" when the provider wrote to its cache.
std::string tokenText(const Usage& usage) {
    std::string out =
        std::format("{}+{} tokens ({} cached", usage.promptTokens, usage.completionTokens, usage.cachedTokens);
    if (usage.cacheWriteTokens > 0) out += std::format(", {} written", usage.cacheWriteTokens);
    return out + ")";
}

// The estimated tokens of a chat message: its text, its tool calls' names and arguments, and 4 for its frame.
std::int64_t messageTokens(const json::Value& m) {
    std::int64_t n = 4;
    const json::Value& content = m["content"];
    if (content.type == json::Value::Type::String) {
        n += estimateTokens(content.string);
    } else if (content.isArray()) {
        for (const json::Value& part : content.array) n += estimateTokens(part["text"].stringOr(""));
    }
    for (const json::Value& call : m["tool_calls"].array) {
        n += estimateTokens(call["function"]["name"].stringOr("")) +
             estimateTokens(call["function"]["arguments"].stringOr(""));
    }
    return n;
}

std::int64_t turnTokens(const std::vector<json::Value>& turn) {
    std::int64_t n = 0;
    for (const json::Value& m : turn) n += messageTokens(m);
    return n;
}

// The first line of a text, at most `limit` characters.
std::string firstLine(std::string_view text, std::size_t limit) {
    text = detail::trimSpace(text);
    const std::size_t end = text.find('\n');
    return detail::trimText(detail::trimSpace(text.substr(0, end)), limit);
}

int ms(double seconds) { return int(seconds * 1000.0 + 0.5); }

// The seconds of a 16-bit PCM WAV file (the microphone's recording), 0 when it is not one.
double wavSeconds(std::span<const std::uint8_t> wav) {
    if (wav.size() < 44 || std::string_view(reinterpret_cast<const char*>(wav.data()), 4) != "RIFF") return 0.0;
    const auto u32 = [&](std::size_t at) {
        return std::uint32_t(wav[at]) | (std::uint32_t(wav[at + 1]) << 8) | (std::uint32_t(wav[at + 2]) << 16) |
               (std::uint32_t(wav[at + 3]) << 24);
    };
    const std::uint32_t byteRate = u32(28);
    return byteRate ? double(wav.size() - 44) / byteRate : 0.0;
}

} // namespace

std::int64_t estimateTokens(std::string_view text) { return std::int64_t((detail::codePoints(text) + 2) / 3); }

const char* stateName(State state) {
    switch (state) {
    case State::Idle: return "idle";
    case State::Transcribing: return "transcribing";
    case State::Thinking: return "thinking";
    case State::CallingTool: return "calling tool";
    case State::Speaking: return "speaking";
    case State::Done: return "done";
    case State::Failed: return "failed";
    }
    return "?";
}

void Stats::add(const Stats& o) {
    requests += o.requests;
    rounds += o.rounds;
    toolCalls += o.toolCalls;
    usage.add(o.usage);
    audioSeconds += o.audioSeconds;
    spokenChars += o.spokenChars;
    speechSeconds += o.speechSeconds;
    transcribeTime += o.transcribeTime;
    thinkTime += o.thinkTime;
    toolTime += o.toolTime;
    speakTime += o.speakTime;
    totalTime += o.totalTime;
    refusedCalls += o.refusedCalls;
}

Assistant::Assistant(const AssistantDesc& desc) : m_alive(std::make_shared<int>(0)) {
    if (desc.clock) {
        m_clock = desc.clock;
    } else {
        const auto origin = std::chrono::steady_clock::now();
        m_clock = [origin] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count(); };
    }
    if (desc.client) {
        m_client = desc.client;
    } else {
        m_ownClient = std::make_unique<net::Client>(net::ClientDesc{.fake = desc.fake, .clock = m_clock});
        m_client = m_ownClient.get();
    }
    m_settings = openRouterPreset();
}

Assistant::~Assistant() {
    // A shared client calls the cancelled request's onDone later: m_alive's end tells it that nobody listens.
    if (m_request && !m_ownClient) m_client->cancel(m_request);
    m_alive.reset();
}

double Assistant::now() const { return m_clock(); }

bool Assistant::busy() const {
    return m_state == State::Transcribing || m_state == State::Thinking || m_state == State::CallingTool ||
           m_state == State::Speaking;
}

void Assistant::setState(State state) {
    if (state == m_state) return;
    m_state = state;
    m_events.push_back({.kind = EventKind::StateChanged, .state = state});
}

std::vector<Event> Assistant::takeEvents() { return std::exchange(m_events, {}); }

std::optional<SpeechAudio> Assistant::takeSpeech() { return std::exchange(m_speech, std::nullopt); }

Result<void> Assistant::begin(Op op) {
    if (busy()) return Error{std::format("busy: the assistant is {}; cancel() it first", stateName(m_state))};
    if (m_settings.baseUrl.empty()) return Error{"not configured: no base URL"};
    m_op = op;
    m_failure = {};
    m_toolName.clear();
    m_transcript.clear();
    m_partial.clear();
    m_answer.clear();
    m_calls.clear();
    m_speech.reset();
    m_stats = {};
    m_turn.clear();
    m_round = 0;
    m_actions = 0;
    m_opStart = now();
    return {};
}

Result<void> Assistant::ask(std::string text, AskOptions options) {
    if (busy()) return begin(Op::Ask);
    if (m_settings.chatModel.empty()) return Error{"not configured: no chat model"};
    if (detail::trimSpace(text).empty()) return Error{"nothing to ask: the text is empty"};
    if (options.speak && m_settings.speechModel.empty()) return Error{"not configured: no speech model"};
    if (Result<void> r = begin(Op::Ask); !r) return r;
    m_options = options;
    m_voice = false;
    startTurn(std::string(detail::trimSpace(text)));
    return {};
}

Result<void> Assistant::askByVoice(std::span<const std::uint8_t> wav, AskOptions options) {
    if (busy()) return begin(Op::Ask);
    if (m_settings.chatModel.empty()) return Error{"not configured: no chat model"};
    if (m_settings.transcriptionModel.empty()) return Error{"not configured: no transcription model"};
    if (options.speak && m_settings.speechModel.empty()) return Error{"not configured: no speech model"};
    if (wav.empty()) return Error{"nothing to transcribe: the recording is empty"};
    if (Result<void> r = begin(Op::Ask); !r) return r;
    m_options = options;
    m_voice = true;
    sendTranscription(std::vector<std::uint8_t>(wav.begin(), wav.end()));
    return {};
}

Result<void> Assistant::transcribe(std::span<const std::uint8_t> wav) {
    if (busy()) return begin(Op::Transcribe);
    if (m_settings.transcriptionModel.empty()) return Error{"not configured: no transcription model"};
    if (wav.empty()) return Error{"nothing to transcribe: the recording is empty"};
    if (Result<void> r = begin(Op::Transcribe); !r) return r;
    m_voice = false;
    sendTranscription(std::vector<std::uint8_t>(wav.begin(), wav.end()));
    return {};
}

Result<void> Assistant::speak(std::string text) {
    if (busy()) return begin(Op::Speak);
    if (m_settings.speechModel.empty()) return Error{"not configured: no speech model"};
    if (detail::trimSpace(text).empty()) return Error{"nothing to say: the text is empty"};
    if (Result<void> r = begin(Op::Speak); !r) return r;
    m_answer = std::string(detail::trimSpace(text));
    sendSpeech(m_answer);
    return {};
}

void Assistant::cancel() {
    if (!busy()) return;
    ++m_serial;
    if (m_request) m_client->cancel(m_request);
    m_request = 0;
    m_failure = {ErrorKind::Cancelled, 0, "cancelled"};
    m_stats.totalTime = now() - m_opStart;
    m_totals.add(m_stats);
    m_op = Op::None;
    m_toolName.clear();
    m_turn.clear();
    log::info(kLog, "cancelled while {}", stateName(m_state));
    setState(State::Idle);
    m_events.push_back({.kind = EventKind::Cancelled, .state = State::Idle});
}

void Assistant::fail(Failure f) {
    f.message = redact(f.message, m_settings.apiKey);
    log::warning(kLog, "{} failed: {}", stateName(m_state), f.message);
    ++m_serial;
    if (m_request) m_client->cancel(m_request);
    m_request = 0;
    m_failure = std::move(f);
    m_stats.totalTime = now() - m_opStart;
    m_totals.add(m_stats);
    m_op = Op::None;
    m_toolName.clear();
    m_turn.clear();
    setState(State::Failed);
    m_events.push_back({.kind = EventKind::Failed, .state = State::Failed, .text = m_failure.message});
}

void Assistant::finishOperation() {
    m_stats.totalTime = now() - m_opStart;
    m_totals.add(m_stats);
    m_op = Op::None;
    m_toolName.clear();
    log::info(kLog, "done: {} requests, {} rounds, {} tool calls, {}{}, {} ms", m_stats.requests, m_stats.rounds,
              m_stats.toolCalls, tokenText(m_stats.usage), costText(m_stats.usage), ms(m_stats.totalTime));
    setState(State::Done);
}

bool Assistant::sendRequest(net::Request request, net::Handlers handlers) {
    const std::uint64_t serial = ++m_serial;
    const std::weak_ptr<int> alive = m_alive;
    const auto current = [this, serial, alive] { return !alive.expired() && serial == m_serial; };
    net::Handlers guarded;
    if (handlers.onHeaders) {
        guarded.onHeaders = [current, fn = std::move(handlers.onHeaders)](const net::Response& r) {
            if (current()) fn(r);
        };
    }
    if (handlers.onChunk) {
        guarded.onChunk = [current, fn = std::move(handlers.onChunk)](std::span<const std::uint8_t> c) {
            if (current()) fn(c);
        };
    }
    guarded.onDone = [this, current, fn = std::move(handlers.onDone)](const net::Response& r) {
        if (!current()) return;
        m_request = 0;
        fn(r);
    };
    m_status = 0;
    m_errorBody.clear();
    const std::string url = request.url;
    Result<net::RequestId> id = m_client->send(std::move(request), std::move(guarded));
    ++m_stats.requests;
    if (!id) {
        ErrorKind kind = ErrorKind::BadRequest;
        if (!m_client->available()) {
            kind = ErrorKind::NoBackend;
        } else if (!net::checkUrl(url)) {
            kind = ErrorKind::NotAllowed;
        }
        fail({kind, 0, id.error()});
        return false;
    }
    m_request = *id;
    return true;
}

// ---------------------------------------------------------------------------------------------------- the turn

void Assistant::startTurn(std::string text) {
    m_userText = std::move(text);
    m_turn.clear();
    m_round = 0;
    m_turnState = m_stateSummary ? m_stateSummary() : std::string();
    sendChat();
}

int Assistant::roundLimit() const { return m_options.maxRounds > 0 ? m_options.maxRounds : m_limits.maxRounds; }

std::string Assistant::inputText() const {
    return m_options.source == InputSource::Event ? m_eventHeading + " " + m_userText : m_userText;
}

json::Value Assistant::messages() const {
    // The static part first (the system prompt; the tools are beside it in the body), then the history, which changes
    // only between turns, then this turn's input with its state summary (fresh each turn) and this turn's rounds.
    json::Value list = json::Value::makeArray();
    if (!m_systemPrompt.empty()) list.array.push_back(message("system", m_systemPrompt));
    for (const std::vector<json::Value>& turn : m_history) {
        for (const json::Value& m : turn) list.array.push_back(m);
    }
    const std::string_view state = detail::trimSpace(m_turnState);
    const bool event = m_options.source == InputSource::Event;
    std::string input;
    if (state.empty()) {
        input = inputText();
    } else {
        input = std::format("Game state:\n{}\n\n{} {}", state, event ? m_eventHeading : "Player:", m_userText);
    }
    list.array.push_back(message("user", std::move(input)));
    for (const json::Value& m : m_turn) list.array.push_back(m);
    return list;
}

void Assistant::sendChat() {
    ++m_round;
    m_partial.clear();
    m_parser = std::make_unique<ChatStreamParser>();
    m_parser->onText = [this](std::string_view delta) {
        m_partial += delta;
        if (m_stats.firstTextTime < 0.0) m_stats.firstTextTime = now() - m_opStart;
    };
    ChatOptions options;
    if (m_limits.answerOnLastRound && m_round >= roundLimit()) options.toolChoice = ToolChoice::None;
    net::Request request = buildChatRequest(m_settings, messages(), m_tools.chatTools(), options);
    setState(State::Thinking);
    m_stageStart = now();
    sendRequest(std::move(request), {
                                        .onHeaders = [this](const net::Response& head) { m_status = head.status; },
                                        .onChunk =
                                            [this](std::span<const std::uint8_t> piece) {
                                                if (m_status >= 200 && m_status < 300) {
                                                    m_parser->feed(piece);
                                                } else if (m_errorBody.size() < kMaxErrorBody) {
                                                    m_errorBody.append(reinterpret_cast<const char*>(piece.data()),
                                                                       piece.size());
                                                }
                                            },
                                        .onDone = [this](const net::Response& r) { chatDone(r); },
                                    });
}

void Assistant::chatDone(const net::Response& r) {
    m_stats.thinkTime += now() - m_stageStart;
    if (!r.body.empty()) {
        if (r.success()) {
            m_parser->feed(r.body);
        } else {
            m_errorBody.assign(r.text().substr(0, kMaxErrorBody));
        }
    }
    Failure f = failureFromResponse(r, m_errorBody, m_settings.apiKey);
    ++m_stats.rounds;
    if (!f.failed()) {
        m_parser->finish();
        f = m_parser->result().failure;
    }
    if (f.failed()) {
        fail(std::move(f));
        return;
    }
    const ChatResult& result = m_parser->result();
    m_stats.usage.add(result.usage);
    log::info(kLog, "round {}: {}, {} characters, {} tool calls, {}{}, {} ms{}", m_round, r.status,
              detail::codePoints(result.text), result.toolCalls.size(), tokenText(result.usage), costText(result.usage),
              ms(r.seconds), result.model.empty() ? std::string() : " (" + result.model + ")");

    if (!result.toolCalls.empty() && m_round < roundLimit()) {
        json::Value assistant = json::Value::makeObject();
        assistant.object["role"] = json::Value::makeString("assistant");
        assistant.object["content"] = result.text.empty() ? json::Value() : json::Value::makeString(result.text);
        json::Value calls = json::Value::makeArray();
        m_nextTool = m_calls.size();
        for (std::size_t i = 0; i < result.toolCalls.size(); ++i) {
            const ToolCall& c = result.toolCalls[i];
            ToolCallRecord record;
            record.id = c.id.empty() ? std::format("call_{}_{}", m_round, i) : c.id;
            record.name = c.name;
            record.arguments = std::string(detail::trimSpace(c.arguments)).empty() ? "{}" : c.arguments;
            record.round = m_round;
            json::Value fn = json::Value::makeObject();
            fn.object["name"] = json::Value::makeString(record.name);
            fn.object["arguments"] = json::Value::makeString(record.arguments);
            json::Value call = json::Value::makeObject();
            call.object["id"] = json::Value::makeString(record.id);
            call.object["type"] = json::Value::makeString("function");
            call.object["function"] = std::move(fn);
            calls.array.push_back(std::move(call));
            m_calls.push_back(std::move(record));
        }
        assistant.object["tool_calls"] = std::move(calls);
        m_turn.push_back(std::move(assistant));
        m_stageStart = now();
        announceTool();
        return;
    }
    const std::string text(detail::trimSpace(result.text));
    if (text.empty()) {
        if (!result.toolCalls.empty()) {
            fail({ErrorKind::RoundLimit, 0,
                  std::format("the model still asked for tools after {} rounds", roundLimit())});
        } else {
            fail({ErrorKind::BadResponse, 0, "the model answered with no text"});
        }
        return;
    }
    m_answer = text;
    m_turn.push_back(message("assistant", m_answer));
    commitTurn();
    m_events.push_back({.kind = EventKind::Answered, .state = m_state, .text = m_answer});
    if (m_options.speak) {
        sendSpeech(m_answer);
    } else {
        finishOperation();
    }
}

void Assistant::announceTool() {
    const ToolCallRecord& record = m_calls[m_nextTool];
    m_toolName = record.name;
    // Visible for at least one update(): inside update() the next one runs it; outside (a shared client updated
    // by the game) the one after the next.
    m_toolAnnounced = m_inUpdate ? m_updates : m_updates + 1;
    setState(State::CallingTool);
    m_events.push_back({.kind = EventKind::ToolStarted, .state = State::CallingTool, .tool = m_nextTool});
}

void Assistant::stepTools() {
    if (m_state != State::CallingTool || m_updates <= m_toolAnnounced) return;
    const std::uint64_t serial = m_serial;
    const std::size_t index = m_nextTool;
    const std::string name = m_calls[index].name;
    const std::string arguments = m_calls[index].arguments;
    const double start = now();
    // A tool not marked read-only is an action; over this turn's cap it is refused, not run.
    const Tool* found = m_tools.find(name);
    const bool action = found && !found->annotations.readOnly.value_or(false);
    const bool refused = action && m_options.maxActions >= 0 && m_actions >= m_options.maxActions;
    ToolResult result;
    if (refused) {
        result = ToolResult::error(std::format("not run: this turn may make at most {} change{} to the game "
                                               "(tools that are not read-only), and it has; read-only tools still "
                                               "work. Answer with what you know.",
                                               m_options.maxActions, m_options.maxActions == 1 ? "" : "s"));
    } else {
        if (action) ++m_actions;
        result = m_tools.callWithText(name, arguments);
    }
    const double seconds = now() - start;
    if (serial != m_serial || m_state != State::CallingTool) return; // the handler cancelled or started something
    ToolCallRecord& record = m_calls[index];
    record.result = std::move(result);
    record.finished = true;
    record.seconds = seconds;
    ++m_stats.toolCalls;
    if (refused) ++m_stats.refusedCalls;
    m_stats.toolTime += seconds;
    std::string content = detail::trimText(record.result.contentText(), m_limits.toolResultChars);
    if (record.result.isError) content = "Error: " + content;
    json::Value tool = json::Value::makeObject();
    tool.object["role"] = json::Value::makeString("tool");
    tool.object["tool_call_id"] = json::Value::makeString(record.id);
    tool.object["content"] = json::Value::makeString(std::move(content));
    m_turn.push_back(std::move(tool));
    log::info(kLog, "tool {}: {}, {:.2f} ms", record.name,
              refused                 ? "refused (the turn's action cap)"
              : record.result.isError ? "error"
                                      : "ok",
              seconds * 1000.0);
    m_events.push_back({.kind = EventKind::ToolFinished, .state = State::CallingTool, .tool = index});
    ++m_nextTool;
    if (m_nextTool < m_calls.size()) {
        announceTool();
        return;
    }
    m_toolName.clear();
    sendChat();
}

void Assistant::commitTurn() {
    if (m_limits.historyTurns == 0) {
        m_history.clear();
        m_turn.clear();
        return;
    }
    // The newest turn as it was sent (its tool results cut at toolResultChars only); compactHistory() cuts it further
    // once a newer one follows.
    std::vector<json::Value> turn;
    turn.reserve(m_turn.size() + 1);
    turn.push_back(message("user", inputText()));
    for (json::Value& m : m_turn) turn.push_back(std::move(m));
    m_turn.clear();
    m_history.push_back(std::move(turn));
    compactHistory();
}

bool Assistant::isEventTurn(const std::vector<json::Value>& turn) const {
    if (turn.empty() || m_eventHeading.empty()) return false;
    const std::string content = turn.front()["content"].stringOr("");
    return content.starts_with(m_eventHeading);
}

void Assistant::compactHistory() {
    if (m_limits.historyTurns == 0) {
        m_history.clear();
        return;
    }
    if (m_limits.historyTurns > 0) {
        while (m_history.size() > std::size_t(m_limits.historyTurns)) m_history.erase(m_history.begin());
    }
    // Every turn but the newest: tool results cut to historyToolResultChars (the count of what is left out stays the
    // whole result's), old event turns down to their marked first line and their answer. Both are idempotent, so a
    // turn's messages change once, when it stops being the newest (or the old event turn), and not again.
    const std::size_t n = m_history.size();
    for (std::size_t i = 0; i + 1 < n; ++i) {
        std::vector<json::Value>& turn = m_history[i];
        const std::size_t age = n - 1 - i;
        if (isEventTurn(turn) && m_limits.historyEventTurns >= 0 && age >= std::size_t(m_limits.historyEventTurns)) {
            std::vector<json::Value> compact;
            compact.push_back(message("user", firstLine(turn.front()["content"].stringOr(""), 300)));
            for (std::size_t k = turn.size(); k-- > 1;) {
                const json::Value& m = turn[k];
                if (m["role"].stringOr("") == "assistant" && m["content"].type == json::Value::Type::String &&
                    !m["tool_calls"].isArray()) {
                    compact.push_back(m);
                    break;
                }
            }
            turn = std::move(compact);
            continue;
        }
        for (json::Value& m : turn) {
            if (m["role"].stringOr("") != "tool") continue;
            m.object["content"] =
                json::Value::makeString(detail::retrimText(m["content"].stringOr(""), m_limits.historyToolResultChars));
        }
    }
    // The budget: the oldest turns go first; the newest stays whatever its size.
    if (m_limits.historyTokens > 0) {
        std::int64_t total = historyTokenEstimate();
        while (m_history.size() > 1 && total > m_limits.historyTokens) {
            total -= turnTokens(m_history.front());
            m_history.erase(m_history.begin());
        }
    }
}

std::int64_t Assistant::historyTokenEstimate() const {
    std::int64_t n = 0;
    for (const std::vector<json::Value>& turn : m_history) n += turnTokens(turn);
    return n;
}

// ---------------------------------------------------------------------------------------------------- speech

void Assistant::sendTranscription(std::vector<std::uint8_t> wav) {
    setState(State::Transcribing);
    m_stageStart = now();
    m_stats.audioSeconds += wavSeconds(wav);
    net::Request request = buildTranscriptionRequest(m_settings, wav);
    sendRequest(std::move(request), {.onDone = [this](const net::Response& r) { transcriptionDone(r); }});
}

void Assistant::transcriptionDone(const net::Response& r) {
    m_stats.transcribeTime += now() - m_stageStart;
    Failure f = failureFromResponse(r, r.text(), m_settings.apiKey);
    if (f.failed()) {
        fail(std::move(f));
        return;
    }
    Result<Transcript> t = parseTranscription(r.text());
    if (!t) {
        fail({ErrorKind::BadResponse, r.status, t.error()});
        return;
    }
    m_stats.usage.add(t->usage);
    m_transcript = t->text;
    log::info(kLog, "transcription: {}, {} characters{}, {} ms", r.status, detail::codePoints(m_transcript),
              costText(t->usage), ms(r.seconds));
    m_events.push_back({.kind = EventKind::Transcribed, .state = m_state, .text = m_transcript});
    if (!m_voice) {
        finishOperation();
        return;
    }
    if (m_transcript.empty()) {
        fail({ErrorKind::NothingHeard, 0, "nothing was heard in the recording"});
        return;
    }
    startTurn(m_transcript);
}

void Assistant::sendSpeech(const std::string& text) {
    setState(State::Speaking);
    m_stageStart = now();
    m_stats.spokenChars += std::int64_t(detail::codePoints(speechInput(m_settings, text)));
    net::Request request = buildSpeechRequest(m_settings, text);
    sendRequest(std::move(request), {.onDone = [this](const net::Response& r) { speechDone(r); }});
}

void Assistant::speechDone(const net::Response& r) {
    m_stats.speakTime += now() - m_stageStart;
    Failure f = failureFromResponse(r, r.text(), m_settings.apiKey);
    if (f.failed()) {
        fail(std::move(f));
        return;
    }
    const std::string* type = r.header("Content-Type");
    Result<SpeechAudio> audio =
        decodeSpeech(r.body, type ? *type : std::string(), m_settings.speechFormat, m_settings.pcmRate);
    if (!audio) {
        fail({ErrorKind::BadResponse, r.status, audio.error()});
        return;
    }
    m_stats.speechSeconds += audio->seconds();
    log::info(kLog, "speech: {}, {:.2f} s at {} Hz, {} bytes, {} ms", r.status, audio->seconds(), audio->sampleRate,
              r.bytes, ms(r.seconds));
    m_speech = std::move(*audio);
    m_events.push_back({.kind = EventKind::SpeechReady, .state = m_state});
    finishOperation();
}

// ---------------------------------------------------------------------------------------------------- the frame

void Assistant::update() {
    ++m_updates;
    m_inUpdate = true;
    if (m_ownClient) m_ownClient->update();
    stepTools();
    m_inUpdate = false;
}

// ---------------------------------------------------------------------------------------------------- history

json::Value Assistant::historyJson() const {
    json::Value turns = json::Value::makeArray();
    for (const std::vector<json::Value>& turn : m_history) {
        json::Value t = json::Value::makeArray();
        t.array = turn;
        turns.array.push_back(std::move(t));
    }
    json::Value v = json::Value::makeObject();
    v.object["turns"] = std::move(turns);
    return v;
}

Result<void> Assistant::setHistory(const json::Value& history) {
    if (busy()) return Error{"busy: the history cannot change while a turn runs"};
    const json::Value& turns = history["turns"];
    if (!turns.isArray()) return Error{"the history has no \"turns\" array"};
    std::vector<std::vector<json::Value>> loaded;
    for (const json::Value& turn : turns.array) {
        if (!turn.isArray() || turn.array.empty()) return Error{"a turn of the history is not a list of messages"};
        for (const json::Value& m : turn.array) {
            const std::string role = m["role"].stringOr("");
            if (role != "user" && role != "assistant" && role != "tool") {
                return Error{std::format("a message of the history has the role '{}'", detail::trimText(role, 20))};
            }
        }
        if (turn.array.front()["role"].stringOr("") != "user")
            return Error{"a turn of the history starts without the player"};
        loaded.push_back(turn.array);
    }
    m_history = std::move(loaded);
    compactHistory(); // a save from before the budget loads the same way: its turns are plain player turns
    return {};
}

} // namespace quench::assistant
