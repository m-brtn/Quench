// assistant::LiveSession (quench/assistant/Live.h): GPT-Live's primary WebSocket. Every event name and field here
// is taken from OpenAI's documentation as read on 2026-11-14 (developers.openai.com/api/docs/guides/live,
// live-delegation, live-conversations, voice-websockets?api=live and the reference
// api/reference/resources/live/primary-websocket); where the documentation is silent, a comment says so.

#include <quench/assistant/Live.h>

#include <quench/assistant/Chat.h>   // writeCompactJson
#include <quench/assistant/Speech.h> // base64

#include "assistant/Text.h"

#include <quench/core/Log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <utility>

namespace quench::assistant {
namespace {

constexpr const char* kLog = "assistant";
constexpr float kAudiblePeak = 0.002f; // an output audio piece above this peak is the station speaking

json::Value str(std::string_view s) { return json::Value::makeString(std::string(s)); }
json::Value num(double v) { return json::Value::makeNumber(v); }

json::Value textMessage(const char* role, const char* contentType, std::string text) {
    json::Value part = json::Value::makeObject();
    part.object["type"] = str(contentType);
    part.object["text"] = json::Value::makeString(std::move(text));
    json::Value m = json::Value::makeObject();
    m.object["type"] = str("message");
    m.object["role"] = str(role);
    m.object["content"] = json::Value::makeArray();
    m.object["content"].array.push_back(std::move(part));
    return m;
}

LiveCloseReason reasonFromServer(const std::string& reason) {
    if (reason == "close_requested") return LiveCloseReason::Requested;
    if (reason == "expired") return LiveCloseReason::Expired;
    if (reason == "content") return LiveCloseReason::Content;
    if (reason == "remote_hangup") return LiveCloseReason::RemoteHangup;
    if (reason == "connection_lost") return LiveCloseReason::ConnectionLost;
    return LiveCloseReason::ConnectionLost; // an unknown reason: the docs list only these five
}

// The kind of a server error event before session.started (the session cannot run), by its code and type. The
// documentation lists codes only as examples (immutable_field_update, unknown_parameter, responses_validation_failed,
// tool_outputs_required); these are guesses where it is silent.
ErrorKind kindOfStartError(const std::string& code, const std::string& type) {
    if (code.find("api_key") != std::string::npos || code == "invalid_api_key" || type == "authentication_error")
        return ErrorKind::BadKey;
    if (code.find("quota") != std::string::npos || code.find("billing") != std::string::npos)
        return ErrorKind::NoCredits;
    if (code.find("model") != std::string::npos || code.find("permission") != std::string::npos ||
        type == "permission_error")
        return ErrorKind::Refused;
    if (code.find("rate_limit") != std::string::npos) return ErrorKind::RateLimited;
    if (type == "server_error") return ErrorKind::ProviderDown;
    return ErrorKind::BadRequest;
}

} // namespace

const char* liveStateName(LiveState state) {
    switch (state) {
    case LiveState::Idle: return "idle";
    case LiveState::Connecting: return "connecting";
    case LiveState::Starting: return "starting";
    case LiveState::Listening: return "listening";
    case LiveState::Speaking: return "speaking";
    case LiveState::CallingTool: return "calling a tool";
    case LiveState::Closing: return "closing";
    case LiveState::Closed: return "closed";
    case LiveState::Failed: return "failed";
    }
    return "?";
}

const char* liveCloseReasonName(LiveCloseReason reason) {
    switch (reason) {
    case LiveCloseReason::None: return "none";
    case LiveCloseReason::Requested: return "requested";
    case LiveCloseReason::Idle: return "idle";
    case LiveCloseReason::Expired: return "expired";
    case LiveCloseReason::Content: return "content";
    case LiveCloseReason::RemoteHangup: return "remote hangup";
    case LiveCloseReason::ConnectionLost: return "connection lost";
    case LiveCloseReason::Cancelled: return "cancelled";
    }
    return "?";
}

LiveSession::LiveSession(const LiveDesc& desc) : m_clock(desc.clock), m_unixClock(desc.unixClock) {
    if (!m_clock) {
        const auto origin = std::chrono::steady_clock::now();
        m_clock = [origin] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count(); };
    }
    if (!m_unixClock) {
        m_unixClock = [] {
            return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        };
    }
    m_fake = desc.fake;
    m_ws = std::make_unique<net::WebSocket>(net::WebSocketDesc{.fake = m_fake, .clock = m_clock});
}

LiveSession::~LiveSession() = default;

double LiveSession::now() const { return m_clock(); }

void LiveSession::note(std::string_view what) const { log::info(kLog, "live: {}", redact(what, m_settings.apiKey)); }

bool LiveSession::running() const {
    return m_state == LiveState::Connecting || m_state == LiveState::Starting || m_state == LiveState::Listening ||
           m_state == LiveState::Speaking || m_state == LiveState::CallingTool || m_state == LiveState::Closing;
}

bool LiveSession::gateClosed() const {
    return m_settings.uplinkGate && (m_playback || now() - m_lastPlayback < m_settings.gateHoldSeconds);
}

void LiveSession::setState(LiveState state) {
    if (state == m_state) return;
    m_state = state;
    m_stateTime = now();
    m_events.push_back({.kind = LiveEventKind::StateChanged, .state = state});
}

void LiveSession::noteActivity() { m_lastActivity = now(); }

std::string LiveSession::nextEventId(std::string_view prefix) {
    return std::format("{}_{}_{}", prefix, m_session, ++m_eventSerial);
}

void LiveSession::sendJson(const json::Value& event) {
    if (!m_ws->open()) return;
    (void)m_ws->sendText(writeCompactJson(event));
}

std::string LiveSession::fullInstructions() const {
    std::string text = m_settings.instructions;
    if (m_settings.describeTools && m_tools.size() > 0) {
        // The documentation's template for gpt-live-1 (live-prompting, "Recommended prompt structure").
        text += text.empty() ? "" : "\n\n";
        text += "Delegation policy:\nBackend tools:\n";
        for (const Tool& t : m_tools.tools()) text += "- " + t.name + ": " + t.description + "\n";
        text += "\nGenerate <|SpawnThinking|> when:\n- The request needs one of the backend tools, or the current "
                "state of the game.\n- A correction changes the work already requested.\n\nDo not generate "
                "<|SpawnThinking|> when:\n- You can answer from the conversation or a still-current result.\n\n"
                "Generate <|SpawnThinking|> before giving an answer that depends on backend work.\nDo not guess the "
                "result while waiting.";
    }
    return text;
}

json::Value LiveSession::inputItems() const {
    json::Value input = json::Value::makeArray();
    if (m_stateSummary) {
        if (std::string summary = m_stateSummary(); !summary.empty())
            input.array.push_back(textMessage("developer", "input_text", std::move(summary)));
    }
    // The newest turns within the budget, consecutive turns of one speaker merged; at most 128 messages in all.
    std::vector<std::pair<LiveSpeaker, std::string>> merged;
    for (const LiveTurn& t : m_history) {
        const std::string_view text = detail::trimSpace(t.text);
        if (text.empty()) continue;
        if (!merged.empty() && merged.back().first == t.speaker)
            merged.back().second += " " + std::string(text);
        else
            merged.emplace_back(t.speaker, std::string(text));
    }
    std::size_t chars = 0, first = merged.size();
    const std::size_t maxMessages = 128 - input.array.size();
    while (first > 0 && merged.size() - first < maxMessages) {
        const std::size_t size = merged[first - 1].second.size();
        if (chars + size > m_settings.historyChars) break;
        chars += size;
        --first;
    }
    for (std::size_t i = first; i < merged.size(); ++i) {
        if (merged[i].first == LiveSpeaker::Player)
            input.array.push_back(textMessage("user", "input_text", merged[i].second));
        else
            input.array.push_back(textMessage("assistant", "output_text", merged[i].second));
    }
    return input;
}

json::Value LiveSession::startEvent() const {
    json::Value format = json::Value::makeObject();
    format.object["type"] = str("audio/pcm");
    format.object["rate"] = num(m_settings.sampleRate);
    json::Value output = json::Value::makeObject();
    output.object["voice"] = str(m_settings.voice);
    json::Value audio = json::Value::makeObject();
    audio.object["format"] = std::move(format);
    audio.object["output"] = std::move(output);

    json::Value responses = json::Value::makeObject();
    responses.object["model"] = str(m_settings.backendModel);
    if (!m_settings.backendInstructions.empty()) responses.object["instructions"] = str(m_settings.backendInstructions);
    responses.object["tools"] = m_tools.responsesTools();
    responses.object["tool_choice"] = str(m_settings.toolChoice);
    responses.object["parallel_tool_calls"] = json::Value::makeBool(m_settings.parallelToolCalls);
    if (!m_settings.reasoningEffort.empty()) {
        json::Value reasoning = json::Value::makeObject();
        reasoning.object["effort"] = str(m_settings.reasoningEffort);
        responses.object["reasoning"] = std::move(reasoning);
    }
    if (!m_settings.serviceTier.empty()) responses.object["service_tier"] = str(m_settings.serviceTier);
    if (m_settings.maxOutputTokens > 0) responses.object["max_output_tokens"] = num(m_settings.maxOutputTokens);
    json::Value delegation = json::Value::makeObject();
    delegation.object["type"] = str("responses");
    delegation.object["responses"] = std::move(responses);
    if (m_settings.acknowledgementDelayMs >= 0)
        delegation.object["acknowledgement_delay_ms"] = num(std::min(800, m_settings.acknowledgementDelayMs));

    json::Value session = json::Value::makeObject();
    session.object["model"] = str(m_settings.model);
    if (const std::string instructions = fullInstructions(); !instructions.empty())
        session.object["instructions"] = str(instructions);
    session.object["input"] = inputItems();
    session.object["audio"] = std::move(audio);
    session.object["delegation"] = std::move(delegation);
    if (!m_settings.safetyIdentifier.empty()) session.object["safety_identifier"] = str(m_settings.safetyIdentifier);

    json::Value event = json::Value::makeObject();
    event.object["type"] = str("session.start");
    event.object["event_id"] = str(std::format("start_{}", m_session));
    event.object["session"] = std::move(session);
    return event;
}

Result<void> LiveSession::start() {
    if (running()) return Error{std::format("a live session is running ({})", liveStateName(m_state))};
    if (m_settings.apiKey.empty()) return Error{"no API key"};
    if (m_settings.model.empty() || m_settings.backendModel.empty()) return Error{"no model or no backend model"};
    if (m_settings.sampleRate != 16000 && m_settings.sampleRate != 24000)
        return Error{std::format("a live session's audio is 16000 or 24000 Hz, not {}", m_settings.sampleRate)};
    if (Result<void> url = net::checkWebSocketUrl(m_settings.url); !url) return url;
    m_failure = {};
    m_closeReason = LiveCloseReason::None;
    m_closeIntent = LiveCloseReason::None;
    m_lastServerError.clear();
    ++m_session;
    m_stats.sessions += 1;
    m_stats.sessionId.clear();
    m_stats.expiresAt = 0;
    m_sessionStarted = false;
    m_usage.sessionSeconds = 0.0;
    m_usage.confirmed = true;
    m_sessionCounted = false;
    m_expiryDeadline = 0.0;
    m_startTime = now();
    m_lastActivity = m_startTime;
    m_uplink.clear();
    m_delegations.clear();
    m_nextCall = m_calls.size();
    m_toolName.clear();
    // A new connection object for each session: the last one may still wait for its close.
    m_ws = std::make_unique<net::WebSocket>(net::WebSocketDesc{.fake = m_fake, .clock = m_clock});

    net::Headers headers = {{"Authorization", "Bearer " + m_settings.apiKey}};
    headers.insert(headers.end(), m_settings.extraHeaders.begin(), m_settings.extraHeaders.end());
    const Result<void> connected = m_ws->connect({.url = m_settings.url,
                                                  .headers = std::move(headers),
                                                  .connectTimeout = m_settings.connectTimeout,
                                                  .closeTimeout = 3.0,
                                                  .pongTimeout = 0.0,
                                                  .maxMessageBytes = 8u << 20},
                                                 {.onOpen = [this] { onOpen(); },
                                                  .onMessage = [this](const net::WsMessage& m) { onMessage(m); },
                                                  .onEnd = [this](const net::WsEnd& e) { onEnd(e); }});
    if (!connected) {
        fail({.kind = ErrorKind::NoBackend, .message = redact(connected.error(), m_settings.apiKey)});
        return Error{m_failure.message};
    }
    note(std::format("session {}: connecting to {} ({}, voice {}, backend {}, {} tools, {} Hz)", m_session,
                     net::logUrl(m_settings.url), m_settings.model, m_settings.voice, m_settings.backendModel,
                     m_tools.size(), m_settings.sampleRate));
    setState(LiveState::Connecting);
    return {};
}

void LiveSession::onOpen() {
    m_stats.connectSeconds = now() - m_startTime;
    setState(LiveState::Starting);
    sendJson(startEvent());
}

void LiveSession::fail(Failure failure) {
    failure.message = redact(failure.message, m_settings.apiKey);
    m_failure = std::move(failure);
    m_nextCall = m_calls.size();
    m_toolName.clear();
    log::warning(kLog, "live: session {} failed: {} ({})", m_session, errorKindName(m_failure.kind), m_failure.message);
    setState(LiveState::Failed);
    m_events.push_back({.kind = LiveEventKind::Failed, .state = m_state, .text = m_failure.message});
}

void LiveSession::closed(LiveCloseReason reason, bool confirmed) {
    if (!m_sessionCounted) {
        m_previousSeconds += m_usage.sessionSeconds;
        m_sessionCounted = true;
    }
    m_usage.confirmed = confirmed;
    m_closeReason = reason;
    m_nextCall = m_calls.size();
    m_toolName.clear();
    m_uplink.clear();
    if (m_reconnect) {
        m_reconnect = false;
        m_reconnectPending = true; // started from update(), once this connection's handlers are done
        m_ws->abort();
    } else if (m_ws->active()) {
        m_ws->close();
    }
    note(std::format("session {} closed: {}{}, {:.1f} s billed", m_session, liveCloseReasonName(reason),
                     confirmed ? "" : " (no session.closed: the usage is not confirmed)", m_usage.sessionSeconds));
    setState(LiveState::Closed);
    m_events.push_back({.kind = LiveEventKind::Closed, .state = m_state, .reason = reason});
}

void LiveSession::beginClose(LiveCloseReason intent) {
    if (m_state == LiveState::Connecting || m_state == LiveState::Starting) {
        // Nothing to finalize yet that the docs describe: the connection is dropped.
        m_ws->abort();
        closed(intent, m_state == LiveState::Connecting);
        return;
    }
    if (m_state != LiveState::Listening && m_state != LiveState::Speaking && m_state != LiveState::CallingTool) return;
    m_closeIntent = intent;
    json::Value event = json::Value::makeObject();
    event.object["type"] = str("session.close");
    event.object["event_id"] = str(nextEventId("close"));
    sendJson(event);
    note(std::format("session {}: closing ({})", m_session, liveCloseReasonName(intent)));
    setState(LiveState::Closing);
}

void LiveSession::close() { beginClose(LiveCloseReason::Requested); }

void LiveSession::cancel() {
    m_reconnectPending = false;
    if (!running()) return;
    m_reconnect = false;
    m_ws->abort();
    closed(LiveCloseReason::Cancelled, false);
}

void LiveSession::onEnd(const net::WsEnd& end) {
    if (!running()) return; // closed() or fail() already ran
    if (!m_sessionStarted) {
        Failure f;
        if (end.status != 0) {
            net::Response r;
            r.status = end.status;
            f = failureFromResponse(r, {}, m_settings.apiKey);
            f.message += " (the WebSocket upgrade)";
        } else if (!end.clean()) {
            net::Response r;
            r.error = end.error;
            r.message = end.message;
            f = failureFromResponse(r, {}, m_settings.apiKey);
        } else {
            f.kind = ErrorKind::BadResponse;
            f.message = std::format("the server closed the connection before session.started (code {}{})",
                                    end.closeCode, end.closeReason.empty() ? "" : ", " + end.closeReason);
        }
        if (!m_lastServerError.empty()) {
            f.message += ": " + m_lastServerError;
            if (f.kind == ErrorKind::BadResponse) f.kind = ErrorKind::Refused;
        }
        fail(std::move(f));
        return;
    }
    // The connection ended without session.closed.
    closed(m_closeIntent != LiveCloseReason::None ? m_closeIntent : LiveCloseReason::ConnectionLost, false);
}

void LiveSession::onMessage(const net::WsMessage& message) {
    if (message.kind != net::WsKind::Text) return; // the docs: JSON text messages only
    Result<json::Value> event = json::parse(message.data);
    if (!event || !event->isObject()) {
        log::warning(kLog, "live: a message that is not a JSON object ({} bytes)", message.data.size());
        return;
    }
    if (m_tap) m_tap(*event);
    handleEvent(*event);
}

void LiveSession::appendTranscript(LiveSpeaker speaker, const json::Value& event) {
    const std::string delta = event["delta"].stringOr("");
    if (delta.empty()) return;
    const int startMs = int(event["start_ms"].numberOr(0));
    const int endMs = int(event["end_ms"].numberOr(0));
    if (m_history.empty() || m_history.back().speaker != speaker || m_history.back().session != m_session)
        m_history.push_back({.speaker = speaker, .startMs = startMs, .session = m_session});
    LiveTurn& turn = m_history.back();
    turn.text += delta; // exactly as received, spaces included (the docs)
    turn.endMs = std::max(turn.endMs, endMs);
    m_events.push_back(
        {.kind = speaker == LiveSpeaker::Player ? LiveEventKind::InputTranscript : LiveEventKind::OutputTranscript,
         .state = m_state,
         .text = delta,
         .turn = m_history.size() - 1});
    noteActivity();
}

void LiveSession::handleEvent(const json::Value& e) {
    const std::string type = e["type"].stringOr("");
    if (type == "session.output_audio.delta") {
        Result<std::vector<std::uint8_t>> bytes = base64Decode(e["delta"].stringOr(""));
        if (!bytes) {
            log::warning(kLog, "live: output audio that is not base64");
            return;
        }
        const std::size_t n = bytes->size() / 2;
        const std::size_t base = m_downlink.size();
        m_downlink.resize(base + n);
        float peak = 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            const auto s = std::int16_t(std::uint16_t((*bytes)[2 * i]) | (std::uint16_t((*bytes)[2 * i + 1]) << 8));
            m_downlink[base + i] = float(s) / 32768.0f;
            peak = std::max(peak, std::abs(m_downlink[base + i]));
        }
        m_stats.audioReceivedSeconds += double(n) / m_settings.sampleRate;
        // Not documented, seen on 2026-11-14: the server sends audio all the time, 100 ms pieces at real-time pace,
        // silence (zeros) while the station does not speak. Only pieces above about -54 dBFS count as speech.
        if (peak > kAudiblePeak) {
            m_stats.audibleReceivedSeconds += double(n) / m_settings.sampleRate;
            m_lastAudioIn = now();
            noteActivity();
        }
        refreshSpeaking();
    } else if (type == "session.input_transcript.delta") {
        appendTranscript(LiveSpeaker::Player, e);
        if (m_playback) m_events.push_back({.kind = LiveEventKind::PlayerOverlap, .state = m_state});
    } else if (type == "session.output_transcript.delta") {
        appendTranscript(LiveSpeaker::Station, e);
    } else if (type == "response.event") {
        handleResponseEvent(e);
    } else if (type == "session.delegation.created") {
        const json::Value& d = e["delegation"];
        m_stats.delegations += 1;
        noteActivity();
        if (d["target"].stringOr("") == "responses") {
            Delegation del;
            del.id = d["id"].stringOr("");
            del.created = now();
            m_delegations.push_back(std::move(del));
        }
    } else if (type == "session.usage.updated") {
        m_usage.sessionSeconds = e["usage"]["seconds"].numberOr(m_usage.sessionSeconds);
        m_usage.contextRatio = e["context_window"]["usage_ratio"].numberOr(m_usage.contextRatio);
    } else if (type == "session.instructions.appended" || type == "session.thinking.appended" ||
               type == "session.commentary.appended") {
        m_events.push_back(
            {.kind = LiveEventKind::Appended, .state = m_state, .text = e["client_event_id"].stringOr("")});
    } else if (type == "session.started") {
        const json::Value& s = e["session"];
        m_sessionStarted = true;
        m_stats.startSeconds = now() - m_startTime;
        m_stats.sessionId = s["id"].stringOr("");
        m_stats.expiresAt = std::int64_t(s["expires_at"].numberOr(0));
        if (m_stats.expiresAt > 0) m_expiryDeadline = now() + (double(m_stats.expiresAt) - m_unixClock());
        note(std::format("session {} started: {}, {} ms after start(), expires in {:.0f} s", m_session,
                         m_stats.sessionId, (long long)(m_stats.startSeconds * 1000.0 + 0.5),
                         m_stats.expiresAt > 0 ? double(m_stats.expiresAt) - m_unixClock() : -1.0));
        setState(LiveState::Listening);
        m_events.push_back({.kind = LiveEventKind::Started, .state = m_state, .text = m_stats.sessionId});
        noteActivity();
        for (const std::string& pending : m_pendingAppends) (void)m_ws->sendText(pending);
        m_pendingAppends.clear();
        flushAudio();
    } else if (type == "session.closed") {
        m_usage.sessionSeconds = e["usage"]["seconds"].numberOr(m_usage.sessionSeconds);
        const std::string reason = e["reason"].stringOr("");
        LiveCloseReason r = reasonFromServer(reason);
        if (r == LiveCloseReason::Requested && m_closeIntent != LiveCloseReason::None) r = m_closeIntent;
        if (r == LiveCloseReason::Expired && m_settings.reconnectOnExpiry) m_reconnect = true;
        closed(r, true);
    } else if (type == "error") {
        const json::Value& err = e["error"];
        const std::string code = err["code"].stringOr("");
        const std::string message = redact(err["message"].stringOr(""), m_settings.apiKey);
        m_lastServerError = code.empty() ? message : code + ": " + message;
        m_stats.serverErrors += 1;
        if (!m_sessionStarted && m_state == LiveState::Starting) {
            // Before session.started the session cannot run (a refused model, a bad setting): it fails here.
            m_ws->abort();
            fail({.kind = kindOfStartError(code, err["type"].stringOr("")),
                  .message = "the server refused the session: " + m_lastServerError});
            return;
        }
        log::warning(kLog, "live: server error {}{}", m_lastServerError,
                     err["client_event_id"].isNull() ? "" : " (event " + err["client_event_id"].stringOr("") + ")");
        m_events.push_back({.kind = LiveEventKind::ServerError, .state = m_state, .text = m_lastServerError});
    } else if (type == "session.updated") {
        note(std::format("session {}: settings updated", m_session));
    } else if (type == "info") {
        note(std::format("info {}: {}", e["code"].stringOr(""), e["message"].stringOr("")));
    }
    // session.input_audio.muted / unmuted and the fixture events are not used here; other events are ignored.
}

void LiveSession::handleResponseEvent(const json::Value& envelope) {
    const std::string delegationId = envelope["delegation_id"].stringOr("");
    const json::Value& e = envelope["event"];
    const std::string type = e["type"].stringOr("");
    auto find = [&]() -> Delegation& {
        for (Delegation& d : m_delegations)
            if (d.id == delegationId) return d;
        // A response event without a delegation seen first (the docs allow a null delegation_id).
        Delegation d;
        d.id = delegationId;
        d.created = now();
        m_delegations.push_back(std::move(d));
        return m_delegations.back();
    };
    if (type == "response.created") {
        Delegation& d = find();
        d.calls.clear();
        d.responseDone = false;
        d.continued = false;
        d.answeredAt = 0.0;
    } else if (type == "response.output_item.done") {
        const json::Value& item = e["item"];
        if (item["type"].stringOr("") != "function_call") return;
        Delegation& d = find();
        LiveToolCall call;
        call.callId = item["call_id"].stringOr("");
        call.name = item["name"].stringOr("");
        call.arguments = item["arguments"].stringOr("");
        call.delegationId = delegationId;
        d.calls.push_back(m_calls.size());
        m_calls.push_back(std::move(call));
        m_stats.toolCalls += 1;
        noteActivity();
    } else if (type == "response.completed" || type == "response.failed" || type == "response.incomplete") {
        const json::Value& usage = e["response"]["usage"];
        const auto in = std::int64_t(usage["input_tokens"].numberOr(0));
        const auto cached = std::int64_t(usage["input_tokens_details"]["cached_tokens"].numberOr(0));
        m_usage.backendInputTokens += in - cached;
        m_usage.backendCachedTokens += cached;
        m_usage.backendOutputTokens += std::int64_t(usage["output_tokens"].numberOr(0));
        if (type != "response.completed") {
            const json::Value& err = e["response"]["error"];
            log::warning(kLog, "live: the backend's response ended {}{}", type.substr(9),
                         err.isNull() ? "" : ": " + redact(err["message"].stringOr(""), m_settings.apiKey));
        }
        Delegation& d = find();
        d.responseDone = true;
        if (d.calls.empty()) {
            // The backend answered without (more) tool calls: the delegation is done.
            m_stats.lastDelegationSeconds = now() - d.created;
            m_events.push_back({.kind = LiveEventKind::DelegationDone,
                                .state = m_state,
                                .text = d.id,
                                .seconds = m_stats.lastDelegationSeconds});
            const std::string id = d.id;
            std::erase_if(m_delegations, [&](const Delegation& x) { return x.id == id; });
        } else {
            continueDelegations();
        }
    }
}

void LiveSession::stepTools() {
    if (m_nextCall >= m_calls.size()) return;
    LiveToolCall& call = m_calls[m_nextCall];
    if (m_callAnnounced == 0 || m_toolName != call.name || m_state != LiveState::CallingTool) {
        // Shown for one update() before it runs (the state, toolName(), ToolStarted).
        m_toolName = call.name;
        m_callAnnounced = m_updates;
        setState(LiveState::CallingTool);
        m_events.push_back({.kind = LiveEventKind::ToolStarted, .state = m_state, .tool = m_nextCall});
        return;
    }
    if (m_updates == m_callAnnounced) return;
    const double t0 = now();
    call.result = m_tools.callWithText(call.name, call.arguments);
    call.seconds = now() - t0;
    call.finished = true;
    log::info(kLog, "live: tool {}: {}, {:.2f} ms", call.name, call.result.isError ? "error" : "ok",
              call.seconds * 1000.0);
    json::Value item = json::Value::makeObject();
    item.object["type"] = str("function_call_output");
    item.object["call_id"] = str(call.callId);
    item.object["output"] = str((call.result.isError ? "Error: " : "") + call.result.contentText());
    json::Value event = json::Value::makeObject();
    event.object["type"] = str("response.item.create");
    event.object["event_id"] = str(nextEventId("tool"));
    event.object["item"] = std::move(item);
    sendJson(event);
    m_events.push_back({.kind = LiveEventKind::ToolFinished, .state = m_state, .tool = m_nextCall});
    ++m_nextCall;
    m_callAnnounced = 0;
    m_toolName.clear();
    noteActivity();
    if (m_nextCall >= m_calls.size()) refreshSpeaking();
    continueDelegations();
}

void LiveSession::continueDelegations() {
    const double t = now();
    for (Delegation& d : m_delegations) {
        if (d.continued || d.calls.empty()) continue;
        const bool all =
            std::all_of(d.calls.begin(), d.calls.end(), [&](std::size_t i) { return m_calls[i].finished; });
        if (!all) continue;
        if (d.answeredAt == 0.0) d.answeredAt = t;
        if (!d.responseDone) {
            // The docs say the forwarded lifecycle events include response.completed; should it not come, the
            // backend would wait for ever: after the fallback's time it is continued anyway.
            if (m_settings.completedFallbackSeconds <= 0.0 || t - d.answeredAt < m_settings.completedFallbackSeconds)
                continue;
            if (!m_fallbackLogged)
                log::warning(kLog,
                             "live: no response.completed {:.0f} s after the tool results of delegation {}: "
                             "response.create sent anyway",
                             t - d.answeredAt, d.id);
            m_fallbackLogged = true;
        }
        // Every pending call has its result: one response.create continues the backend (the docs).
        json::Value event = json::Value::makeObject();
        event.object["type"] = str("response.create");
        event.object["event_id"] = str(nextEventId("continue"));
        sendJson(event);
        d.continued = true;
    }
}

void LiveSession::refreshSpeaking() {
    if (m_state != LiveState::Listening && m_state != LiveState::Speaking) return;
    const bool speaking = m_playback || now() - m_lastAudioIn < 0.3;
    setState(speaking ? LiveState::Speaking : LiveState::Listening);
}

void LiveSession::update() {
    m_ws->update();
    ++m_updates;
    if (m_reconnectPending && m_state == LiveState::Closed) {
        m_reconnectPending = false;
        m_events.push_back({.kind = LiveEventKind::Reconnecting, .state = m_state});
        note("a new session with the conversation's text");
        if (Result<void> again = start(); !again) log::warning(kLog, "live: no new session: {}", again.error());
    }
    const double t = now();
    if (m_state == LiveState::Starting && t - m_stateTime >= m_settings.startTimeout) {
        m_ws->abort();
        fail({.kind = ErrorKind::Timeout,
              .message = std::format("no session.started within {} s", m_settings.startTimeout)});
    }
    if (m_state == LiveState::Closing && t - m_stateTime >= m_settings.closeTimeout) {
        m_ws->abort();
        closed(m_closeIntent, false);
    }
    const bool started =
        m_state == LiveState::Listening || m_state == LiveState::Speaking || m_state == LiveState::CallingTool;
    if (!started) return;
    stepTools();
    continueDelegations(); // the fallback's time
    if (m_state == LiveState::CallingTool && m_nextCall >= m_calls.size()) setState(LiveState::Listening);
    if (m_playback) {
        m_lastPlayback = t;
        noteActivity();
    }
    refreshSpeaking();
    const bool quiet = m_nextCall >= m_calls.size() && !m_playback && t - m_lastAudioIn > 0.5;
    if (m_settings.idleSeconds > 0.0 && quiet && t - m_lastActivity >= m_settings.idleSeconds) {
        note(std::format("session {}: {:.0f} s without speech", m_session, t - m_lastActivity));
        beginClose(LiveCloseReason::Idle);
        return;
    }
    if (m_settings.reconnectOnExpiry && m_settings.reconnectBeforeExpirySeconds > 0.0 && m_expiryDeadline > 0.0 &&
        quiet && t - m_lastActivity > 1.0 && t >= m_expiryDeadline - m_settings.reconnectBeforeExpirySeconds) {
        note(std::format("session {}: {:.0f} s before its expiry, at a quiet moment", m_session, m_expiryDeadline - t));
        m_reconnect = true;
        beginClose(LiveCloseReason::Expired);
    }
}

void LiveSession::sendChunk(std::span<const std::int16_t> pcm) {
    std::vector<std::uint8_t> bytes(pcm.size() * 2);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const auto u = std::uint16_t(pcm[i]);
        bytes[2 * i] = std::uint8_t(u & 0xFF);
        bytes[2 * i + 1] = std::uint8_t(u >> 8);
    }
    // Built by hand: the base64 text is most of the message, and a json::Value would copy it twice more.
    std::string message = R"({"type":"session.input_audio.append","audio":")";
    message += base64Encode(bytes);
    message += "\"}";
    (void)m_ws->sendText(std::move(message));
    m_stats.audioSentSeconds += double(pcm.size()) / m_settings.sampleRate;
}

void LiveSession::flushAudio() {
    const auto chunk = std::size_t(std::max(0.01, m_settings.chunkSeconds) * m_settings.sampleRate);
    std::size_t at = 0;
    while (m_uplink.size() - at >= chunk) {
        sendChunk(std::span(m_uplink).subspan(at, chunk));
        at += chunk;
    }
    m_uplink.erase(m_uplink.begin(), m_uplink.begin() + std::ptrdiff_t(at));
}

void LiveSession::sendAudio(std::span<const float> samples) {
    const bool started =
        m_state == LiveState::Listening || m_state == LiveState::Speaking || m_state == LiveState::CallingTool;
    const bool starting = m_state == LiveState::Connecting || m_state == LiveState::Starting;
    if (!started && !starting) return;
    const bool gate = gateClosed();
    const std::size_t base = m_uplink.size();
    m_uplink.resize(base + samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const float s = gate || std::isnan(samples[i]) ? 0.0f : std::clamp(samples[i], -1.0f, 1.0f);
        m_uplink[base + i] = std::int16_t(std::lround(s * 32767.0f));
    }
    if (starting) {
        const auto max = std::size_t(std::max(0.0, m_settings.preStartBufferSeconds) * m_settings.sampleRate);
        if (m_uplink.size() > max) m_uplink.erase(m_uplink.begin(), m_uplink.end() - std::ptrdiff_t(max));
        return;
    }
    flushAudio();
}

std::size_t LiveSession::takeAudio(std::vector<float>& out) {
    const std::size_t n = m_downlink.size();
    out.insert(out.end(), m_downlink.begin(), m_downlink.end());
    m_downlink.clear();
    return n;
}

void LiveSession::setPlaybackActive(bool active) {
    m_playback = active;
    if (active) {
        m_lastPlayback = now();
        noteActivity();
    }
}

Result<std::string> LiveSession::append(const char* type, std::string text) {
    if (text.empty()) return Error{"an empty append"};
    const bool started =
        m_state == LiveState::Listening || m_state == LiveState::Speaking || m_state == LiveState::CallingTool;
    const bool starting = m_state == LiveState::Connecting || m_state == LiveState::Starting;
    if (!started && !starting) return Error{std::format("no live session ({})", liveStateName(m_state))};
    const std::string id = nextEventId("ctx");
    json::Value event = json::Value::makeObject();
    event.object["type"] = str(type);
    event.object["event_id"] = str(id);
    event.object["delegation_id"] = json::Value(); // null: session-wide (non-null ids are client delegations only)
    event.object["content"] = json::Value::makeString(std::move(text));
    if (starting) {
        if (m_pendingAppends.size() >= 16) return Error{"too many appends queued while the session starts"};
        m_pendingAppends.push_back(writeCompactJson(event));
    } else {
        sendJson(event);
    }
    noteActivity();
    return id;
}

Result<std::string> LiveSession::appendInstructions(std::string text) {
    return append("session.instructions.append", std::move(text));
}
Result<std::string> LiveSession::appendThinking(std::string text) {
    return append("session.thinking.append", std::move(text));
}
Result<std::string> LiveSession::appendCommentary(std::string text) {
    return append("session.commentary.append", std::move(text));
}

Result<void> LiveSession::updateTools() {
    if (m_state != LiveState::Listening && m_state != LiveState::Speaking && m_state != LiveState::CallingTool)
        return Error{std::format("no running live session ({})", liveStateName(m_state))};
    json::Value responses = json::Value::makeObject();
    responses.object["tools"] = m_tools.responsesTools();
    json::Value delegation = json::Value::makeObject();
    delegation.object["type"] = str("responses");
    delegation.object["responses"] = std::move(responses);
    json::Value session = json::Value::makeObject();
    session.object["delegation"] = std::move(delegation);
    json::Value event = json::Value::makeObject();
    event.object["type"] = str("session.update");
    event.object["event_id"] = str(nextEventId("update"));
    event.object["session"] = std::move(session);
    sendJson(event);
    return {};
}

LiveUsage LiveSession::usage() const {
    LiveUsage u = m_usage;
    u.totalSeconds = m_previousSeconds + (m_sessionCounted ? 0.0 : m_usage.sessionSeconds);
    u.sessionCost = u.totalSeconds / 60.0 * m_settings.pricePerMinute;
    u.backendCost = (double(u.backendInputTokens) * m_settings.backendInputPerMillion +
                     double(u.backendCachedTokens) * m_settings.backendCachedPerMillion +
                     double(u.backendOutputTokens) * m_settings.backendOutputPerMillion) /
                    1e6;
    return u;
}

std::vector<LiveEvent> LiveSession::takeEvents() { return std::exchange(m_events, {}); }

json::Value LiveSession::historyJson() const {
    json::Value turns = json::Value::makeArray();
    for (const LiveTurn& t : m_history) {
        json::Value v = json::Value::makeObject();
        v.object["speaker"] = str(t.speaker == LiveSpeaker::Player ? "player" : "station");
        v.object["text"] = str(t.text);
        turns.array.push_back(std::move(v));
    }
    json::Value out = json::Value::makeObject();
    out.object["turns"] = std::move(turns);
    return out;
}

Result<void> LiveSession::setHistory(const json::Value& history) {
    if (!history["turns"].isArray()) return Error{"a live history is {\"turns\":[...]}"};
    std::vector<LiveTurn> turns;
    for (const json::Value& v : history["turns"].array) {
        const std::string speaker = v["speaker"].stringOr("");
        if (speaker != "player" && speaker != "station") return Error{"a turn's speaker is player or station"};
        turns.push_back({.speaker = speaker == "player" ? LiveSpeaker::Player : LiveSpeaker::Station,
                         .text = v["text"].stringOr("")});
    }
    m_history = std::move(turns);
    return {};
}

} // namespace quench::assistant
