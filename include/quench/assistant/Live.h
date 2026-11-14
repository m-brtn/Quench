#pragma once

// A live, full-duplex spoken conversation with OpenAI's GPT-Live (docs/live.md): the player talks to the game at any
// time, the voice model answers by voice, yields when interrupted, and hands reasoning and the game's tools to a
// backend model (Responses delegation); the tool calls come back to the game, run through the same ToolRegistry as
// the turn-based Assistant (the same schema check), and their results go back to the backend.
//
// Headless and transport only: the session neither records nor plays. The game feeds microphone samples
// (sendAudio(), from audio::Microphone's streaming mode), plays what takeAudio() gives (an audio::VoiceStream on a bus
// of its choice) and tells the session whether that playback is active (setPlaybackActive(), for the uplink gate and
// the idle timer). Transcripts of both sides arrive as they stream (captions, a chat history), the game's own events
// go in as context while it runs (appendInstructions(), appendThinking(), appendCommentary()).
//
// Money: GPT-Live bills every second a session is open, silence included (LiveSettings::pricePerMinute, 0.05 US
// dollars on 2026-11-14), plus the backend's tokens. A session closes by itself after LiveSettings::idleSeconds
// without speech from either side; start() again resumes with the conversation's text carried over.
//
// One session at a time; everything on the main thread (update() once a frame). The API key is in settings() only:
// it is never logged, never in an error message, a history or an event.

#include <quench/assistant/Provider.h>
#include <quench/assistant/Tools.h>
#include <quench/core/Json.h>
#include <quench/core/Result.h>
#include <quench/net/WebSocket.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quench::net {
class FakeWebSocketServer;
}

namespace quench::assistant {

struct LiveSettings {
    std::string url = "wss://api.openai.com/v1/live/sessions"; // no query parameters (the model is in session.start)
    std::string apiKey; // "Authorization: Bearer <key>"; memory only, never logged or saved
    std::string model = "gpt-live-1";
    std::string voice = "marin"; // one of the 38 built-in voices; fixed for a session
    // The voice model's instructions (up to 16,384 tokens): who it is, how it speaks, the language ("Speak Russian
    // unless the player asks to switch": there is no language setting), and when to hand work to the backend
    // ("Generate <|SpawnThinking|> when ...", the docs' wording for gpt-live-1).
    std::string instructions;
    // Appends a "Delegation policy" with the registered tools' names and descriptions to the instructions (the docs'
    // template), so the voice model knows what the backend can do.
    bool describeTools = true;

    std::string backendModel = "gpt-6-luna";
    std::string backendInstructions; // the backend's own prompt: the game's rules, how to use its tools
    std::string reasoningEffort;     // "none", "minimal", "low" ... ; empty: not sent
    std::string serviceTier;         // "priority" (fast mode), "flex" ... ; empty: not sent
    int maxOutputTokens = 0;         // per delegated response, at least 16; 0: not sent
    bool parallelToolCalls = true;
    std::string toolChoice = "auto"; // "auto", "none", "required"
    int acknowledgementDelayMs = -1; // 0 .. 800 before the automatic spoken acknowledgement; -1: not sent

    std::uint32_t sampleRate = 24000; // PCM16 mono both ways: 24000 (the default) or 16000
    double chunkSeconds = 0.1;        // microphone audio is sent in pieces of this length (the docs send 100 ms)

    // Closing by itself: after this many seconds without speech from either side (transcripts, audio received, the
    // game's playback, a tool call, an append); 0: never (the session runs, and bills, until close()).
    double idleSeconds = 45.0;
    // At the server's expiry (session.closed with reason "expired") a new session starts with the conversation's
    // text; also a little before expires_at, at a quiet moment (reconnectBeforeExpirySeconds; 0: only at expiry).
    bool reconnectOnExpiry = true;
    double reconnectBeforeExpirySeconds = 30.0;

    // The uplink gate for loudspeakers: while the game's playback is active, and gateHoldSeconds after it, silence is
    // sent instead of the microphone (the station does not hear itself and answer its own echo). Off for headphones:
    // then the player can interrupt the station.
    bool uplinkGate = true;
    double gateHoldSeconds = 0.25;

    double pricePerMinute = 0.05;         // US dollars per minute of open session (gpt-live-1, 2026-11-14)
    double backendInputPerMillion = 0.10; // US dollars per million tokens (gpt-6-luna standard, 2026-11-14)
    double backendCachedPerMillion = 0.01;
    double backendOutputPerMillion = 0.50;

    double connectTimeout = 15.0; // seconds to the open WebSocket
    double startTimeout = 15.0;   // seconds from the open WebSocket to session.started
    double closeTimeout = 15.0;   // seconds from session.close to session.closed (the docs' example waits 15)
    // The text history carried into a new session's input: the newest messages within these characters (about 4
    // characters a token; the API takes at most 128 messages and 8,192 tokens).
    std::size_t historyChars = 24000;
    double preStartBufferSeconds = 5.0; // microphone audio kept while connecting, sent once the session started
    // When every tool call of a response has its result but the backend's response.completed has not come within
    // this many seconds, response.create is sent anyway (once per response, one log line). Delegations took 2.3 to
    // 3.8 s in the manual check; response.completed always came there.
    double completedFallbackSeconds = 15.0;
    std::string safetyIdentifier; // session.safety_identifier; empty: not sent
    net::Headers extraHeaders;
};

enum class LiveState : std::uint8_t {
    Idle,        // nothing started yet
    Connecting,  // the WebSocket opens
    Starting,    // session.start sent, waiting for session.started
    Listening,   // the session runs; nobody speaks now (or only the player)
    Speaking,    // the station's audio arrives or plays
    CallingTool, // a tool call runs (toolName())
    Closing,     // session.close sent, waiting for session.closed
    Closed,      // closeReason() says why; start() begins a new session with the history
    Failed,      // failure() says why; nothing runs
};
const char* liveStateName(LiveState state);

enum class LiveCloseReason : std::uint8_t {
    None,
    Requested,      // close(), or the server's close_requested
    Idle,           // closed by the session after idleSeconds without speech
    Expired,        // the server's duration limit
    Content,        // a safety filter ended it
    RemoteHangup,   // the server ended it gracefully
    ConnectionLost, // the connection was lost (the server's connection_lost, or no session.closed at all)
    Cancelled,      // cancel(): dropped at once
};
const char* liveCloseReasonName(LiveCloseReason reason);

enum class LiveSpeaker : std::uint8_t { Player, Station };

// A stretch of one side's transcript (deltas appended until the other side speaks).
struct LiveTurn {
    LiveSpeaker speaker = LiveSpeaker::Player;
    std::string text;
    int startMs = 0, endMs = 0; // on the session's timeline
    int session = 0;            // which session of this object (1, 2 ... across reconnects)
};

enum class LiveEventKind : std::uint8_t {
    StateChanged,     // state
    Started,          // text: the session's id
    InputTranscript,  // text: the player's delta; turn: its index in history()
    OutputTranscript, // text: the station's delta; turn
    PlayerOverlap,    // the player spoke while the game's playback was active (the game may flush it)
    ToolStarted,      // tool: the index in toolCalls()
    ToolFinished,     // tool
    DelegationDone,   // seconds: from the delegation to the backend's last response
    Appended,         // text: the event id of an append the server accepted
    ServerError,      // text: "code: message" of an error event (a rejected command; the session goes on)
    Reconnecting,     // a new session starts with the history (expiry)
    Closed,           // reason
    Failed,           // failure()
};

struct LiveEvent {
    LiveEventKind kind = LiveEventKind::StateChanged;
    LiveState state = LiveState::Idle;
    LiveCloseReason reason = LiveCloseReason::None;
    std::string text;
    std::size_t turn = 0;
    std::size_t tool = 0;
    double seconds = 0.0;
};

struct LiveToolCall {
    std::string callId;
    std::string name;
    std::string arguments; // JSON text as the backend sent it
    std::string delegationId;
    bool finished = false;
    ToolResult result;
    double seconds = 0.0; // the handler's time
};

struct LiveUsage {
    double sessionSeconds = 0.0; // billed seconds of the running (or last) session, from the server
    double totalSeconds = 0.0;   // of every session of this object
    bool confirmed = true;       // false when a session ended without session.closed (its last count stands)
    std::int64_t backendInputTokens = 0, backendCachedTokens = 0, backendOutputTokens = 0;
    double contextRatio = 0.0; // the voice model's context in use, 0 .. 1
    double sessionCost = 0.0;  // US dollars by the settings' prices: the open seconds
    double backendCost = 0.0;  // the backend's tokens
    double cost() const { return sessionCost + backendCost; }
};

struct LiveStats {
    int sessions = 0;
    double connectSeconds = 0.0; // of the last session: start() to the open WebSocket
    double startSeconds = 0.0;   // start() to session.started
    double audioSentSeconds = 0.0, audioReceivedSeconds = 0.0;
    double audibleReceivedSeconds = 0.0; // of the received audio, the pieces that are not silence
    int delegations = 0;
    int toolCalls = 0;
    int serverErrors = 0;
    double lastDelegationSeconds = 0.0;
    std::string sessionId;
    std::int64_t expiresAt = 0; // Unix seconds, from session.started
};

struct LiveDesc {
    net::FakeWebSocketServer* fake = nullptr; // connect to this fake server (tests)
    std::function<double()> clock;            // seconds; default: a steady clock
    std::function<double()> unixClock;        // Unix seconds (expires_at); default: the system clock
};

class LiveSession {
public:
    explicit LiveSession(const LiveDesc& desc = {});
    ~LiveSession();
    LiveSession(const LiveSession&) = delete;
    LiveSession& operator=(const LiveSession&) = delete;

    LiveSettings& settings() { return m_settings; }
    const LiveSettings& settings() const { return m_settings; }
    ToolRegistry& tools() { return m_tools; }
    // A text put before the history at every start (a developer message): the game's state now ("Day 3, two
    // walkers idle ..."). Called at each start and reconnect.
    void setStateSummary(std::function<std::string()> summary) { m_stateSummary = std::move(summary); }

    // Opens a session: Connecting, Starting, Listening. Refused while one runs, without a key or a model, with a rate
    // other than 16000 or 24000. The history of earlier sessions goes into the new one.
    Result<void> start();
    // Closes gracefully (session.close, then session.closed with the final usage): Closing, Closed(Requested).
    void close();
    // Drops the connection at once: Closed(Cancelled); the usage stays unconfirmed. Nothing when not running.
    void cancel();
    // Once a frame on the main thread: messages, tool calls (one per update()), the gate, the idle timer, expiry.
    void update();

    // Microphone samples (mono, -1 .. 1, at settings().sampleRate), sent in chunkSeconds pieces; silence instead
    // while the uplink gate is closed. Kept for up to preStartBufferSeconds while the session starts.
    void sendAudio(std::span<const float> samples);
    // Appends the station's audio received since the last call (mono, -1 .. 1, at settings().sampleRate). GPT-Live
    // sends audio all the time (silence between answers, at real-time pace; seen 2026-11-14, not documented).
    std::size_t takeAudio(std::vector<float>& out);
    // The game's playback is audible now (VoiceStream::audible(), not active(): the stream is never empty while a
    // session runs): for the gate, the idle timer, Speaking.
    void setPlaybackActive(bool active);

    // Context while the session runs (queued while it starts; at most 500 tokens each). Return the event id.
    // instructions: how the station behaves now (may interrupt its speech); thinking: a silent fact ("Rover 2 has
    // arrived at E7"); commentary: something it should say aloud (it may paraphrase it).
    Result<std::string> appendInstructions(std::string text);
    Result<std::string> appendThinking(std::string text);
    Result<std::string> appendCommentary(std::string text);
    // Sends the registry's tools again (session.update of delegation.responses.tools) after the game changed them.
    Result<void> updateTools();

    LiveState state() const { return m_state; }
    bool running() const;    // Connecting .. Closing
    bool gateClosed() const; // the microphone is silenced now
    const std::string& toolName() const { return m_toolName; }
    LiveCloseReason closeReason() const { return m_closeReason; }
    const Failure& failure() const { return m_failure; }
    // The last error event of the server (a rejected command), "code: message"; empty when none.
    const std::string& lastServerError() const { return m_lastServerError; }
    const std::vector<LiveTurn>& history() const { return m_history; }
    const std::vector<LiveToolCall>& toolCalls() const { return m_calls; }
    LiveUsage usage() const;
    const LiveStats& stats() const { return m_stats; }
    std::vector<LiveEvent> takeEvents();

    // The conversation's text ({"turns":[{"speaker":"player"|"station","text"}...]}) for a save, and back; without
    // the key or the instructions.
    json::Value historyJson() const;
    Result<void> setHistory(const json::Value& history);
    void clearHistory() { m_history.clear(); }

    // The session.start event this session would send now (tests, the manual check); the key is not in it.
    json::Value startEvent() const;
    // Diagnostics: called with every server event as parsed, before the session handles it (the manual check prints
    // them). The key is never in an event the client receives, but output audio is: keep the tap cheap.
    void setEventTap(std::function<void(const json::Value& event)> tap) { m_tap = std::move(tap); }

private:
    struct Delegation {
        std::string id;
        double created = 0.0;
        std::vector<std::size_t> calls; // indices into m_calls of the current response
        bool responseDone = false;      // the current response reached its end
        bool continued = false;         // response.create sent for the current calls
        double answeredAt = 0.0;        // when the last of the current calls got its result (0: not all yet)
    };

    Result<void> connect();
    void onOpen();
    void onMessage(const net::WsMessage& message);
    void onEnd(const net::WsEnd& end);
    void handleEvent(const json::Value& event);
    void handleResponseEvent(const json::Value& envelope);
    void stepTools();
    void continueDelegations();
    void sendJson(const json::Value& event);
    std::string nextEventId(std::string_view prefix);
    Result<std::string> append(const char* type, std::string text);
    void sendChunk(std::span<const std::int16_t> pcm);
    void flushAudio();
    void appendTranscript(LiveSpeaker speaker, const json::Value& event);
    void setState(LiveState state);
    void fail(Failure failure);
    void closed(LiveCloseReason reason, bool confirmed);
    void beginClose(LiveCloseReason intent);
    void noteActivity();
    void refreshSpeaking();
    json::Value inputItems() const;
    std::string fullInstructions() const;
    double now() const;
    void note(std::string_view what) const;

    std::function<double()> m_clock;
    std::function<double()> m_unixClock;
    net::FakeWebSocketServer* m_fake = nullptr;
    std::unique_ptr<net::WebSocket> m_ws;
    LiveSettings m_settings;
    ToolRegistry m_tools;
    std::function<std::string()> m_stateSummary;
    std::function<void(const json::Value&)> m_tap;

    LiveState m_state = LiveState::Idle;
    LiveCloseReason m_closeReason = LiveCloseReason::None;
    LiveCloseReason m_closeIntent = LiveCloseReason::None; // why we sent session.close
    Failure m_failure;
    std::string m_lastServerError;
    std::string m_toolName;
    std::vector<LiveTurn> m_history;
    std::vector<LiveToolCall> m_calls;
    std::vector<Delegation> m_delegations;
    std::size_t m_nextCall = 0;
    std::uint64_t m_updates = 0, m_callAnnounced = 0;
    std::vector<LiveEvent> m_events;
    std::vector<std::string> m_pendingAppends; // JSON events queued while starting
    std::vector<std::int16_t> m_uplink;        // microphone samples not yet sent (a chunk, or the pre-start buffer)
    std::vector<float> m_downlink;             // received samples not yet taken
    LiveStats m_stats;
    LiveUsage m_usage;
    double m_previousSeconds = 0.0; // billed seconds of the earlier sessions
    double m_startTime = 0.0;       // of the running session's start()
    double m_stateTime = 0.0;       // when the state last changed (start and close timeouts)
    double m_lastActivity = 0.0;
    double m_lastAudioIn = -1e9;
    double m_lastPlayback = -1e9;
    double m_expiryDeadline = 0.0; // on the clock; 0: unknown
    bool m_playback = false;
    bool m_sessionStarted = false;
    bool m_reconnect = false;        // start a new session once this one ended (expiry)
    bool m_reconnectPending = false; // ... from the next update()
    bool m_sessionCounted = false;   // the running session's seconds are in m_previousSeconds
    int m_session = 0;
    std::uint64_t m_eventSerial = 0;
    bool m_fallbackLogged = false;
};

} // namespace quench::assistant
