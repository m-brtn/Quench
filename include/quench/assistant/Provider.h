#pragma once

// The provider an assistant talks to (docs/assistant.md, "Settings"): an OpenAI-compatible API (OpenRouter, OpenAI's
// own, a local server) chosen by settings only, and the errors its answers map to. The API key lives in memory only:
// nothing in this module writes it to a log line, an error message, a saved conversation or a settings file
// (settingsToJson leaves it out, redact() takes it out of a provider's own messages).

#include <quench/core/Json.h>
#include <quench/net/Http.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace quench::assistant {

// How a recording is uploaded for speech-to-text: OpenRouter's JSON with the audio in base64, or OpenAI's multipart
// form with a file (OpenRouter also takes it; most local servers take only this one).
enum class TranscriptionForm : std::uint8_t { Json, Multipart };

// What text-to-speech answers with: raw 16-bit little-endian mono PCM (no decoding; the rate from the answer's
// Content-Type, else ProviderSettings::pcmRate), or MP3 (smaller, decoded with the engine's decoder).
enum class SpeechFormat : std::uint8_t { Pcm, Mp3 };

const char* transcriptionFormName(TranscriptionForm form); // "json", "multipart"
const char* speechFormatName(SpeechFormat format);         // "pcm", "mp3"

struct ProviderSettings {
    std::string name;    // "openrouter", "openai", "local" or the game's own; shown, never sent
    std::string baseUrl; // "https://openrouter.ai/api/v1": the paths /chat/completions, /audio/... are appended
    // Sent as "Authorization: Bearer <key>"; empty sends no Authorization (a local server). Memory only.
    std::string apiKey;

    std::string chatModel;          // "anthropic/claude-haiku-5.5"
    std::string transcriptionModel; // "openai/whisper-large-v3-turbo"
    std::string speechModel;        // "elevenlabs/eleven-v4-turbo"
    std::string voice;              // "matilda": a name the speech model knows

    // Put before the text sent to speech, separated by a space (an audio tag such as "[flat, even, neutral tone]"
    // for a model that reads them); not a part of the answer the player reads.
    std::string speechPrefix;
    std::string speechInstructions; // "instructions" of the speech request, for models that take it; empty: none
    double speechSpeed = 0.0;       // "speed"; 0: not sent (the model's default)
    SpeechFormat speechFormat = SpeechFormat::Pcm;
    std::uint32_t pcmRate = 24000; // the PCM rate when the answer's Content-Type does not name one

    TranscriptionForm transcriptionForm = TranscriptionForm::Json;
    std::string language = "en"; // a hint for speech-to-text (ISO 639-1); empty: none

    double chatTimeout = 30.0; // seconds per request, from the send to the end of the body
    double transcriptionTimeout = 30.0;
    double speechTimeout = 30.0;

    bool streamChat = true;        // server-sent events: the text arrives as it is written
    bool streamUsage = true;       // stream_options.include_usage (OpenAI needs it for usage in a stream)
    bool parallelToolCalls = true; // parallel_tool_calls
    int maxTokens = 0;             // max_tokens of an answer; 0: not sent
    double temperature = -1.0;     // below 0: not sent

    // Sent with every request (OpenRouter's HTTP-Referer and X-Title name an app on its dashboard). A header
    // whose name looks secret (net::isSecretHeader) is never logged either.
    net::Headers extraHeaders;
};

// OpenRouter (https://openrouter.ai/api/v1): JSON transcription, PCM speech, the models measured on 2026-11-13.
ProviderSettings openRouterPreset();
// OpenAI's own API (https://api.openai.com/v1): multipart transcription. Its model names are not verified here.
ProviderSettings openAiPreset();
// A local OpenAI-compatible server (http://localhost is allowed without TLS); no key, multipart transcription, the
// models left for the game to name.
ProviderSettings localPreset(std::string baseUrl = "http://localhost:8080/v1");

// Everything but the key, for a game's settings file; settingsFromJson reads it back onto `base` (the key of `base`
// stays as it is: it is never read from a file).
json::Value settingsToJson(const ProviderSettings& settings);
ProviderSettings settingsFromJson(const json::Value& value, ProviderSettings base = openRouterPreset());

// `text` with every occurrence of `key` replaced by "[key]" (nothing when the key is shorter than 4 bytes).
std::string redact(std::string_view text, std::string_view key);

enum class ErrorKind : std::uint8_t {
    None,
    NotConfigured, // no base URL or no model for the request
    Busy,          // an operation is running (one at a time)
    BadKey,        // HTTP 401: the key is wrong, revoked or missing
    NoCredits,     // HTTP 402: the account has no credits left
    Refused,       // HTTP 403: refused (a model's moderation, a key without access)
    NotFound,      // HTTP 404: a wrong base URL or model name
    BadRequest,    // another 4xx: the provider did not accept the request
    RateLimited,   // HTTP 429
    ProviderDown,  // 5xx: the provider or the model failed
    StreamError,   // the provider reported an error in the middle of a stream, or the stream ended early
    BadResponse,   // an answer that cannot be read (not JSON, no text, audio that does not decode)
    Timeout,
    NoNetwork,    // offline, the host not found, the connection refused or lost
    NotAllowed,   // the URL is refused by quench_net's HTTPS rule
    TooLarge,     // an answer over the size limit
    NoBackend,    // no network backend on this platform
    Cancelled,    // Assistant::cancel
    RoundLimit,   // the model still asked for tools after the last round
    NothingHeard, // the transcription of a voice turn is empty
};
const char* errorKindName(ErrorKind kind); // "bad key", "no credits", ...

struct Failure {
    ErrorKind kind = ErrorKind::None;
    int status = 0;      // the HTTP status, 0 without one
    std::string message; // readable, for the player or a log; never the key

    bool failed() const { return kind != ErrorKind::None; }
};

// The failure of a finished request (Failure{} for a 2xx answer): the transport's error or the HTTP status, with the
// provider's own message from an error body ({"error":{"message":...}}) when there is one, without the key.
Failure failureFromResponse(const net::Response& response, std::string_view body, std::string_view key);

} // namespace quench::assistant
