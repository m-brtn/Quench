# Assistant: a language model that operates a game

`quench_assistant` (since 2026-11-13, released with 0.18.0) lets a language model operate a game through tools
the game registers: the player types or says something ("send walker one to the nearest cell", "what is my power
balance?"), the model calls the game's tools, the game executes them, and the model answers, as text and optionally as
speech. It is a client for OpenAI-compatible APIs (chat completions with tool calls, speech-to-text, text-to-speech)
on [`quench_net`](net.md): OpenRouter first, OpenAI's own API or a local server by settings only. Turn-based (push to
talk); a live speech-to-speech session is a later, separate backend.

Public headers in `include/quench/assistant/`: `Assistant.h` (the turn), `Tools.h` (the tools), `Provider.h` (the
settings, presets and errors), `Chat.h` (the chat request and the stream parser), `Speech.h` (transcription and
speech). The target is headless (core, net and, for MP3 speech, the audio module's decoder); `quench_add_game()` links
it into every game, like `quench_net`, so a game only includes it. The module neither records nor plays: the app's
microphone (`App::microphone()`, [Audio](audio.md#microphone)) and audio system do.

## Use

```cpp
#include <quench/assistant/Assistant.h>
namespace qa = quench::assistant;

qa::Assistant m_assistant;              // a member; m_assistant.update() once a frame in Game::update

m_assistant.settings() = qa::openRouterPreset();
m_assistant.settings().apiKey = playerKey;          // from the game's settings UI; memory only
m_assistant.setSystemPrompt("You are the station's assistant. Use the tools. Answer in one short sentence.");
m_assistant.setStateSummary([this] { return colony.summary(); });   // fresh for each turn
(void)m_assistant.tools().add({.name = "get_power", .description = "The colony's power balance in kW.",
    .annotations = {.readOnly = true},
    .handler = [this](const json::Value&) { return qa::ToolResult::ok(colony.powerJson()); }});
(void)m_assistant.tools().add({.name = "order_collect", .description = "Send a walker to collect a cell.",
    .inputSchema = json::parse(R"({"type":"object","properties":{"unit":{"type":"string"},
        "item":{"type":"string"}},"required":["unit","item"]})").value(),
    .handler = [this](const json::Value& a) { return colony.collect(a["unit"].string, a["item"].string); }});

// add() refuses a bad name, a duplicate or a schema that is not an object schema (a Result)
if (Result<void> r = m_assistant.ask("Send walker one to the nearest cell"); !r) { /* busy, not configured */ }
// or askByVoice(recording.wav()); askByVoice and ask(text, {.speak = true}) also give speech
// every frame: state(), toolName(), partialText(), answer(), takeEvents(), takeSpeech()
```

## Settings and presets

`ProviderSettings`: the base URL (the paths `/chat/completions`, `/audio/transcriptions`, `/audio/speech` are appended),
the key (sent as `Authorization: Bearer`; empty sends none, for a local server), the chat, transcription and speech
models, the voice, a text prefix for speech (an audio tag such as `[flat, even, neutral tone]` for a model that reads
them; not in the answer the player reads), speech instructions and speed (sent only when set), the speech format
(`pcm`: 16-bit mono, the rate from the answer's `Content-Type`, else `pcmRate`, 24 kHz; or `mp3`, decoded by the
engine), the transcription form (JSON with base64 audio, or a multipart form), a language hint, a timeout per kind of
request (30 s; 120 s for the local preset), streaming on or off, `stream_options.include_usage`,
`parallel_tool_calls`, `max_tokens`, `temperature`, extra headers (OpenRouter's `HTTP-Referer` and `X-Title`).

| Preset | Base URL | Chat | Transcription | Speech | Form |
|---|---|---|---|---|---|
| `openRouterPreset()` | `https://openrouter.ai/api/v1` | `anthropic/claude-haiku-5.5` | `openai/whisper-large-v3-turbo` | `elevenlabs/eleven-v4-turbo`, `matilda`, pcm | JSON |
| `openAiPreset()` | `https://api.openai.com/v1` | `gpt-6-luna` | `whisper-1` | `gpt-4o-mini-tts`, `alloy`, pcm | multipart |
| `localPreset(url)` | `http://localhost:8080/v1` | (the game's) | (the game's) | (the game's) | multipart |

The OpenRouter preset's models are the ones measured on 2026-11-13 (below). The OpenAI preset's model names were not
checked against OpenAI's API, and nothing has run against it or against a local server. Plain `http` is allowed to
`localhost`, `127.0.0.1` and `[::1]` only (`quench_net`'s rule); an app bundle on Apple platforms also needs ATS's
`NSAllowsLocalNetworking` for it ([Network](net.md)).

`settingsToJson()` and `settingsFromJson()` save and read everything but the key (and extra headers whose names look
secret), for a game's settings file.

## Tools and MCP

A tool is what the Model Context Protocol calls one: a `name` (1 to 64 of `a-z A-Z 0-9 _ -`), a `description`, an
`inputSchema` (JSON Schema as JSON, of type `object`), `annotations` (MCP's hints: `title`, `readOnlyHint`,
`destructiveHint`, `idempotentHint`, `openWorldHint`) and a handler from the arguments to a `ToolResult`: text, JSON
(MCP's `structuredContent`; the model reads its compact text) or an error with a message the model can act on.

- `ToolRegistry::list()` returns what MCP's `tools/list` returns (`{"tools":[{name, title?, description, inputSchema,
  annotations?}]}`); `ToolRegistry::callResult()` turns a result into `tools/call`'s (`{"content":[{"type":"text",
  "text"}], "structuredContent"?, "isError"}`). `chatTools()` is the chat request's `tools` array.
- Arguments are checked against the schema before the handler runs: `type` (also a list of types), `properties`,
  `required`, `additionalProperties: false`, `enum`, `const`, `minimum`, `maximum`, `exclusiveMinimum`,
  `exclusiveMaximum`, `minLength`, `maxLength`, `items`, `minItems`, `maxItems`; other keywords go to the model
  unchecked. An unknown tool, arguments that are not a JSON object or fail the schema, and a handler's exception (not in the
  web builds, which do not catch exceptions: there it aborts, as anywhere in a web build) are error results with a readable message ("invalid arguments for order_collect: unit: missing (required)"), never a
  crash; the model gets them as the tool's result (`Error: ...`) and can try again in the next round.
- Handlers run on the main thread, from `Assistant::update()`.

What an MCP transport would still need: JSON-RPC 2.0 framing and its errors, the `initialize` handshake and
capabilities, a transport (stdio or streamable HTTP; a game would serve, so a listening socket, which `quench_net`
does not have), `notifications/tools/list_changed`, pagination of `tools/list` (`cursor`), the `tools/call` request's
`_meta` and progress, and authorization for HTTP. The registry, the schemas, the annotations and the result shapes
stay as they are.

## The turn

`ask(text)` (typed) or `askByVoice(wav)` (a recording, `audio::MicRecording::wav()`):

1. `Transcribing` (voice only): the recording goes to speech-to-text; an empty transcript fails with `NothingHeard`.
2. `Thinking`: a chat request with the system prompt, the history, this turn's user message (the state summary first,
   `Game state:\n...\n\nPlayer: ...`, called once per turn and not kept in the history), the tools, `tool_choice`
   `auto`, streamed; `partialText()` grows as the text arrives.
3. `CallingTool`: each tool call of the answer, in order (parallel calls of one round are all executed), one per
   `update()` and visible for at least one before it runs (`toolName()`, `ToolStarted` and `ToolFinished` events,
   `toolCalls()` with their arguments and results); then the results go back (the assistant message with its
   `tool_calls`, a `tool` message per call, the tools again) as the next round.
4. Until the model answers with text or the round limit (`Limits::maxRounds`, 3): the last round sends `tool_choice`
   `none`, so the model must answer; tool calls on it are not executed; without text the turn fails with `RoundLimit`.
5. `Speaking` (with `AskOptions::speak`): the answer (already final in `answer()`) to text-to-speech; `takeSpeech()`
   gives mono float samples and their rate (`pcm16()` for 16-bit), ready for the game's audio system.
6. `Done`, or `Failed` with `failure()`.

The stages alone: `transcribe(wav)` (to `transcript()`), `ask(text)` without speech (a typed-text game uses only this),
`speak(text)`.

**One operation at a time**: a second `ask`, `askByVoice`, `transcribe` or `speak` while one runs is refused with an
error that says `busy` (not queued: a voice turn that waited behind another would answer a moment that has passed;
the game decides, and can `cancel()` first). **Cancel** works in every state: the request in flight is cancelled,
the tool calls not executed yet are dropped (those already executed keep their effects in the game), the state goes
back to `Idle` with a `Cancelled` event and `failure().kind == Cancelled`, and the turn is not added to the history
(a turn cancelled while its answer is spoken is: its answer was final). A handler may call `cancel()`.

Observable every frame: `state()` (`idle`, `transcribing`, `thinking`, `calling tool`, `speaking`, `done`, `failed`),
`toolName()`, `transcript()`, `partialText()`, `answer()`, `toolCalls()`, `failure()`, `takeEvents()` (state changes,
`Transcribed`, `ToolStarted`, `ToolFinished`, `Answered`, `SpeechReady`, `Failed`, `Cancelled`) and the counters:
`stats()` of the last operation and `totals()` (requests, rounds, tool calls, prompt and completion tokens, the
provider's cost when it says (OpenRouter does for chat and transcription, not for speech), seconds of audio
transcribed and received, characters sent to speech, and the seconds of each stage, the first text and the whole).

**History**: finished turns as chat messages (the player's words, the assistant's tool calls, the tool results, the
answer), the last `Limits::historyTurns` (6) of them; a tool result is cut to `toolResultChars` (4000 characters) in
its own turn and to `historyToolResultChars` (400) once in the history ("... (N more characters)"). A failed or
cancelled turn is not kept. `historyJson()` and `setHistory()` save and restore it (no system prompt, no state
summaries, no key); `clearHistory()`.

## The chat client

`buildChatRequest()` writes the body (compact JSON); `ChatStreamParser` reads the answer incrementally from pieces that
end anywhere (in the middle of a line, of a UTF-8 character or of a JSON string): server-sent events (`data:` lines,
several joined, `:` comments as keep-alives ignored, `event:` and `id:` ignored, `\r\n` or `\n`), `data: [DONE]`, text
deltas, tool calls gathered by their `index` (the first delta has the id and the name, later ones the argument
fragments; sorted by index at the end), the finish reason, the usage of the last chunk (`prompt_tokens`,
`completion_tokens`, OpenRouter's `cost`). A body that starts with `{` is read as one JSON answer (a server that does
not stream, `stream: false`). A stream that ends without `[DONE]` is complete when it had a finish reason; without
either it is a `StreamError`. JSON's `\u` surrogate pairs (an emoji from a server that escapes non-ASCII) arrive as
UTF-8: `quench::json` decodes a pair to one code point and a lone surrogate to U+FFFD.

## Errors

`Failure{kind, status, message}`; the message is readable for a player or a log, with the provider's own words when it
sent them (`{"error":{"message"}}`, OpenRouter's `metadata.raw`), at most 300 characters, never the key.

| Kind | When |
|---|---|
| `BadKey` | HTTP 401 |
| `NoCredits` | 402 |
| `Refused` | 403 (OpenRouter: a model's moderation flagged the input, a key without access) |
| `NotFound` | 404: the base URL or the model name |
| `BadRequest` | another 4xx |
| `RateLimited` | 429 |
| `ProviderDown` | 5xx |
| `StreamError` | an error chunk in the middle of a stream (`error` and `finish_reason: "error"`), a stream that ended early |
| `BadResponse` | not JSON, no choices, no text, audio that does not decode |
| `Timeout`, `NoNetwork`, `NotAllowed`, `TooLarge`, `NoBackend` | from `quench_net` (408 and 413 also map to `Timeout` and `TooLarge`) |
| `RoundLimit`, `NothingHeard`, `Cancelled` | the turn's own |

`ask()` and the others return an error at once (and change nothing) when busy, when the settings lack a base URL or
the model the operation needs, or for an empty text or recording.

## Privacy: what leaves the machine

Nothing until the game calls `ask`, `askByVoice`, `transcribe` or `speak`. Then, to the base URL in the settings only
(OpenRouter forwards to the model's provider): the recording (`askByVoice`, `transcribe`), and with each chat request
the system prompt, the state summary of this turn, the history (the last turns' words, tool calls and trimmed
results), the player's words, the tools' names, descriptions and schemas and this turn's tool results; for speech the
answer's text. The key goes in the `Authorization` header of each request. The log (category `assistant`, beside
`quench_net`'s line per request) has counts, statuses, tool names, tokens, cost and milliseconds, never the key, the
player's words, the answer or a tool's arguments or result. The key is kept in `settings()` only; it is not in
`settingsToJson()`, `historyJson()` or a `Failure`'s message (`redact()` takes it out of a provider's message that
echoes it).

## The manual check

`build/release/examples/assistant_check/quench-assistant-check "TEXT" [--speak] [--transcribe] [--wav FILE]
[--chat-model M] [--no-stream]` (macOS, a command line program without a window): one typed turn with two invented
tools of a test colony (`get_status`, `order_collect`), optionally the answer spoken (`--speak`) and that speech
transcribed back (`--transcribe`), or a voice turn from a 16 kHz WAV file (`--wav`); it prints each stage with its
milliseconds, the text as it streams, the tool calls and their results, the answer and the counters. The key comes
from the environment variable `OPENROUTER_API_KEY` only. It sends requests to OpenRouter and costs a little; no
automated check runs it.

Run on the Mac on 2026-11-13 through NSURLSession (OpenRouter preset, `anthropic/claude-haiku-5.5`,
`openai/whisper-large-v3-turbo`, `elevenlabs/eleven-v4-turbo`):

- "What is the power balance? Then send walker one to its nearest energy cell." with `--transcribe`: round 1
  `get_status` (1412 ms), round 2 `order_collect {"unit": "Walker 1", "item": "E7"}` (977 ms), round 3 the answer
  (902 ms; its 94 characters arrived as 4 deltas between 760 and 900 ms after the request), speech 7.68 s of 24 kHz
  PCM in 1251 ms, the whole turn 4547 ms; the speech resampled to 16 kHz and transcribed back word for word in
  1039 ms. 2037 + 209 tokens, $0.000334 reported (speech not reported).
- A voice turn from a 3 s recording of "Send walker one to the nearest energy cell.": transcription 585 ms, the same
  three rounds (1036, 941, 1036 ms), speech 5.12 s in 996 ms, 4601 ms in all; $0.000411 reported.

The model called the read-only tool and the order in two rounds, not in parallel: a compound order uses all three
rounds of the default limit.

## Tests

`assistant.*` (label `assistant`, `tests/assistant/`, all on `net::FakeNetwork`; also in the `headless` and
`wasm-node` builds):

- the stream parser: two fixtures in the shape of recorded OpenRouter streams (`tests/assistant/data/`), cut at every
  byte and in pieces of 1 to 17 bytes, arguments split across deltas in the middle of strings and characters,
  interleaved and reordered parallel calls, keep-alive comments and `\r\n`, a mid-stream error, a missing `[DONE]`, a
  bad event, a whole JSON answer, surrogate pairs, the compact writer;
- the tools: `tools/list` and `tools/call` shapes, refused registrations, every schema keyword, bad calls as error
  results;
- the turn: one tool, two parallel tools (in order, each visible for an update), two rounds (the last with
  `tool_choice: "none"`), the round limit, a handler's error, arguments that fail the schema or are not JSON, an
  unknown tool, the state summary, history trimming and its save, one operation at a time, cancel while transcribing,
  thinking (before and in the middle of the stream), calling a tool, speaking, and from a handler, a shared client
  that outlives the assistant, a non-streamed answer;
- HTTP failures (401, 402, 403, 404, 400, 429, 500, 503, streamed and not), a timeout, no network, a mid-stream error,
  a URL the HTTPS rule refuses, missing settings; the key in no log line, error, saved history or saved settings
  (searched in the captured log), and no player words or answers in the log;
- transcription bodies (JSON with base64, multipart with its parts and the file's bytes), speech bodies (prefix, voice,
  format, speed), PCM answers (rate and channels from the `Content-Type`, the settings' rate without one) and an MP3
  answer (the audio tests' tone, decoded to the decoder's length and rate), a voice turn's states and counters,
  nothing heard, the presets and the settings' save.

## Cost on the main thread

Per frame without a request: nothing but `net::Client::update()`. While a stream arrives: each piece is scanned for
line ends and each event's JSON parsed (a few hundred bytes; the messages of a turn are rebuilt and written once per
round, a few kilobytes with the history). Tool handlers are the game's code, one per `update()`. A speech answer is
decoded on the main thread in the `update()` that receives it (PCM: a copy; MP3 of a few seconds: miniaudio's decoder,
expected around a millisecond, not measured). Not measured in a frame.

## Not there

A live speech-to-speech session (a realtime WebSocket or WebRTC backend), an MCP transport (above), OAuth sign-in
(OpenRouter's PKCE flow), keeping the key (in the system's keychain or anywhere else: the game decides, and should
not put it in a save), any UI, recording and playback (the app's), voice activity detection, streaming speech
playback while it is generated, the Responses API, images, a queue of turns, async tool handlers (a handler returns
its result at once), retries on 429 or 5xx. Not run: the web build against a real provider (CORS was measured as
allowed by OpenRouter, the fetch backend only ran against the test page's server), OpenAI's own API, a local server,
iOS (neither the Simulator nor an iPhone), Windows and Linux (no network backend there yet).
