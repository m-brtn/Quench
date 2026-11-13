// Speech-to-text and text-to-speech requests and answers (quench/assistant/Speech.h).

#include <quench/assistant/Speech.h>

#include "assistant/Text.h"

#include <quench/audio/Sound.h>
#include <quench/core/Json.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <utility>

namespace quench::assistant {

std::string base64Encode(std::span<const std::uint8_t> bytes) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t n = (std::uint32_t(bytes[i]) << 16) | (std::uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63];
        out += kAlphabet[n & 63];
    }
    if (const std::size_t rest = bytes.size() - i; rest > 0) {
        std::uint32_t n = std::uint32_t(bytes[i]) << 16;
        if (rest == 2) n |= std::uint32_t(bytes[i + 1]) << 8;
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += rest == 2 ? kAlphabet[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

namespace {

// A boundary that does not occur in the file: derived from its bytes, made longer until it is absent.
std::string boundaryFor(std::span<const std::uint8_t> data) {
    std::uint64_t h = 1469598103934665603ull;
    for (const std::uint8_t b : data) h = (h ^ b) * 1099511628211ull;
    std::string boundary = std::format("quench-assistant-{:016x}", h);
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    while (text.find(boundary) != std::string_view::npos) boundary += "x";
    return boundary;
}

void appendText(std::vector<std::uint8_t>& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}

} // namespace

net::Request buildTranscriptionRequest(const ProviderSettings& s, std::span<const std::uint8_t> wav) {
    net::Request r;
    r.method = "POST";
    r.url = detail::endpointUrl(s, "/audio/transcriptions");
    r.timeoutSeconds = s.transcriptionTimeout;
    if (s.transcriptionForm == TranscriptionForm::Json) {
        json::Value audio = json::Value::makeObject();
        audio.object["data"] = json::Value::makeString(base64Encode(wav));
        audio.object["format"] = json::Value::makeString("wav");
        json::Value body = json::Value::makeObject();
        body.object["model"] = json::Value::makeString(s.transcriptionModel);
        body.object["input_audio"] = std::move(audio);
        if (!s.language.empty()) body.object["language"] = json::Value::makeString(s.language);
        r.headers = detail::requestHeaders(s, "application/json");
        r.setBody(writeCompactJson(body));
        return r;
    }
    const std::string boundary = boundaryFor(wav);
    const auto field = [&](std::string_view name, std::string_view value) {
        appendText(r.body, std::format("--{}\r\nContent-Disposition: form-data; name=\"{}\"\r\n\r\n{}\r\n", boundary,
                                       name, value));
    };
    field("model", s.transcriptionModel);
    if (!s.language.empty()) field("language", s.language);
    field("response_format", "json");
    appendText(r.body, std::format("--{}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
                                   "Content-Type: audio/wav\r\n\r\n",
                                   boundary));
    r.body.insert(r.body.end(), wav.begin(), wav.end());
    appendText(r.body, std::format("\r\n--{}--\r\n", boundary));
    r.headers = detail::requestHeaders(s, "multipart/form-data; boundary=" + boundary);
    return r;
}

Result<Transcript> parseTranscription(std::string_view body) {
    Result<json::Value> v = json::parse(std::string(detail::trimSpace(body)));
    if (!v) return Error{std::format("the transcription is not JSON ({})", v.error())};
    if ((*v)["text"].type != json::Value::Type::String) return Error{"the transcription has no text"};
    Transcript t;
    t.text = std::string(detail::trimSpace((*v)["text"].string));
    const json::Value& usage = (*v)["usage"];
    t.audioSeconds = usage["seconds"].numberOr((*v)["duration"].numberOr(0.0));
    t.usage.promptTokens = std::int64_t(usage["input_tokens"].numberOr(usage["prompt_tokens"].numberOr(0.0)));
    t.usage.completionTokens = std::int64_t(usage["output_tokens"].numberOr(usage["completion_tokens"].numberOr(0.0)));
    if (usage["cost"].isNumber()) {
        t.usage.cost = usage["cost"].number;
        t.usage.costKnown = true;
    }
    return t;
}

std::string speechInput(const ProviderSettings& s, std::string_view text) {
    const std::string_view prefix = detail::trimSpace(s.speechPrefix);
    if (prefix.empty()) return std::string(text);
    return std::format("{} {}", prefix, text);
}

net::Request buildSpeechRequest(const ProviderSettings& s, std::string_view text) {
    json::Value body = json::Value::makeObject();
    body.object["model"] = json::Value::makeString(s.speechModel);
    body.object["input"] = json::Value::makeString(speechInput(s, text));
    if (!s.voice.empty()) body.object["voice"] = json::Value::makeString(s.voice);
    body.object["response_format"] = json::Value::makeString(speechFormatName(s.speechFormat));
    if (s.speechSpeed > 0.0) body.object["speed"] = json::Value::makeNumber(s.speechSpeed);
    if (!s.speechInstructions.empty()) body.object["instructions"] = json::Value::makeString(s.speechInstructions);
    net::Request r;
    r.method = "POST";
    r.url = detail::endpointUrl(s, "/audio/speech");
    r.headers = detail::requestHeaders(s, "application/json");
    r.setBody(writeCompactJson(body));
    r.timeoutSeconds = s.speechTimeout;
    return r;
}

std::vector<std::int16_t> SpeechAudio::pcm16() const {
    std::vector<std::int16_t> out(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        out[i] = std::int16_t(std::lround(std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f));
    }
    return out;
}

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    return out;
}

// A parameter of a media type ("rate" of "audio/pcm;rate=24000;channels=1"), 0 when absent.
std::uint32_t parameter(std::string_view type, std::string_view name) {
    std::size_t at = 0;
    while ((at = type.find(';', at)) != std::string_view::npos) {
        ++at;
        std::string_view p = detail::trimSpace(type.substr(at, type.find(';', at) - at));
        if (p.size() > name.size() && p.substr(0, name.size()) == name && p[name.size()] == '=') {
            std::uint32_t n = 0;
            const std::string_view value = p.substr(name.size() + 1);
            std::from_chars(value.data(), value.data() + value.size(), n);
            return n;
        }
    }
    return 0;
}

} // namespace

Result<SpeechAudio> decodeSpeech(std::span<const std::uint8_t> body, std::string_view contentType,
                                 SpeechFormat expected, std::uint32_t pcmRate) {
    const std::string type = lower(detail::trimSpace(contentType));
    const std::string media(detail::trimSpace(std::string_view(type).substr(0, type.find(';'))));
    if (media == "application/json" || media.starts_with("text/")) {
        return Error{"the speech answer is text, not audio"};
    }
    if (body.empty()) return Error{"the speech answer is empty"};
    const bool pcm = media.empty() ? expected == SpeechFormat::Pcm
                                   : (media == "audio/pcm" || media == "audio/l16" || media == "audio/raw");
    SpeechAudio audio;
    if (pcm) {
        const std::uint32_t rate = parameter(type, "rate") ? parameter(type, "rate") : pcmRate;
        const std::uint32_t channels = std::max<std::uint32_t>(1, parameter(type, "channels"));
        if (rate < 4000 || rate > 192000) return Error{std::format("the speech's PCM rate {} is not usable", rate)};
        const std::size_t frames = body.size() / (2 * channels); // an odd last byte is dropped
        audio.sampleRate = rate;
        audio.samples.resize(frames);
        for (std::size_t f = 0; f < frames; ++f) {
            float sum = 0.0f;
            for (std::uint32_t c = 0; c < channels; ++c) {
                const std::size_t i = (f * channels + c) * 2;
                const auto s = std::int16_t(std::uint16_t(body[i]) | (std::uint16_t(body[i + 1]) << 8));
                sum += float(s) / 32768.0f;
            }
            audio.samples[f] = sum / float(channels);
        }
        return audio;
    }
    Result<audio::SoundData> decoded = audio::decodeSound(body);
    if (!decoded) return Error{std::format("the speech does not decode ({})", decoded.error())};
    const std::uint32_t channels = decoded->format.channels;
    audio.sampleRate = decoded->format.sampleRate;
    const std::size_t frames = std::size_t(decoded->frames());
    audio.samples.resize(frames);
    for (std::size_t f = 0; f < frames; ++f) {
        float sum = 0.0f;
        for (std::uint32_t c = 0; c < channels; ++c) sum += decoded->samples[f * channels + c];
        audio.samples[f] = sum / float(std::max<std::uint32_t>(1, channels));
    }
    return audio;
}

} // namespace quench::assistant
