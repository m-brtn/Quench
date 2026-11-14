# Live session: a spoken conversation with GPT-Live

`assistant::LiveSession` (`include/quench/assistant/Live.h`, `quench_assistant`, since 2026-11-14, released with 0.19.0) is a
live, full-duplex spoken conversation with OpenAI's GPT-Live (`gpt-live-1`): the player talks to the game at any
time, the voice model listens all the time, answers by voice, yields when interrupted, and hands reasoning and the
game's tools to a backend model. It is the other way to talk to a game beside the turn-based
[Assistant](assistant.md) (record, transcribe, a chat model with tools, synthesise: about 4 s a turn). The tools are
the same `ToolRegistry`, with the same schema check.

Everything about the API comes from OpenAI's documentation read on 2026-11-14 (`developers.openai.com/api/docs/guides/
live`, `live-delegation`, `live-conversations`, `live-prompting`, `voice-websockets?api=live` and the reference
`api/reference/resources/live/primary-websocket`); what the documentation does not say and was seen in the manual
check is marked as seen.

## What it is

- **Transport.** One WebSocket (`net::WebSocket`, [Network](net.md#websocket)) to
  `wss://api.openai.com/v1/live/sessions` with `Authorization: Bearer <key>`, no query parameters, no subprotocol.
  The first message is `session.start` with the whole configuration; the session waits for `session.started`. Audio
  goes both ways as base64 PCM16 mono inside JSON text messages (`session.input_audio.append`,
  `session.output_audio.delta`), 24 kHz (or 16 kHz), fixed at the start.
- **Two models.** The voice model talks; when it decides a request needs work (the docs' wording for `gpt-live-1`:
  it "generates `<|SpawnThinking|>`") it delegates to a backend. With Responses delegation (the one used here)
  OpenAI runs the backend model (`gpt-6-luna` by default) with the game's function tools; its function calls arrive
  inside `response.event` envelopes as `response.output_item.done` items of type `function_call`; the session runs
  each through the registry on the main thread, sends `response.item.create` with a `function_call_output` for every
  call, then one `response.create` to continue (parallel calls included). The backend's text goes to the voice model
  by itself.
- **Interruptions** are the model's own: there is no turn detection to set, no cancel, no truncate, no speech-started
  event. When the player talks over the station the model yields; the client can only drop its own queued audio.
- **Headless.** The session neither records nor plays: the game feeds the microphone and plays the audio.

## Use

```cpp
#include <quench/assistant/Live.h>
#include <quench/audio/VoiceStream.h>

assistant::LiveSession m_live;                    // members
std::shared_ptr<audio::VoiceStream> m_voice = std::make_shared<audio::VoiceStream>();   // 24 kHz, 80 ms buffer
std::vector<float> m_buf;

// once, when the player turns the station's voice on (a key the player typed in, never saved by the engine)
m_live.settings().apiKey = playerKey;
m_live.settings().instructions = "You are the station's computer. Speak calmly, briefly. Speak English unless the "
                                 "player asks to switch.";
m_live.settings().backendInstructions = "You operate the station through its tools. Check get_status first.";
m_live.tools().add(getStatusTool);                // assistant::Tool, as for the Assistant
m_live.tools().add(sendRoverTool);
m_live.setStateSummary([this] { return describeStation(); });
app.audio().playGenerator(m_voice, {.bus = app.audio().bus("voice")});
app.microphone().start({.sampleRate = 24000, .stream = true});
m_live.start();

// every frame in Game::update (the app updates the microphone before it)
m_buf.clear(); app.microphone().takeStream(m_buf); m_live.sendAudio(m_buf);
m_live.update();
m_buf.clear(); m_live.takeAudio(m_buf); m_voice->push(m_buf);
m_live.setPlaybackActive(m_voice->audible());
for (const assistant::LiveEvent& e : m_live.takeEvents()) { /* captions, the tool shown, closed, failed */ }

// the game's own events
m_live.appendCommentary("Rover 2 has arrived at E7.");   // to be said aloud (paraphrased)
m_live.appendThinking("A dust storm starts in 3 minutes."); // known, not said now
```

## Settings

`LiveSettings` (the key in memory only; nothing in the module writes it anywhere):

| Field | Default | What |
|---|---|---|
| `url` | `wss://api.openai.com/v1/live/sessions` | |
| `apiKey` | | sent as `Authorization: Bearer`; never logged, never in an error, a history or an event |
| `model`, `voice` | `gpt-live-1`, `marin` | fixed for a session; 38 built-in voices (the reference's list) |
| `instructions` | | the voice model's: who it is, how it speaks, the language (there is no language setting: "Speak Russian unless the player asks to switch") |
| `describeTools` | true | appends the docs' "Delegation policy" template with the tools' names and descriptions |
| `backendModel`, `backendInstructions` | `gpt-6-luna` | the backend; the docs also name `gpt-5.6-luna` (the WebSocket guide) and `gpt-5.6-terra` (the WebRTC guide) |
| `reasoningEffort`, `serviceTier`, `maxOutputTokens`, `toolChoice`, `parallelToolCalls`, `acknowledgementDelayMs` | not sent / `auto` / true | `delegation.responses.*` and `delegation.acknowledgement_delay_ms` |
| `sampleRate`, `chunkSeconds` | 24000, 0.1 | 24000 or 16000 Hz both ways; the microphone goes out in 100 ms pieces (the docs' examples) |
| `idleSeconds` | 45 | closes by itself after this long without speech from either side; 0: never |
| `reconnectOnExpiry`, `reconnectBeforeExpirySeconds` | true, 30 | a new session with the conversation's text at `expired`, or at a quiet moment before `expires_at` |
| `uplinkGate`, `gateHoldSeconds` | true, 0.25 | silence instead of the microphone while the station is audible (loudspeakers) |
| `pricePerMinute`, `backend*PerMillion` | 0.05, 0.10 / 0.01 / 0.50 | US dollars, for `usage()` (gpt-live-1 and gpt-6-luna's standard prices on 2026-11-14) |
| `connectTimeout`, `startTimeout`, `closeTimeout` | 15 s each | |
| `historyChars` | 24000 | the text carried into a new session (the API takes at most 128 messages and 8,192 tokens) |
| `preStartBufferSeconds` | 5 | microphone audio kept while connecting and sent once started (the player's first words) |

## States and events

`state()`: `Idle`, `Connecting`, `Starting` (`session.start` sent), `Listening`, `Speaking` (station audio that is not
silence arrived within 0.3 s, or the game's playback is audible), `CallingTool` (`toolName()`), `Closing`, `Closed`
(`closeReason()`: `Requested`, `Idle`, `Expired`, `Content`, `RemoteHangup`, `ConnectionLost`, `Cancelled`),
`Failed` (`failure()`: a readable message and an `ErrorKind` as for the Assistant: a 401 at the upgrade is `BadKey`,
an `error` event before `session.started` fails the start with the server's message). `takeEvents()`:
`StateChanged`, `Started`, `InputTranscript` and `OutputTranscript` (each delta, and its turn in `history()`),
`PlayerOverlap` (the player spoke while the playback was audible), `ToolStarted`, `ToolFinished`, `DelegationDone`
(its seconds), `Appended` (the server took an append), `ServerError` (a rejected command: the session goes on),
`Reconnecting`, `Closed`, `Failed`.

- **Transcripts** (`session.input_transcript.delta`, `session.output_transcript.delta`): appended exactly as received
  into `history()` turns (a turn grows until the other side speaks), with `start_ms` and `end_ms`. The docs: there
  is no "turn done" event; full duplex means both sides can grow at once.
- **Tool calls** (`toolCalls()`): one per `update()`, shown for one update before it runs (as the Assistant does).
  A bad call (an unknown tool, arguments the schema refuses) is an error result (`Error: ...`) sent back like any
  result: the backend reads it and may try again.
- **Context while it runs**: `appendInstructions()` (`session.instructions.append`: how the station behaves now; may
  interrupt its speech), `appendThinking()` (`session.thinking.append`: a fact it knows but does not say now),
  `appendCommentary()` (`session.commentary.append`: something it should say, paraphrased); each at most 500 tokens,
  `delegation_id` null (non-null ids are for client delegation only), queued while the session starts. The
  acknowledgement (`Appended`) means the server took it, not that it was said.
- `updateTools()` sends the registry again (`session.update` of `delegation.responses.tools`); the voice, the format
  and the model cannot change in a session.

## The uplink gate, and why

On loudspeakers the microphone hears the station; a full-duplex model that hears itself answers its own echo or
yields to it. The engine's capture has no echo cancellation (`docs/audio.md`, "Microphone"), and miniaudio uses no
voice-processing I/O unit. So while the game's playback is audible (`setPlaybackActive(voice->audible())`) and
`gateHoldSeconds` after it, `sendAudio()` sends silence instead of the samples: the timeline stays continuous (the
docs ask for continuous input, silence included) and the station does not hear itself. The API has its own mute
(`session.input_audio.mute`; the reference: "an independent input mute sends silence to the model"); it is not used
for the gate because it costs a round trip and an acknowledgement per switch, where the local silence takes effect
in the same frame. The price: with the gate on, the player cannot interrupt the station on loudspeakers. With
headphones turn it off (`uplinkGate = false`) and interruptions work as the model handles them. `PlayerOverlap`
tells the game that the player spoke over the station; the game may `flush()` its `VoiceStream` then (the server's
audio arrives at real-time pace, so little is queued: see below).

## Cost and the idle close

GPT-Live bills every second a session is open, silence included: 0.05 US dollars a minute (3 dollars an hour for a
session nobody speaks in), per second, plus the backend's tokens (gpt-6-luna: 0.10 / 0.50 dollars a million input /
output tokens). `usage()`: the seconds from the server (`session.usage.updated`, cumulative; final in
`session.closed`), `confirmed` false when a session ended without `session.closed`, the backend's tokens from the
nested `response.completed` events, the context in use, and the cost by the settings' prices. The session closes by
itself after `idleSeconds` without speech from either side (audio that is not silence, transcripts, the game's
playback, a tool call, an append), as the docs advise; `start()` again later opens a new session with the
conversation's text (`input` messages: the state summary as a developer message, then the newest turns within
`historyChars`). The player must be told that an open voice session costs money every second on their own key, and
the game should show when it is open.

## The manual check

`examples/live_check` (macOS, not a part of any gate): one session with two invented tools, a spoken question from a
WAV file fed through the microphone's fake source in its streaming mode at real-time pace, optionally a second file
over the answer; it prints every stage with its milliseconds and writes the answer's audio to a WAV. The key from
`OPENAI_API_KEY` only.

```sh
say -v Samantha -o question.wav --file-format=WAVE --data-format=LEI16@24000 "Station, what is our power level?"
OPENAI_API_KEY=... build/release/examples/live_check/quench-live-check --wav question.wav --out /tmp
#   [--russian] [--instructions TEXT] [--barge-in other.wav --barge-after-delegation 1.5] [--max-seconds 30]
```

`--russian` swaps the English instructions (which ask for English) for Russian ones; `--instructions TEXT` replaces
them in either mode and wins over `--russian`. GPT-Live has no language setting: the language follows the
instructions and the speaker (2026-11-14, four sessions: an English instruction "Always answer in the language the
player has just spoken." gave Russian, English and German answers to questions in those languages, and the Russian
question was answered in Russian also without any sentence about language).

`--talk [--gate off|on] [--seconds 60] [--russian] [--instructions TEXT]` is for a person in a terminal (never run by the checks or by
agents): the Mac's default microphone in the streaming mode into the session, its audio through a `VoiceStream` on
the default output, no window (miniaudio's devices and the AVFoundation permission need none; the first time the
prompt is the terminal's). It prints the state, both transcripts (the station's own words in the player's transcript
mean it hears itself), the microphone's and the output's levels four times a second while the station is audible
(their ratio is the echo's strength), tool calls, each stop of the station's speech before its sentence ended, and
the seconds billed. The stops are classified roughly: silent for 0.8 s with the transcript not ending in . ! ? or an
ellipsis; if the microphone's highest level in the 0.6 s before the stop was under twice the echo level (the mean
microphone level while the station was audible), "self-interruption suspected", else "barge-in". The idle close stays
on; Ctrl-C closes the session cleanly. Not run yet.

Run on 2026-11-14 on the Mac, four sessions, 93 s billed in all (about 0.08 US dollars):

| | English | Russian | English, barge-in near the end | English, barge-in mid-answer |
|---|---|---|---|---|
| WebSocket open / `session.started` after `start()` | 1091 / 1907 ms | 612 / 1277 ms | 525 / 913 ms | 725 / 1629 ms |
| `expires_at` | 7200 s after the start (two hours) | the same | the same | the same |
| first audible audio after the question ended | about 1.4 s (from the answer's WAV, 100 ms steps) | 1479 ms | 1362 ms | 1331 ms |
| delegation (created to the backend's last response) | 3710 ms, 2 calls | 3790 ms, 2 calls | 3732 ms, 2 calls | 2316 ms, 1 call |
| billed (`session.closed`) | 29 s | 23 s | 22 s | 19 s |

Seen, where the docs are silent:

- **Audio arrives all the time**: `session.output_audio.delta` comes in 100 ms pieces at real-time pace for the
  whole session, silence (zeros) between answers; at most 0.10 to 0.11 s ahead of real-time playback in every run.
  So an 80 ms jitter buffer suffices, a flush on interruption drops little, `VoiceStream::active()` is always true
  (use `audible()`), and only pieces above about -54 dBFS count as speech in the session.
- **The first audible audio** is the voice model's own acknowledgement ("Checking ...", "One moment.", "Одну
  секунду, проверяю статус..."), about 1.3 to 1.5 s after the question's audio ended; the answer with the tools'
  results follows the delegation, about 3.3 to 5 s after the question.
- **The tool calls**: each delegation was one Responses response per call (`response.created`, a `reasoning` item,
  the `function_call` item, `response.completed`), then the text answer after the last `response.create`; no call
  ran in parallel in these runs.
- **An interruption on the wire**: nothing marks it. Speaking over the answer: the station's audio turned to silence
  about 1 s after the second voice began, before the transcript of the player's first word arrived, and its output
  transcript stopped mid-sentence ("Power balance is twelve kilowatts,"); then the player's transcript came and the
  station answered the new question ("Fourteen twenty", from the state summary).
- **Russian** was understood (the transcript in Cyrillic) and answered in Russian with the voice `marin`, following
  Russian instructions.
- `session.usage.updated` came once a session, at about 15 s; `session.closed` came about 0.75 s after
  `session.close`, with whole seconds.

Not tried: a real microphone into a session, loudspeakers and their echo, sessions longer than 30 s, the idle close
and the reconnect against the real service (only with the fake), an expiry, parallel calls from the real backend,
iOS.

## Tests

`assistant.live_*` (`tests/assistant/LiveTests.cpp`, the fake WebSocket server playing GPT-Live with the reference's
event shapes; every build, Node.js included): the start handshake (headers, `session.start`'s fields, the tools in
the Responses shape, no key in it), audio both ways (100 ms PCM16 pieces, decoding), the opening words kept while
connecting, transcripts in pieces, a tool call and its result then `response.create`, two parallel calls with one
`response.create` after both, calls the schema refuses, injected context (queued while starting, a null
`delegation_id`, the acknowledgement), the uplink gate (closed, held, open, off), the idle close with the final
usage, each close reason of the server, the connection lost without `session.closed`, an expiry that reconnects with
the history and the state summary, the reconnect before the expiry, error events before and after the start, a 401,
a start timeout, cancel and close, silence that is not speech, the key in no log line.

## Not there

- **The browser.** GPT-Live in a browser goes through WebRTC (the docs' recommended path: `POST /v1/live/sessions`
  with an SDP offer, events on a data channel); a browser's WebSocket cannot send the Authorization header, and the
  Live API documents no other way to authenticate one. A later stage.
- Barge-in on loudspeakers without the gate (needs echo cancellation: Apple's voice-processing I/O unit or a
  browser's WebRTC; neither is wired in).
- Custom voices (`POST /v1/audio/voices`, scoped to a project: every player's key would need its own), audio
  fixtures (`session.output_audio.fixture.play`), forking stored sessions (`store: true`, `.../fork`), client and
  Decisions delegation, typed input to the backend (`response.item.create` with a user message), sideband
  connections, recordings download.
- An iOS run (the WebSocket backend is the same NSURLSession code; not run there), a real microphone, long
  sessions.
