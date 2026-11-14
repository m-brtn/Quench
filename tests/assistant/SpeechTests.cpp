// Speech-to-text and text-to-speech (quench/assistant/Speech.h) and the voice turn: the requests' bodies as the fake
// received them, PCM and MP3 answers decoded to the right length and rate, the settings and their presets.

#include "Rig.h"
#include "Test.h"

#include <quench/assistant/Speech.h>
#include <quench/audio/Sound.h>

#include <cmath>
#include <string>

using namespace quench;
using namespace quench::assistant;
using namespace assistant_test;

QUENCH_TEST(speech_base64) {
    const auto enc = [](std::string_view s) {
        return base64Encode({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
    };
    CHECK(enc("") == "" && enc("f") == "Zg==" && enc("fo") == "Zm8=" && enc("foo") == "Zm9v", "short");
    CHECK(enc("foobar") == "Zm9vYmFy" && enc("\xff\xfe\x00") == "//4=", "%s", enc("\xff\xfe").c_str());
}

QUENCH_TEST(speech_transcription_json_request) {
    Rig rig;
    const std::vector<std::uint8_t> recording = wav(1.5);
    rig.fake.respond("POST", kSttUrl,
                     jsonAnswer(200, R"({"text":"  Send walker one to the nearest cell.\n","usage":)"
                                     R"({"seconds":1.5,"cost":0.0001,"input_tokens":20,"output_tokens":9}})"));
    REQUIRE(rig.a.transcribe(recording), "transcribe");
    CHECK(rig.a.state() == State::Transcribing, "%s", stateName(rig.a.state()));
    REQUIRE(rig.run(), "not finished");
    CHECK(rig.a.state() == State::Done && rig.a.transcript() == "Send walker one to the nearest cell.", "'%s' %s",
          rig.a.transcript().c_str(), rig.a.failure().message.c_str());
    const std::vector<json::Value> bodies = rig.bodies(kSttUrl);
    REQUIRE(bodies.size() == 1, "%zu", bodies.size());
    const json::Value& b = bodies[0];
    CHECK(b["model"].stringOr("") == "openai/whisper-large-v3-turbo" && b["language"].stringOr("") == "en", "%s",
          compact(b).substr(0, 200).c_str());
    CHECK(b["input_audio"]["format"].stringOr("") == "wav", "format");
    CHECK(b["input_audio"]["data"].stringOr("") == base64Encode(recording), "the audio as base64");
    const std::string* type = net::findHeader(rig.fake.last()->request.headers, "Content-Type");
    CHECK(type && *type == "application/json", "Content-Type");
    const Stats& s = rig.a.stats();
    CHECK(std::abs(s.audioSeconds - 1.5) < 1e-6 && s.usage.costKnown && s.requests == 1, "%f", s.audioSeconds);
}

QUENCH_TEST(speech_transcription_multipart_request) {
    Rig rig;
    rig.a.settings() = openAiPreset();
    rig.a.settings().apiKey = kKey;
    const std::string url = "https://api.openai.com/v1/audio/transcriptions";
    rig.fake.respond("POST", url, jsonAnswer(200, R"({"text":"Hello there."})"));
    const std::vector<std::uint8_t> recording = wav(0.25);
    REQUIRE(rig.a.transcribe(recording), "transcribe");
    REQUIRE(rig.run(), "not finished");
    CHECK(rig.a.transcript() == "Hello there.", "'%s'", rig.a.transcript().c_str());
    const net::Request& r = rig.fake.last()->request;
    const std::string* type = net::findHeader(r.headers, "Content-Type");
    REQUIRE(type && type->starts_with("multipart/form-data; boundary="), "%s", type ? type->c_str() : "none");
    const std::string boundary = type->substr(std::string("multipart/form-data; boundary=").size());
    const std::string body = text(r.body);
    CHECK(body.starts_with("--" + boundary + "\r\n"), "the first boundary");
    CHECK(body.ends_with("\r\n--" + boundary + "--\r\n"), "the closing boundary");
    CHECK(body.find("name=\"model\"\r\n\r\nwhisper-1\r\n") != std::string::npos, "model");
    CHECK(body.find("name=\"language\"\r\n\r\nen\r\n") != std::string::npos, "language");
    CHECK(body.find("name=\"response_format\"\r\n\r\njson\r\n") != std::string::npos, "response_format");
    const std::string fileHead = "name=\"file\"; filename=\"speech.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    const std::size_t at = body.find(fileHead);
    REQUIRE(at != std::string::npos, "file part");
    CHECK(body.compare(at + fileHead.size(), recording.size(), text(recording)) == 0, "the file's bytes");
}

QUENCH_TEST(speech_transcription_answers) {
    CHECK(parseTranscription(R"({"text":"x"})").ok(), "text only");
    CHECK(!parseTranscription("not json"), "not JSON");
    CHECK(!parseTranscription(R"({"error":{"message":"no"}})"), "no text");
    const Result<Transcript> openAi = parseTranscription(R"({"text":"Hi","usage":{"type":"duration","seconds":2}})");
    CHECK(openAi && openAi->audioSeconds == 2.0 && !openAi->usage.costKnown, "OpenAI's usage");
}

QUENCH_TEST(speech_request_and_pcm_answer) {
    Rig rig;
    rig.a.settings().speechPrefix = "[flat, even, neutral tone]";
    rig.a.settings().speechSpeed = 1.1;
    // 0.5 s at 24 kHz, 16-bit: a ramp, so that the samples can be checked.
    std::string pcm;
    for (int i = 0; i < 12000; ++i) {
        const auto v = std::int16_t((i % 200) * 100 - 10000);
        pcm += char(std::uint16_t(v) & 0xFF);
        pcm += char(std::uint16_t(v) >> 8);
    }
    rig.fake.respond("POST", kTtsUrl,
                     net::FakeResponse::text(200, pcm, {{"Content-Type", "audio/pcm;rate=24000;channels=1"}}));
    REQUIRE(rig.a.speak("Walker 1 is on its way."), "speak");
    CHECK(rig.a.state() == State::Speaking, "%s", stateName(rig.a.state()));
    REQUIRE(rig.run(), "not finished");
    REQUIRE(rig.a.state() == State::Done, "%s", rig.a.failure().message.c_str());
    const json::Value body = rig.bodies(kTtsUrl).at(0);
    CHECK(body["model"].stringOr("") == "elevenlabs/eleven-v4-turbo" && body["voice"].stringOr("") == "matilda", "%s",
          compact(body).c_str());
    CHECK(body["input"].stringOr("") == "[flat, even, neutral tone] Walker 1 is on its way.", "%s",
          body["input"].stringOr("").c_str());
    CHECK(body["response_format"].stringOr("") == "pcm" && body["speed"].numberOr(0) == 1.1 &&
              !body.has("instructions"),
          "%s", compact(body).c_str());
    std::optional<SpeechAudio> audio = rig.a.takeSpeech();
    REQUIRE(audio, "no speech");
    CHECK(audio->sampleRate == 24000 && audio->samples.size() == 12000, "%u Hz, %zu samples", audio->sampleRate,
          audio->samples.size());
    CHECK(std::abs(audio->samples[1] - (100.0f - 10000.0f) / 32768.0f) < 1e-6f, "%f", double(audio->samples[1]));
    CHECK(std::abs(audio->seconds() - 0.5) < 1e-9 && audio->pcm16().size() == 12000, "seconds");
    CHECK(!rig.a.takeSpeech(), "taken once");
    const Stats& s = rig.a.stats();
    CHECK(s.spokenChars == 50 && std::abs(s.speechSeconds - 0.5) < 1e-9, "%lld chars", (long long)s.spokenChars);
}

QUENCH_TEST(speech_pcm_rates_and_channels) {
    std::vector<std::uint8_t> bytes(4 * 1000 + 1, 0); // 1000 stereo frames and an odd byte
    Result<SpeechAudio> a = decodeSpeech(bytes, "audio/pcm; rate=22050; channels=2", SpeechFormat::Pcm, 24000);
    REQUIRE(a, "%s", a.error().c_str());
    CHECK(a->sampleRate == 22050 && a->samples.size() == 1000, "%u %zu", a->sampleRate, a->samples.size());
    Result<SpeechAudio> b = decodeSpeech(bytes, "", SpeechFormat::Pcm, 16000);
    CHECK(b && b->sampleRate == 16000 && b->samples.size() == 2000, "the settings' rate without a Content-Type");
    CHECK(!decodeSpeech(bytes, "application/json", SpeechFormat::Pcm, 24000), "JSON is not audio");
    CHECK(!decodeSpeech({}, "audio/pcm", SpeechFormat::Pcm, 24000), "empty");
}

QUENCH_TEST(speech_mp3_answer) {
    Rig rig;
    rig.a.settings().speechFormat = SpeechFormat::Mp3;
    const std::span<const std::uint8_t> mp3 = quench::test::toneMp3();
    Result<audio::SoundData> reference = audio::decodeSound(mp3);
    REQUIRE(reference, "%s", reference.error().c_str());
    rig.fake.respond("POST", kTtsUrl, net::FakeResponse::text(200, text(mp3), {{"Content-Type", "audio/mpeg"}}));
    REQUIRE(rig.a.speak("Tone."), "speak");
    REQUIRE(rig.run(), "not finished");
    REQUIRE(rig.a.state() == State::Done, "%s", rig.a.failure().message.c_str());
    CHECK(rig.bodies(kTtsUrl).at(0)["response_format"].stringOr("") == "mp3", "response_format");
    std::optional<SpeechAudio> audio = rig.a.takeSpeech();
    REQUIRE(audio, "no speech");
    CHECK(audio->sampleRate == reference->format.sampleRate && audio->samples.size() == reference->frames(),
          "%u Hz %zu frames, expected %u Hz %llu", audio->sampleRate, audio->samples.size(),
          reference->format.sampleRate, (unsigned long long)reference->frames());
    // Not an MP3 after all.
    rig.fake.respond("POST", kTtsUrl, net::FakeResponse::text(200, "garbage", {{"Content-Type", "audio/mpeg"}}));
    REQUIRE(rig.a.speak("Again."), "speak");
    rig.run();
    CHECK(rig.a.failure().kind == ErrorKind::BadResponse, "%s", errorKindName(rig.a.failure().kind));
}

QUENCH_TEST(speech_voice_turn) {
    Rig rig;
    int called = 0;
    Tool t;
    t.name = "get_status";
    t.description = "Status.";
    t.handler = [&](const json::Value&) {
        ++called;
        return ToolResult::ok("power +12 kW");
    };
    REQUIRE(rig.a.tools().add(std::move(t)), "add");
    rig.fake.respond("POST", kSttUrl, jsonAnswer(200, R"({"text":"What is my power balance?"})"));
    rig.fake.respond("POST", kChatUrl, stream(sseTools({{"v1", "get_status", "{}"}})));
    rig.fake.respond(
        "POST", kTtsUrl,
        net::FakeResponse::text(200, std::string(9600, '\0'), {{"Content-Type", "audio/pcm;rate=24000;channels=1"}}));
    REQUIRE(rig.a.askByVoice(wav(2.0)), "voice");
    REQUIRE(rig.runUntil([&] { return rig.a.state() == State::CallingTool; }), "no tool");
    CHECK(rig.a.transcript() == "What is my power balance?", "'%s'", rig.a.transcript().c_str());
    rig.fake.respond("POST", kChatUrl, stream(sseText({"Plus twelve kilowatts."})));
    REQUIRE(rig.run(), "not finished");
    REQUIRE(rig.a.state() == State::Done, "%s", rig.a.failure().message.c_str());
    CHECK(called == 1 && rig.a.answer() == "Plus twelve kilowatts.", "%s", rig.a.answer().c_str());
    std::optional<SpeechAudio> audio = rig.a.takeSpeech();
    CHECK(audio && audio->samples.size() == 4800, "speech");
    // The states in order, each one at least once.
    std::vector<State> order;
    for (const Event& e : rig.a.takeEvents()) {
        if (e.kind == EventKind::StateChanged) order.push_back(e.state);
    }
    CHECK(order == (std::vector<State>{State::Transcribing, State::Thinking, State::CallingTool, State::Thinking,
                                       State::Speaking, State::Done}),
          "%zu state changes", order.size());
    const Stats& s = rig.a.stats();
    CHECK(s.requests == 4 && s.rounds == 2 && s.toolCalls == 1 && std::abs(s.audioSeconds - 2.0) < 1e-6 &&
              s.spokenChars == 22,
          "%d requests, %lld chars", s.requests, (long long)s.spokenChars);
    CHECK(s.transcribeTime > 0 && s.thinkTime > 0 && s.speakTime > 0 && s.totalTime >= s.thinkTime, "times");
    CHECK(rig.a.totals().requests == 4, "totals");
    // The history keeps what the player said, as transcribed.
    CHECK(rig.a.historyJson()["turns"].array[0].array[0]["content"].stringOr("") == "What is my power balance?",
          "history");
}

QUENCH_TEST(speech_nothing_heard) {
    Rig rig;
    rig.fake.respond("POST", kSttUrl, jsonAnswer(200, R"({"text":"  "})"));
    REQUIRE(rig.a.askByVoice(wav(1.0)), "voice");
    REQUIRE(rig.run(), "not finished");
    CHECK(rig.a.state() == State::Failed && rig.a.failure().kind == ErrorKind::NothingHeard, "%s",
          errorKindName(rig.a.failure().kind));
    CHECK(rig.bodies(kChatUrl).empty(), "no chat request");
    CHECK(!rig.a.askByVoice({}), "an empty recording is refused");
}

QUENCH_TEST(speech_settings_and_presets) {
    ProviderSettings s = openRouterPreset();
    s.apiKey = kKey;
    s.speechPrefix = "[flat]";
    s.extraHeaders = {{"X-Title", "Test Game"}, {"X-Api-Key", "secret"}};
    const json::Value saved = settingsToJson(s);
    const std::string written = json::write(saved);
    CHECK(written.find(kKey) == std::string::npos && written.find("secret") == std::string::npos, "%s",
          written.c_str());
    ProviderSettings base = localPreset();
    base.apiKey = "kept";
    const ProviderSettings back = settingsFromJson(saved, base);
    CHECK(back.apiKey == "kept" && back.baseUrl == s.baseUrl && back.chatModel == s.chatModel &&
              back.speechPrefix == "[flat]" && back.transcriptionForm == TranscriptionForm::Json &&
              back.extraHeaders.size() == 1,
          "round trip");
    CHECK(openAiPreset().transcriptionForm == TranscriptionForm::Multipart &&
              localPreset().baseUrl.starts_with("http://localhost"),
          "presets");
    CHECK(redact("a sk-123456 b sk-123456", "sk-123456") == "a <key> b <key>", "%s",
          redact("a sk-123456 b sk-123456", "sk-123456").c_str());
    // A local server: no key, no Authorization header.
    ProviderSettings local = localPreset("http://127.0.0.1:9000/v1/");
    local.chatModel = "local-model";
    const net::Request r = buildChatRequest(local, json::Value::makeArray(), json::Value::makeArray());
    CHECK(r.url == "http://127.0.0.1:9000/v1/chat/completions" && !net::findHeader(r.headers, "Authorization"), "%s",
          r.url.c_str());
}

// The token limit's name and reasoning_effort per preset (since 0.19.1): OpenAI's own API wants
// max_completion_tokens, and its reasoning models take function tools on chat completions only with
// reasoning_effort "none"; OpenRouter's request stays as it was.
QUENCH_TEST(speech_chat_token_limit_and_reasoning_per_preset) {
    json::Value messages = json::Value::makeArray();
    messages.array.push_back(parse(R"({"role":"user","content":"Hi"})"));
    json::Value tools = json::Value::makeArray();
    tools.array.push_back(parse(R"({"type":"function","function":{"name":"get_status","description":"Status.",)"
                                R"("parameters":{"type":"object","properties":{}}}})"));

    ProviderSettings openAi = openAiPreset();
    CHECK(openAi.maxTokensField == "max_completion_tokens" && openAi.reasoningEffort == "none", "the preset");
    openAi.maxTokens = 300;
    const json::Value a = parse(text(buildChatRequest(openAi, messages, tools).body));
    CHECK(a["max_completion_tokens"].numberOr(0) == 300 && !a.has("max_tokens"), "%s", compact(a).c_str());
    CHECK(a["reasoning_effort"].stringOr("") == "none" && a["tools"].isArray(), "%s", compact(a).c_str());

    ProviderSettings router = openRouterPreset();
    CHECK(router.maxTokensField == "max_tokens" && router.reasoningEffort.empty(), "OpenRouter's preset");
    router.maxTokens = 300;
    const std::string body = text(buildChatRequest(router, messages, tools).body);
    const json::Value b = parse(body);
    CHECK(b["max_tokens"].numberOr(0) == 300 && !b.has("max_completion_tokens") && !b.has("reasoning_effort"), "%s",
          body.c_str());
    // Byte for byte what 0.19.0 sent: model, the settings' keys, tools, messages, tool_choice.
    CHECK(body.starts_with(R"({"model":"anthropic/claude-haiku-5.5","max_tokens":300,"parallel_tool_calls":true,)"
                           R"("stream":true,"stream_options":{"include_usage":true},"tools":[)"),
          "%s", body.c_str());
    router.reasoningEffort = "low";
    CHECK(parse(text(buildChatRequest(router, messages, tools).body))["reasoning_effort"].stringOr("") == "low",
          "sent when set");
    router.maxTokens = 0;
    const json::Value none = parse(text(buildChatRequest(router, messages, tools).body));
    CHECK(!none.has("max_tokens") && !none.has("max_completion_tokens"), "no limit: neither name");

    // Saved and read back; an unknown name for the limit is not taken.
    ProviderSettings saved = openAiPreset();
    saved.reasoningEffort = "low";
    const ProviderSettings back = settingsFromJson(settingsToJson(saved), openRouterPreset());
    CHECK(back.maxTokensField == "max_completion_tokens" && back.reasoningEffort == "low", "round trip");
    json::Value odd = settingsToJson(openRouterPreset());
    odd.object["maxTokensField"] = json::Value::makeString("max_output_tokens");
    CHECK(settingsFromJson(odd, openAiPreset()).maxTokensField == "max_completion_tokens", "the base's name kept");
}

// A model without reasoning (OpenAI's gpt-4.1-mini on the OpenAI preset) refuses reasoning_effort as an unknown
// argument (HTTP 400, seen on 2026-11-14): the round goes again once without it, and so do that model's later
// requests; a refusal of the value itself is a failure as before.
QUENCH_TEST(speech_chat_model_without_reasoning_effort) {
    Rig rig;
    rig.a.settings() = openAiPreset();
    rig.a.settings().apiKey = kKey;
    rig.a.settings().chatModel = "gpt-4.1-mini";
    const std::string url = "https://api.openai.com/v1/chat/completions";
    const std::string refusal = R"({"error":{"message":"Unrecognized request argument supplied: reasoning_effort",)"
                                R"("type":"invalid_request_error","param":null,"code":null}})";
    rig.fake.respond("POST", url, jsonAnswer(400, refusal));
    REQUIRE(rig.a.ask("Hello"), "ask");
    rig.fake.respond("POST", url, stream(sseText({"Hi."}))); // the first request already has its answer
    REQUIRE(rig.run(), "not finished");
    CHECK(rig.a.state() == State::Done && rig.a.answer() == "Hi.", "%s", rig.a.failure().message.c_str());
    std::vector<json::Value> bodies = rig.bodies(url);
    REQUIRE(bodies.size() == 2, "%zu requests", bodies.size());
    CHECK(bodies[0]["reasoning_effort"].stringOr("") == "none" && !bodies[1].has("reasoning_effort"), "%s",
          compact(bodies[1]).c_str());
    CHECK(rig.a.stats().rounds == 1 && rig.a.stats().requests == 2, "rounds %d, requests %d", rig.a.stats().rounds,
          rig.a.stats().requests);
    REQUIRE(rig.a.ask("Again") && rig.run(), "a second turn");
    bodies = rig.bodies(url);
    CHECK(bodies.size() == 3 && !bodies[2].has("reasoning_effort"), "the model's later requests leave it out");

    // Another model, and a refusal of the value: no second request.
    rig.a.settings().chatModel = "gpt-6-luna";
    rig.a.settings().reasoningEffort = "low";
    rig.fake.respond("POST", url,
                     jsonAnswer(400, R"({"error":{"message":"Function tools with reasoning_effort are not supported )"
                                     R"(for gpt-6-luna in /v1/chat/completions. To use function tools, use )"
                                     R"(/v1/responses or set reasoning_effort to 'none'."}})"));
    REQUIRE(rig.a.ask("Third"), "ask");
    rig.run();
    CHECK(rig.a.state() == State::Failed && rig.a.failure().kind == ErrorKind::BadRequest &&
              rig.bodies(url).size() == 4,
          "%s, %zu", rig.a.failure().message.c_str(), rig.bodies(url).size());
    CHECK(rig.bodies(url)[3]["reasoning_effort"].stringOr("") == "low", "the game's value sent");
}
