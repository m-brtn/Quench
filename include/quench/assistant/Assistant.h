#pragma once

// An assistant that lets a language model operate a game through its tools (docs/assistant.md): a turn takes the
// player's text (typed, or transcribed from a recording), sends it with the game's system prompt, a fresh state
// summary and the conversation so far, executes the tool calls the model asks for through the ToolRegistry and sends
// their results back, round after round, until the model answers with text (or the round limit), and can turn the
// answer into speech. Each stage can be used alone (transcribe, ask, speak) or all in one call (askByVoice).
//
// One operation at a time: a new one while one runs is refused (ErrorKind::Busy); cancel() first to replace it.
// Everything happens on the main thread: the requests' answers arrive in update() (through net::Client::update())
// and tool handlers run there, one tool call per update(), so that a UI sees each call and a turn can be cancelled
// between a tool call's request and its result. Observable every frame: state(), toolName(), partialText(),
// answer(), toolCalls(), failure(), stats(); transitions also as events (takeEvents()).
//
// The API key is in settings() only: it is never logged, never in an error message or in historyJson().

#include <quench/assistant/Chat.h>
#include <quench/assistant/Provider.h>
#include <quench/assistant/Speech.h>
#include <quench/assistant/Tools.h>
#include <quench/core/Json.h>
#include <quench/core/Result.h>
#include <quench/net/Http.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quench::assistant {

enum class State : std::uint8_t {
    Idle,         // nothing ran yet, or the last operation was cancelled
    Transcribing, // the recording is uploaded and transcribed
    Thinking,     // a chat request is out: the model writes text or asks for tools
    CallingTool,  // a tool call is being executed (toolName())
    Speaking,     // the answer (answer(), already final) is being turned into speech
    Done,         // finished: answer(), transcript() and the speech are there
    Failed,       // failure() says why; what was finished before stays readable
};
const char* stateName(State state); // "idle", "transcribing", ...

struct Limits {
    int maxRounds = 3;                        // chat requests per turn; the last one asks for text (answerOnLastRound)
    bool answerOnLastRound = true;            // the last round sends tool_choice "none"
    int historyTurns = 6;                     // finished turns kept for the next ones
    std::size_t toolResultChars = 4000;       // a tool result as sent in its turn, cut beyond (UTF-8 safe)
    std::size_t historyToolResultChars = 400; // the same once the turn is in the history
};

enum class EventKind : std::uint8_t {
    StateChanged, // state
    Transcribed,  // text: the transcript
    ToolStarted,  // tool: the index in toolCalls()
    ToolFinished, // tool
    Answered,     // text: the answer
    SpeechReady,  // the speech can be taken
    Failed,       // failure()
    Cancelled,
};

struct Event {
    EventKind kind = EventKind::StateChanged;
    State state = State::Idle;
    std::string text;
    std::size_t tool = 0;
};

struct ToolCallRecord {
    std::string id;
    std::string name;
    std::string arguments; // JSON text as the model sent it
    int round = 0;         // 1-based
    bool finished = false;
    ToolResult result;    // once finished
    double seconds = 0.0; // the handler's time
};

// Counters of a turn (stats()) and of the assistant's life (totals()).
struct Stats {
    int requests = 0; // HTTP requests sent
    int rounds = 0;   // chat requests
    int toolCalls = 0;
    Usage usage;                  // tokens and cost of chat, transcription (and speech when the provider says)
    double audioSeconds = 0.0;    // recordings transcribed
    std::int64_t spokenChars = 0; // characters sent to speech (the prefix included)
    double speechSeconds = 0.0;   // audio received
    double transcribeTime = 0.0;  // seconds of each stage
    double thinkTime = 0.0;
    double toolTime = 0.0;
    double speakTime = 0.0;
    double firstTextTime = -1.0; // from the start to the model's first text; -1 without text (and in totals())
    double totalTime = 0.0;

    void add(const Stats& other);
};

struct AssistantDesc {
    // The client the requests go through; null: the assistant makes its own (with `fake` and `clock`) and updates it
    // in update(). A shared client is updated by its owner.
    net::Client* client = nullptr;
    net::FakeNetwork* fake = nullptr;
    std::function<double()> clock; // seconds, for the stats and the own client; default: a steady clock
};

struct AskOptions {
    bool speak = false; // turn the answer into speech after the turn
};

class Assistant {
public:
    explicit Assistant(const AssistantDesc& desc = {});
    ~Assistant();
    Assistant(const Assistant&) = delete;
    Assistant& operator=(const Assistant&) = delete;

    ProviderSettings& settings() { return m_settings; }
    const ProviderSettings& settings() const { return m_settings; }
    void setSettings(ProviderSettings settings) { m_settings = std::move(settings); }
    ToolRegistry& tools() { return m_tools; }
    Limits& limits() { return m_limits; }

    // The game's instructions, sent as the system message of every request.
    void setSystemPrompt(std::string prompt) { m_systemPrompt = std::move(prompt); }
    // Called once at the start of each turn; its text goes before the player's words in that turn's user message
    // ("Game state:\n...\n\nPlayer: ..."), and not into the history.
    void setStateSummary(std::function<std::string()> summary) { m_stateSummary = std::move(summary); }

    // A typed turn.
    Result<void> ask(std::string text, AskOptions options = {});
    // A voice turn: the recording (a WAV file) -> transcription -> the turn -> speech (AskOptions::speak).
    Result<void> askByVoice(std::span<const std::uint8_t> wav, AskOptions options = {.speak = true});
    // The stages alone.
    Result<void> transcribe(std::span<const std::uint8_t> wav);
    Result<void> speak(std::string text);

    // Stops what runs (its request is cancelled, tool calls not yet executed are dropped; those executed keep their
    // effects) and returns to Idle; the turn is not added to the history. Nothing when idle.
    void cancel();
    // Once a frame on the main thread.
    void update();

    State state() const { return m_state; }
    bool busy() const;
    const std::string& toolName() const { return m_toolName; } // the tool being called, in CallingTool
    const std::string& transcript() const { return m_transcript; }
    const std::string& partialText() const { return m_partial; } // the model's text so far in this round
    const std::string& answer() const { return m_answer; }
    const std::vector<ToolCallRecord>& toolCalls() const { return m_calls; }
    const Failure& failure() const { return m_failure; }
    const Stats& stats() const { return m_stats; }   // of the last (or running) operation
    const Stats& totals() const { return m_totals; } // of every finished or cancelled one
    // The speech of the last operation, once (nothing until SpeechReady).
    std::optional<SpeechAudio> takeSpeech();
    std::vector<Event> takeEvents();

    // The conversation: finished turns as chat messages ({"turns":[[messages...]...]}), without the system prompt,
    // the state summaries or the key; for a save, and back.
    json::Value historyJson() const;
    Result<void> setHistory(const json::Value& history);
    void clearHistory() { m_history.clear(); }
    std::size_t historyTurns() const { return m_history.size(); }

    net::Client& client() { return *m_client; }

private:
    enum class Op : std::uint8_t { None, Transcribe, Ask, Speak };

    Result<void> begin(Op op);
    void setState(State state);
    void fail(Failure failure);
    void finishOperation();
    void startTurn(std::string text);
    void sendChat();
    void chatDone(const net::Response& response);
    void stepTools();
    void sendTranscription(std::vector<std::uint8_t> wav);
    void transcriptionDone(const net::Response& response);
    void sendSpeech(const std::string& text);
    void speechDone(const net::Response& response);
    json::Value messages() const;
    void commitTurn();
    double now() const;
    bool sendRequest(net::Request request, net::Handlers handlers);
    void announceTool();

    std::unique_ptr<net::Client> m_ownClient;
    net::Client* m_client = nullptr;
    std::function<double()> m_clock;

    ProviderSettings m_settings;
    ToolRegistry m_tools;
    Limits m_limits;
    std::string m_systemPrompt;
    std::function<std::string()> m_stateSummary;

    // The running operation.
    Op m_op = Op::None;
    State m_state = State::Idle;
    AskOptions m_options;
    bool m_voice = false;
    std::uint64_t m_serial = 0;   // increases with every request and cancel; a late answer of an old one is ignored
    std::shared_ptr<int> m_alive; // the handlers of a shared client's requests outlive the assistant: they check it
    net::RequestId m_request = 0; // the request in flight, 0 for none
    int m_status = 0;             // its HTTP status, once the headers came
    bool m_inUpdate = false;
    std::string m_errorBody; // an error status's body as it streams
    std::unique_ptr<ChatStreamParser> m_parser;
    double m_opStart = 0.0;
    double m_stageStart = 0.0;
    std::uint64_t m_updates = 0; // update() calls, so that a tool call is visible for one before it runs
    std::uint64_t m_toolAnnounced = 0;
    std::size_t m_nextTool = 0; // the next record of m_calls to execute
    int m_round = 0;
    std::string m_turnState;         // the state summary of this turn
    std::vector<json::Value> m_turn; // this turn's messages after the user's
    std::string m_userText;

    std::string m_toolName;
    std::string m_transcript;
    std::string m_partial;
    std::string m_answer;
    std::vector<ToolCallRecord> m_calls;
    Failure m_failure;
    Stats m_stats;
    Stats m_totals;
    std::optional<SpeechAudio> m_speech;
    std::vector<Event> m_events;

    std::vector<std::vector<json::Value>> m_history; // finished turns, oldest first
};

} // namespace quench::assistant
