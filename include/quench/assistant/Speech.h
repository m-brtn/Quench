#pragma once

// Speech-to-text and text-to-speech requests and their answers (docs/assistant.md, "Speech"): a recording (a WAV
// file in memory, audio::MicRecording::wav()) to text, and text to mono samples ready for the game's audio system.
// The module neither records nor plays.

#include <quench/assistant/Chat.h> // Usage
#include <quench/assistant/Provider.h>
#include <quench/core/Result.h>
#include <quench/net/Http.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quench::assistant {

// POST <baseUrl>/audio/transcriptions in the settings' form: JSON {model, input_audio:{data, format:"wav"},
// language} or a multipart form with model, language, response_format=json and the file.
net::Request buildTranscriptionRequest(const ProviderSettings& settings, std::span<const std::uint8_t> wav);

struct Transcript {
    std::string text;          // trimmed
    double audioSeconds = 0.0; // as the provider counted it, 0 when it did not say
    Usage usage;
};
// {"text": ..., "usage": {...}} (OpenRouter's and OpenAI's json answer).
Result<Transcript> parseTranscription(std::string_view body);

// POST <baseUrl>/audio/speech: {model, input: prefix + " " + text, voice, response_format, speed?, instructions?}.
net::Request buildSpeechRequest(const ProviderSettings& settings, std::string_view text);
// The text a speech request sends for `text` (with the prefix).
std::string speechInput(const ProviderSettings& settings, std::string_view text);

struct SpeechAudio {
    std::uint32_t sampleRate = 0;
    std::vector<float> samples; // mono, -1 .. 1

    double seconds() const { return sampleRate ? double(samples.size()) / sampleRate : 0.0; }
    std::vector<std::int16_t> pcm16() const;
};

// The audio of a speech answer, by its Content-Type: audio/pcm (rate and channels from its parameters, else
// `pcmRate`, mono; 16-bit little endian), audio/mpeg, audio/wav, audio/flac or audio/ogg through the engine's decoder
// (stereo mixed down); without a Content-Type the settings' format decides.
Result<SpeechAudio> decodeSpeech(std::span<const std::uint8_t> body, std::string_view contentType,
                                 SpeechFormat expected, std::uint32_t pcmRate);

// Standard base64 with padding.
std::string base64Encode(std::span<const std::uint8_t> bytes);
// Standard base64 back to bytes (padding optional, no whitespace); an error for any other character.
Result<std::vector<std::uint8_t>> base64Decode(std::string_view text);

} // namespace quench::assistant
