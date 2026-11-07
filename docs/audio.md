# Audio

Stage 7 of the [Roadmap](roadmap.md): `quench_audio`, a headless mixer (buses, reverb buses with sends, sounds in
memory, streamed music with crossfades, sources the game computes, 3D sound with a voice limit), and `quench_audio_device`, the playback device
on miniaudio with the iOS audio session. The app shell owns one of each ([App shell](app.md)); the engine demo's
"Audio" section shows them ([Engine demo](demo.md)). What was run where is under [Platforms](#platforms); nothing in
this stage was listened to by the user yet. All documentation: [index](README.md).

## Use

```cpp
quench::audio::AudioSystem& audio = app.audio();
auto hit = audio.load("sounds/hit.wav");                           // AssetRef<SoundAsset>, decoded on a job
audio.play(hit, {.bus = audio.bus("effects"), .volume = 0.8f});    // a one-shot; plays once the sound is loaded
quench::audio::SoundHandle engine = audio.play("sounds/engine.wav",
    {.bus = audio.bus("effects"), .loop = true, .priority = 5, .fadeIn = 0.1f, .spatial = true});
audio.setPosition(engine, carPosition, carVelocity);                // every frame
audio.setListener({.position = cameraPosition, .forward = cameraForward, .velocity = cameraVelocity});
audio.playMusic("music/level1.mp3", {.crossfade = 2.0f});           // streamed; the old track fades out
audio.setBusPaused(audio.bus("effects"), true);                     // a pause menu; "ui" keeps playing
```

The app calls `AudioSystem::update()` once per frame after `Game::update` (ended sounds, pending starts, the frame's
changes to the mixer, stream refills); a game without the app shell calls it itself and opens an `audio::Device` or
mixes offline. `AppDesc::audio` (`AudioSettings`: sample rate, voice limit, buses, budgets) and
`AppDesc::audioDevice` (`DeviceDesc`: output name, iOS session category, callback period) configure them.

## Mixer

- **Format**: 32-bit float stereo at `AudioSettings::sampleRate` (48 kHz by default), mixed in blocks of 64 frames.
  Sources keep their own rate and channel count (mono or stereo) and are resampled per voice by linear interpolation
  (a 32.32 fixed-point position, playback rate 1/16 to 16 including Doppler).
- **No clicks**: volume, pan and pitch changes are smoothed (one-pole, 10 ms) and ramped linearly inside a block;
  pausing ramps over 5 ms; stops and steals fade out over at least 3 ms (`stop(sound, fade)` for longer fades);
  fade-ins and crossfades use equal-power curves (`audio.mixer_click_free_ramps` checks the largest step between
  samples).
- **Handles**: `SoundHandle` (slot and generation; a stale handle is ignored by every call), at most
  `maxSounds` (256) alive; `play` with every handle in use returns an invalid handle and counts `refused`.
  `setEndCallback` reports every end with its reason (`Finished`, `Stopped`, `Stolen`, `Rejected`, `Dropped`,
  `Failed`).
- **Output**: the master bus is clamped to [-1, 1]; clamped samples are counted (`AudioStats::clipped`). There is no
  limiter or compressor: keep the mix below full scale.

## Buses

A tree of up to 32 buses under `master` (index 0). The default list is `music`, `effects` and `ui`; games give their
own in `AudioSettings::buses` (each names a parent declared before it). A bus has a volume (smoothed), mute and
pause (`setBusPaused` freezes every sound on the bus and its children after a 5 ms ramp, and they resume where they
stopped) and a peak meter per channel with a falling hold (0.3 s time constant; `busInfo(bus).peak`). `bus(name)`
returns the master bus and logs a warning for an unknown name; `findBus` returns nothing instead.

## Reverb

A bus with `BusDesc::reverb` set is a reverb bus: Freeverb through verblib ([Dependencies](dependencies.md), eight
combs and four allpasses per channel). Its input (what plays on it, its child buses and the sends to it) is replaced
by the reverb's wet signal, so it is an aux bus that sounds send to; its volume is the reverb's level and its parent
decides where the reverb goes (the demo puts it under `effects`, so muting or pausing the effects also silences
their reverb).

```cpp
settings.buses.push_back({.name = "reverb", .parent = "effects", .reverb = audio::ReverbDesc{.roomSize = 0.6f}});
audio.play(step, {.bus = audio.bus("effects"), .spatial = true, .sendBus = audio.bus("reverb"), .sendLevel = 0.4f});
audio.setSend(engine, audio.bus("reverb"), 0.2f);       // smoothed like the volume (10 ms)
audio.setReverb(audio.bus("reverb"), {.roomSize = 0.9f}); // a cave; damping and width as well
```

- **Sends**: `PlayParams::sendBus` and `sendLevel` (or `setSend`) add the sound to a second bus after its volume,
  pan, attenuation, fades and pause, at `sendLevel` times that (one send per sound; any bus can be sent to, a reverb
  bus is the use). A send is taken from the sound, before its own bus: the volume and mute of the sound's bus do not
  change it, those of the reverb bus and its parents do.
- **`ReverbDesc`**: `roomSize` 0..1 (verblib's feedback, 0.7 to 0.98), `damping` 0..1 (how much faster high
  frequencies decay), `width` 0..1 (the stereo width of the tail). Measured on an impulse at 48 kHz
  (`audio.reverb_decay_grows_with_room_size`, T30 of the energy decay): RT60 0.67 s at room size 0.2, 1.08 s at 0.5,
  2.43 s at 0.8 (damping 0.25). The wet level is verblib's: an impulse of 1 peaks at about 0.05; 0.7 sends of the
  test's three sounds (0.21 RMS) gave 0.084 RMS of reverb.
- **Cost**: the reverb runs on the audio thread in the mixer's blocks and allocates nothing (its delay lines, about
  400 KB per reverb bus, are allocated when the `AudioSystem` is created). It is idle while its delay lines are
  cleared and its input is under -100 dB: then it costs only a check of its input. It becomes idle once its input
  and output have stayed under -100 dB for twice its longest delay path (7173 frames, 0.15 s at 48 kHz); the delay lines are
  then cleared (verblib's tail does not decay to exact zeros by itself; the step to silence is under 1e-5, 8.8e-6 at
  most in the test). Offline on the M4 Pro (`audio.mix_cost`, opt-in with `QUENCH_TEST_AUDIO_COST=1`, three runs):
  32 looping 3D voices 2.28 µs of render time per millisecond of audio with an idle reverb bus, 3.24 µs with every
  voice sending into it, so about 1 µs per millisecond (0.1% of a core) for one reverb.
- **Rates**: verblib takes 22050 to 176400 Hz. At another `AudioSettings::sampleRate` a reverb bus is silent, with a
  warning when the `AudioSystem` is created.
- **Bit-exact when unused**: a mix with a reverb bus and no sends, or with sends into a reverb bus at volume 0, is
  bit for bit the mix without the bus (`audio.reverb_wet_zero_is_dry`).

## Sounds

Sounds in memory are an asset type of the `AssetManager` ([Jobs, assets and the cook](assets.md)):
`audio::SoundAsset`, type `"sound"`, registered by the `AudioSystem` (and unregistered by its destructor). A load
reads the file through `quench::fs` and decodes it on a job to 32-bit float PCM (WAV, FLAC, MP3 and Ogg Vorbis
through miniaudio, Vorbis through its `stb_vorbis`; more than two channels are mixed down to stereo; at most 256 MB
decoded). The memory counts as CPU memory against
`AudioSettings::soundBudget` (unreferenced sounds are evicted over it). A play of a sound that is still loading
starts when it is ready; a one-shot that would start more than `maxStartDelay` (0.25 s) late is dropped instead
(`EndReason::Dropped`), loops wait. A missing or corrupt file ends its sounds with `Failed` and the manager logs it
once. A reload replaces the data for later plays; sounds already playing keep theirs. `play(std::shared_ptr<const
SoundData>)` plays decoded data the game made itself.

## Streams and music

`playStream(path, params)` streams a WAV, FLAC, MP3 or Ogg Vorbis file: it is opened through `quench::fs::openAsset`
(a reader, not the whole file) and decoded on jobs into a ring of `streamBufferSeconds` (1 s) per stream, refilled
when half of it has played; one job at a time per stream. Loops start over without a gap where the decoder runs out
of frames, without the file's length (`audio.stream_loop_is_gapless`; for MP3 see [Limitations](#limitations)). That
matters for Vorbis: a stream reads it in miniaudio's push mode, which reports no length (a sound decoded from memory
reads it in pull mode, which does). A streamed Vorbis loop gives exactly the frames of the file decoded in memory,
every time round (`audio.stream_vorbis_loop`: 3.5 s of a 1 s loop; the largest step between neighbouring frames 0.0587
within 5 ms of the loop points against 0.0603 elsewhere, 25 ms levels 0.990 to 1.004 of the whole, where 2.5 ms of
silence at a loop point measures 0.939). Starting over reopens a Vorbis decoder (miniaudio seeks back by opening it
again), on the stream's job. A stream that runs empty plays silence and counts an underrun. A file that cannot be
opened or decoded is logged once and ends the sound with `Failed`.

`playMusic(path, {crossfade, volume, loop, bus})` keeps one streamed track: the new one fades in while the playing
one fades out over `crossfade` seconds (equal power); `stopMusic(fade)` ends it. Music goes to the bus named `music`
by default.

## Generators

`Generator` is a source the game computes on the audio thread: `generate(out, frames)` writes interleaved frames at
its own `Format` and follows the audio thread's rules (no allocation, locks, logging, file access or waiting). The
demo's tone is one. `PcmStream` is a generator fed from the game thread through a single-producer single-consumer
ring (`push`, `writable`, `queued`, `finish`); when it runs empty it plays silence and counts an underrun.

## 3D sound

One listener (`setListener`: position, forward, up, velocity) and an `Emitter` per spatial sound (`spatial = true`;
`setEmitter` or `setPosition`):

- **Attenuation** (`Attenuation`): `None`; `Inverse`, min / (min + rolloff · (d − min)), the physical 1/d with
  rolloff 1 (the default); `Linear`, silent at `maxDistance` with rolloff 1; `Exponential`, (d / min)^−rolloff. The
  distance is clamped to [`minDistance`, `maxDistance`].
- **Panning**: the direction's component along the listener's right axis, narrowed toward the center inside
  `minDistance`; constant power for mono sources (both channels at 0.707 in the center). Stereo sources are mixed
  down to mono when spatial; without `spatial`, `pan` is a balance for stereo and constant power for mono. Front and
  back sound alike (no HRTF, no filtering).
- **Doppler**: (c + v_listener) / (c + v_emitter) with the velocities along the line between them (approaching
  raises the pitch), scaled by `AudioSettings::dopplerScale` and `Emitter::doppler`, each velocity limited to 0.9 c
  and the factor clamped to [0.25, 4]; c is `AudioSettings::speedOfSound` (343 m/s).

`spatialize`, `distanceGain` and `panGains` are public, so a game can compute the same numbers (`audio.spatial_*`
check them against the formulas).

## Voice limit and priorities

At most `maxVoices` (32) sounds are mixed at once; sounds fading out after a stop or a steal do not count. A play
beyond the limit steals the voice with the lowest priority among those of at most its own priority, then the
quietest of those (its gain after volume, distance, pan and fades in the last block; not the loudness of its
samples), then the oldest; the stolen sound fades out over 3 ms and ends with
`Stolen`. When every voice has a higher priority the new sound ends with `Rejected`. There are no virtual voices: a
stolen or rejected sound does not come back (a looping sound of a high priority keeps its voice). The demo's
"40 pings" button and `audio.mixer_voice_stealing` show it.

## Threads

The device's callback thread belongs to the operating system's audio API (Core Audio through miniaudio). It is the
one thread that is not a job ([Architecture](architecture.md#threads-and-jobs)): it runs every few milliseconds with
a real-time deadline. On it only `AudioSystem::render` runs: no allocation, freeing, locks, logging, file access or
waits. Every other function is for the game thread. The game thread sends plain commands (play, stop, parameters,
buses, listener) through a lock-free single-producer single-consumer queue in `update()` and receives end events
the same way; statistics are atomics. Data a playing sound uses (decoded sounds, stream rings, generators) stays
alive on the game side until the audio thread reports its end. Decoding (sound loads, stream refills) runs on the
job system, which also works without worker threads (the single-threaded web build: the jobs run in `update()`).
`audio.mixer_thread_stress` plays, changes and stops sounds from the game thread while another thread renders (with
sends and reverb changes), also under ThreadSanitizer. `audio.mixer_render_allocates_nothing` counts the calls of
the test program's replaced global `operator new` and `operator delete` on the rendering thread during 600 renders
with sounds, a Vorbis stream, generators, 3D sound, sends, reverb changes and bus pauses in flight: 0 (a direct
`malloc` would not be counted; the mixer and verblib have none).

## Output

`audio::Device::open(audio, desc)` (`quench_audio_device`) opens a miniaudio playback device whose thread calls
`render`. `DeviceDesc::name` is `""` or `"default"` (the system's default output, followed when it changes),
`"null"` (miniaudio's null backend: a device thread without sound, for tests and benchmarks) or a part of an output's
name (else the default, with a warning listing the outputs, `Device::outputNames()`). The requested period is 10 ms;
the system may choose another (`info()` reports the device, backend, rates and period). miniaudio converts to the
device's sample rate. `suspend()` and `resume()` stop and start the device; the app calls them on
`WILL_ENTER_BACKGROUND` and `DID_ENTER_FOREGROUND`, and stops the device before `Game::shutdown`.

**iOS audio session**: before the device opens, the engine sets the `AVAudioSession` category from
`DeviceDesc::session` and activates the session: `Ambient` (the default: mixes with other apps' audio, silenced by
the Ring/Silent switch and the screen lock), `SoloAmbient` (silences other apps, silenced by the switch) or
`Playback` (silences other apps, plays with the switch on silent). An interruption (a call, Siri, an alarm) stops the
device; `Device::update()` starts it again when the interruption ends. Route changes (headphones, Bluetooth) are
followed by the system and logged; `interruptions()` and `routeChanges()` count them. On other platforms the session
settings are ignored.

## Offline mixing and captures

Without a device the app mixes on the main thread, on the frame clock: `--no-audio` (the mix is discarded),
`--audio-capture FILE.wav` (the whole mix is kept and written at exit as 16-bit stereo WAV) or a device that failed
to open (a warning, then as `--no-audio`). `AudioSystem::mixOffline(seconds, out)` mixes the frame's time step
(whole frames, the remainder carried to the next call). In offline mode streams are opened and refilled on the main
thread as the mix needs them, so once the sounds are loaded the result does not depend on thread timing: in a
deterministic run (`--script`, `--capture`) the same script writes the same WAV file on every run (the demo's audio
WAV was byte-identical between Metal and Vulkan and between Debug and Release). Sound loads still finish in real
time, so a script waits for its sounds (the demo's `sounds_ready`) and starts its measurements from a fixed time.

Scenarios with an `audio_measure` command keep the last 4 s of the mix (`setMonitor`; the copy cost hello's frame
thread about 6 µs at the median, so other scripts, the benchmarks among them, do not);
`audio_measure S [MIN_HZ MAX_HZ]` measures the newest S seconds: RMS per channel and of both, peak, and the strongest frequency (a Hann-windowed FFT
of up to 65536 frames with the peak interpolated between bins, `audio::dominantFrequency`), and sets the
`audio_rms`, `audio_rms_l`, `audio_rms_r`, `audio_peak`, `audio_freq`, `audio_freq_level` and `audio_measured`
values; the `audio_*` counters (voices, sounds, pending, streams, played, stolen, rejected, failed, underruns,
clipped, device) are always readable ([App shell](app.md#scenarios)). `encodeWav` writes 16-bit PCM or 32-bit float
WAV files.

## Statistics

`AudioSystem::stats()`: voices mixed in the last render and the limit, handles alive, pending sounds, open streams,
counters (played, stolen, rejected, dropped, failed, refused, underruns, clipped samples, frames mixed), and the
average time of one `render` call with its share of real time (`mixMs`, `load`, over the last half second of
audio). The demo's panel shows them (the mix time is hidden in deterministic runs, which compare captures).

## Demo and tests

The demo's "Audio" section ([Engine demo](demo.md#sections)): a head with ears at the listener (it faces the way the
camera looks), an emitter circling it at ear height (a motor loop), a fly-by at 30 m/s (a siren, with Doppler), a
tone the demo computes (a `Generator`), music A and B with a 2 s crossfade, bus sliders with mute and meters, pause
of the effects bus, a ping on the ground where it is clicked, a burst of 40 pings for the voice limit, and a reverb
(off by default; a `reverb` bus under `effects` that the 3D sounds send to at 0.4, and a room size slider). Its sounds
are procedural: `tools/assets/make_demo_sounds.py` (Python standard library, CC0) writes `motor`, `siren`, `ping`,
`music_a` and `music_b` as WAV files into the build tree at build time, and `quench_cook_assets(... SOUNDS ...)`
checks and copies them. Its mix checks (`examples/demo/scenarios/audio_tone.txt`, `audio_flyby.txt`,
`audio_music.txt` and `audio_reverb.txt`, parts of the batches) measure levels and
frequencies of the mix: the tone at 2 m (RMS 0.0625 and 1000 Hz), panned to the right, the three attenuation models
at 8 m, a pitch glide, the fly-by approaching (481 Hz from a 440 Hz siren fundamental) and receding (405 Hz), both
music tracks and the crossfade, a muted bus, no underruns or clipping, stealing, and the reverb (silence 0.1 to
0.2 s after the tone's fade without it; with it at room size 0.8 the tone at 0.0733 RMS, its tail 0.0111 RMS after
the fade and 0.0006 a second later, -25 dB; at room size 0.2 under 0.0001 a second after the fade). The section's capture check
(`audio.txt`) runs in the batch of every backend (Metal, the native Metal backend, Vulkan through MoltenVK); the
batches write no WAV file, so the backends' mixes are no longer compared byte for byte (`demo_audio_wav_match` is
gone).

Tests (`tests/audio/`): `audio.*` (31 cases, headless, also under Node.js: spatial formulas, bus gains, click-free
ramps, resampling and pitch, Doppler, panning and attenuation in the mix, voice stealing, one-shots and loops,
pause, handles, offline carry and the monitor, `PcmStream`, the thread stress case, no allocation in render, the
reverb (bit-exact when unused, decay by room size), the opt-in mix cost, streams across chunk
boundaries, on jobs, gapless loops, crossfades, Vorbis streams, loops and music, missing files, decoders and their
errors, the sound asset type, the analysis functions) and `audio_device.*` (`null_device`: the null backend renders and suspends;
`default_output`: plays 0.5 s of a quiet 440 Hz tone through the default output, only with
`QUENCH_TEST_AUDIO_DEVICE=1` in the environment; `audio_session_category`: iOS only). No automated test plays
through the speakers: the app tests and capture checks run offscreen, where the mix is offline
([App shell](app.md#offscreen-runs)), or with `--audio-device null` or `--audio-capture`. The test
decoder files (`tests/audio/data/`, a FLAC and an MP3 tone and an Ogg Vorbis loop) are CC0, made with sox (with
libvorbis) and lame (`CREDITS.md` there). `audio.decode_vorbis` decodes the loop (1 s, 440 Hz left and 880 Hz right
at quality 3, 8.6 KB) to exactly its 48000 frames, 38.97 dB (left) and 37.99 dB (right) of signal to error against
the sines it was encoded from.

## Platforms

- **macOS**: Core Audio through miniaudio; the tests and the demo's checks run on the null backend or offline. No
  run in this stage opened the default output (`audio_device.default_output` is opt-in and was not run), and the
  user has not listened to it yet.
- **iOS Simulator**: built and tested (`ios-sim-tests`: the audio and audio device cases, the session category
  case included). No test opened a real output there, so session activation, interruptions and route changes have
  not run; not listened to.
- **iPhone**: built (`ios-device-release`); not run in this stage. The silent switch, interruptions and route changes
  need the device and the user.
- **Web**: the mixer, decoders and analysis run under Node.js (`wasm-node`, single-threaded, without device I/O);
  browser output (Web Audio, an `AudioWorklet`, and starting audio only after a user gesture) is not built.

## Limitations

- **Not listened to**: every check is numeric (levels, frequencies, counters, click steps); how it sounds on the
  Mac's speakers, headphones and the iPhone is for the user to judge.
- **3D**: stereo panning only (no HRTF, no front and back difference, no occlusion or filters); one listener; no
  virtual voices (a stolen or rejected sound is gone).
- **Reverb**: one algorithm (Freeverb), set by hand per bus: no early reflections from geometry, no zones or
  automatic blending between rooms, no pre-delay, one send per sound. A reverb bus at a rate outside 22050 to
  176400 Hz is silent. Checked by numbers only (decay times, levels, bit-exactness); nobody has listened to it.
- **Formats**: WAV, FLAC, MP3 and Ogg Vorbis; no Opus. The MP3 decoder keeps the encoder's delay and padding (the
  test file decodes to 12672 frames for 11025 encoded), so gapless MP3 loops are not gapless: loop WAV, FLAC or
  Vorbis files. Vorbis: one logical stream per file (stb_vorbis does not decode chained streams), and the
  start-of-stream sample truncation of a file whose first granule position is not 0 is ignored (stb_vorbis' limits).
  Only a file from libvorbis (through sox) was tried. A streamed Vorbis loop opens its decoder again at every loop
  point, on the stream's job (miniaudio's push mode seeks back that way); the cost of that was not timed.
- **Resampling** is linear interpolation (some aliasing at high pitch; cheap). The cook does not resample.
- **Offline mode** decodes streams on the main thread (meant for tests, captures and `--no-audio`, not for play).
- **Mix time** is measured per `render` call on the audio thread; there is no per-bus or per-voice profile.
