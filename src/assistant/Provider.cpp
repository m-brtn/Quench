// Provider settings, presets and the mapping of failed requests (quench/assistant/Provider.h).

#include <quench/assistant/Provider.h>

#include "assistant/Text.h"

#include <format>
#include <utility>

namespace quench::assistant {

const char* transcriptionFormName(TranscriptionForm form) {
    return form == TranscriptionForm::Multipart ? "multipart" : "json";
}

const char* speechFormatName(SpeechFormat format) { return format == SpeechFormat::Mp3 ? "mp3" : "pcm"; }

ProviderSettings openRouterPreset() {
    ProviderSettings s;
    s.name = "openrouter";
    s.baseUrl = "https://openrouter.ai/api/v1";
    s.chatModel = "anthropic/claude-haiku-5.5";
    s.transcriptionModel = "openai/whisper-large-v3-turbo";
    s.speechModel = "elevenlabs/eleven-v4-turbo";
    s.voice = "matilda";
    s.speechFormat = SpeechFormat::Pcm;
    s.transcriptionForm = TranscriptionForm::Json;
    return s;
}

ProviderSettings openAiPreset() {
    ProviderSettings s;
    s.name = "openai";
    s.baseUrl = "https://api.openai.com/v1";
    s.chatModel = "gpt-6-luna";
    s.transcriptionModel = "whisper-1";
    s.speechModel = "gpt-4o-mini-tts";
    s.voice = "alloy";
    s.speechFormat = SpeechFormat::Pcm;
    s.transcriptionForm = TranscriptionForm::Multipart;
    return s;
}

ProviderSettings localPreset(std::string baseUrl) {
    ProviderSettings s;
    s.name = "local";
    s.baseUrl = std::move(baseUrl);
    s.transcriptionForm = TranscriptionForm::Multipart;
    s.speechFormat = SpeechFormat::Pcm;
    s.streamUsage = false; // not every local server knows stream_options
    s.chatTimeout = 120.0; // a model on the player's own machine can be slow
    return s;
}

json::Value settingsToJson(const ProviderSettings& s) {
    json::Value v = json::Value::makeObject();
    auto& o = v.object;
    o["name"] = json::Value::makeString(s.name);
    o["baseUrl"] = json::Value::makeString(s.baseUrl);
    o["chatModel"] = json::Value::makeString(s.chatModel);
    o["transcriptionModel"] = json::Value::makeString(s.transcriptionModel);
    o["speechModel"] = json::Value::makeString(s.speechModel);
    o["voice"] = json::Value::makeString(s.voice);
    o["speechPrefix"] = json::Value::makeString(s.speechPrefix);
    o["speechInstructions"] = json::Value::makeString(s.speechInstructions);
    o["speechSpeed"] = json::Value::makeNumber(s.speechSpeed);
    o["speechFormat"] = json::Value::makeString(speechFormatName(s.speechFormat));
    o["pcmRate"] = json::Value::makeNumber(s.pcmRate);
    o["transcriptionForm"] = json::Value::makeString(transcriptionFormName(s.transcriptionForm));
    o["language"] = json::Value::makeString(s.language);
    o["chatTimeout"] = json::Value::makeNumber(s.chatTimeout);
    o["transcriptionTimeout"] = json::Value::makeNumber(s.transcriptionTimeout);
    o["speechTimeout"] = json::Value::makeNumber(s.speechTimeout);
    o["streamChat"] = json::Value::makeBool(s.streamChat);
    o["streamUsage"] = json::Value::makeBool(s.streamUsage);
    o["parallelToolCalls"] = json::Value::makeBool(s.parallelToolCalls);
    o["maxTokens"] = json::Value::makeNumber(s.maxTokens);
    o["temperature"] = json::Value::makeNumber(s.temperature);
    o["cacheMarker"] = json::Value::makeBool(s.cacheMarker);
    // Extra headers only when their names do not look secret: a secret one belongs with the key.
    json::Value headers = json::Value::makeObject();
    for (const net::Header& h : s.extraHeaders) {
        if (!net::isSecretHeader(h.name)) headers.object[h.name] = json::Value::makeString(h.value);
    }
    o["extraHeaders"] = std::move(headers);
    return v;
}

ProviderSettings settingsFromJson(const json::Value& v, ProviderSettings s) {
    if (!v.isObject()) return s;
    const auto str = [&](const char* key, std::string& out) {
        if (v[key].type == json::Value::Type::String) out = v[key].string;
    };
    const auto num = [&](const char* key, double& out) { out = v[key].numberOr(out); };
    const auto flag = [&](const char* key, bool& out) { out = v[key].boolOr(out); };
    str("name", s.name);
    str("baseUrl", s.baseUrl);
    str("chatModel", s.chatModel);
    str("transcriptionModel", s.transcriptionModel);
    str("speechModel", s.speechModel);
    str("voice", s.voice);
    str("speechPrefix", s.speechPrefix);
    str("speechInstructions", s.speechInstructions);
    str("language", s.language);
    num("speechSpeed", s.speechSpeed);
    num("chatTimeout", s.chatTimeout);
    num("transcriptionTimeout", s.transcriptionTimeout);
    num("speechTimeout", s.speechTimeout);
    num("temperature", s.temperature);
    flag("streamChat", s.streamChat);
    flag("streamUsage", s.streamUsage);
    flag("parallelToolCalls", s.parallelToolCalls);
    flag("cacheMarker", s.cacheMarker);
    if (v["speechFormat"].type == json::Value::Type::String) {
        s.speechFormat = v["speechFormat"].string == "mp3" ? SpeechFormat::Mp3 : SpeechFormat::Pcm;
    }
    if (v["transcriptionForm"].type == json::Value::Type::String) {
        s.transcriptionForm =
            v["transcriptionForm"].string == "multipart" ? TranscriptionForm::Multipart : TranscriptionForm::Json;
    }
    if (v["pcmRate"].isNumber() && v["pcmRate"].number >= 8000 && v["pcmRate"].number <= 192000) {
        s.pcmRate = std::uint32_t(v["pcmRate"].number);
    }
    if (v["maxTokens"].isNumber() && v["maxTokens"].number >= 0) s.maxTokens = int(v["maxTokens"].number);
    if (v["extraHeaders"].isObject()) {
        s.extraHeaders.clear();
        for (const auto& [name, value] : v["extraHeaders"].object) {
            if (value.type == json::Value::Type::String && !net::isSecretHeader(name)) {
                s.extraHeaders.push_back({name, value.string});
            }
        }
    }
    return s;
}

std::string redact(std::string_view text, std::string_view key) {
    std::string out(text);
    if (key.size() < 4) return out;
    for (std::size_t at = out.find(key); at != std::string::npos; at = out.find(key, at + 5)) {
        out.replace(at, key.size(), "[key]");
    }
    return out;
}

const char* errorKindName(ErrorKind kind) {
    switch (kind) {
    case ErrorKind::None: return "none";
    case ErrorKind::NotConfigured: return "not configured";
    case ErrorKind::Busy: return "busy";
    case ErrorKind::BadKey: return "bad key";
    case ErrorKind::NoCredits: return "no credits";
    case ErrorKind::Refused: return "refused";
    case ErrorKind::NotFound: return "not found";
    case ErrorKind::BadRequest: return "bad request";
    case ErrorKind::RateLimited: return "rate limited";
    case ErrorKind::ProviderDown: return "provider error";
    case ErrorKind::StreamError: return "stream error";
    case ErrorKind::BadResponse: return "bad response";
    case ErrorKind::Timeout: return "timeout";
    case ErrorKind::NoNetwork: return "no network";
    case ErrorKind::NotAllowed: return "not allowed";
    case ErrorKind::TooLarge: return "too large";
    case ErrorKind::NoBackend: return "no backend";
    case ErrorKind::Cancelled: return "cancelled";
    case ErrorKind::RoundLimit: return "round limit";
    case ErrorKind::NothingHeard: return "nothing heard";
    }
    return "?";
}

namespace {

// The provider's own words in an error body: {"error":{"message":...}} (OpenRouter, OpenAI), {"error":"..."} or
// {"message":"..."}; at most 300 characters.
std::string providerMessage(std::string_view body) {
    body = detail::trimSpace(body);
    if (body.empty()) return {};
    std::string text;
    if (body.front() == '{') {
        if (Result<json::Value> v = json::parse(std::string(body))) {
            const json::Value& e = (*v)["error"];
            if (e.isObject()) {
                text = e["message"].stringOr("");
                // OpenRouter puts the upstream provider's words under metadata.raw.
                if (const json::Value& raw = e["metadata"]["raw"]; raw.type == json::Value::Type::String) {
                    if (text.empty() || raw.string.size() < 200) text += (text.empty() ? "" : ": ") + raw.string;
                }
            } else {
                text = e.stringOr((*v)["message"].stringOr(""));
            }
        }
    }
    if (text.empty() && body.front() != '{' && body.front() != '<') text = std::string(body); // plain text
    return detail::trimText(detail::trimSpace(text), 300);
}

} // namespace

Failure failureFromResponse(const net::Response& r, std::string_view body, std::string_view key) {
    Failure f;
    if (!r.ok()) {
        switch (r.error) {
        case net::ErrorKind::Timeout: f.kind = ErrorKind::Timeout; break;
        case net::ErrorKind::NoNetwork: f.kind = ErrorKind::NoNetwork; break;
        case net::ErrorKind::Cancelled: f.kind = ErrorKind::Cancelled; break;
        case net::ErrorKind::NotAllowed: f.kind = ErrorKind::NotAllowed; break;
        case net::ErrorKind::TooLarge: f.kind = ErrorKind::TooLarge; break;
        case net::ErrorKind::NoBackend: f.kind = ErrorKind::NoBackend; break;
        default: f.kind = ErrorKind::NoNetwork; break; // Tls, Failed, InvalidRequest: the connection did not work
        }
        f.status = r.status;
        const char* what = f.kind == ErrorKind::Timeout      ? "the provider did not answer in time"
                           : f.kind == ErrorKind::Cancelled  ? "cancelled"
                           : f.kind == ErrorKind::TooLarge   ? "the answer is too large"
                           : f.kind == ErrorKind::NotAllowed ? "the address is not allowed"
                                                             : "the provider cannot be reached";
        f.message = redact(r.message.empty() ? std::string(what) : std::format("{} ({})", what, r.message), key);
        return f;
    }
    if (r.status >= 200 && r.status < 300) return f;
    f.status = r.status;
    const char* what = nullptr;
    switch (r.status) {
    case 401:
        f.kind = ErrorKind::BadKey;
        what = "the provider refused the API key";
        break;
    case 402:
        f.kind = ErrorKind::NoCredits;
        what = "the account has no credits left";
        break;
    case 403:
        f.kind = ErrorKind::Refused;
        what = "the provider refused the request";
        break;
    case 404:
        f.kind = ErrorKind::NotFound;
        what = "not found (the base URL or the model name)";
        break;
    case 408:
        f.kind = ErrorKind::Timeout;
        what = "the provider timed out";
        break;
    case 413:
        f.kind = ErrorKind::TooLarge;
        what = "the request is too large for the provider";
        break;
    case 429:
        f.kind = ErrorKind::RateLimited;
        what = "too many requests: rate limited";
        break;
    default:
        if (r.status >= 500) {
            f.kind = ErrorKind::ProviderDown;
            what = "the provider or the model failed";
        } else {
            f.kind = ErrorKind::BadRequest;
            what = "the provider did not accept the request";
        }
        break;
    }
    const std::string said = providerMessage(body);
    f.message = redact(said.empty() ? std::format("{} (HTTP {})", what, r.status)
                                    : std::format("{} (HTTP {}): {}", what, r.status, said),
                       key);
    return f;
}

} // namespace quench::assistant
