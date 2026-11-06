# Audio

Stage 7 of the [Roadmap](roadmap.md): `quench_audio`, a headless mixer (buses, sounds in memory, streamed music with
crossfades, sources the game computes, 3D sound with a voice limit), and `quench_audio_device`, the playback device
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

## Sounds

Sounds in memory are an asset type of the `AssetManager` ([Jobs, assets and the cook](assets.md)):
`audio::SoundAsset`, type `"sound"`, registered by the `AudioSystem` (and unregistered by its destructor). A load
reads the file through `quench::fs` and decodes it on a job to 32-bit float PCM (WAV, FLAC, MP3 through miniaudio;
more than two channels are mixed down to stereo; at most 256 MB decoded). The memory counts as CPU memory against
`AudioSettings::soundBudget` (unreferenced sounds are evicted over it). A play of a sound that is still loading
starts when it is ready; a one-shot that would start more than `maxStartDelay` (0.25 s) late is dropped instead
(`EndReason::Dropped`), loops wait. A missing or corrupt file ends its sounds with `Failed` and the manager logs it
once. A reload replaces the data for later plays; sounds already playing keep theirs. `play(std::shared_ptr<const
SoundData>)` plays decoded data the game made itself.

## Streams and music

`playStream(path, params)` streams a WAV, FLAC or MP3 file: it is opened through `quench::fs::openAsset` (a reader,
not the whole file) and decoded on jobs into a ring of `streamBufferSeconds` (1 s) per stream, refilled when half of
it has played; one job at a time per stream. Loops start over without a gap (`audio.stream_loop_is_gapless`; for
MP3 see [Limitations](#limitations)). A stream that runs empty plays silence and counts an underrun. A file that
cannot be opened or decoded is logged once and ends the sound with `Failed`.

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
`audio.mixer_thread_stress` plays, changes and stops sounds from the game thread while another thread renders, also
under ThreadSanitizer.

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
of the effects bus, a ping on the ground where it is clicked and a burst of 40 pings for the voice limit. Its sounds
are procedural: `tools/assets/make_demo_sounds.py` (Python standard library, CC0) writes `motor`, `siren`, `ping`,
`music_a` and `music_b` as WAV files into the build tree at build time, and `quench_cook_assets(... SOUNDS ...)`
checks and copies them. Its mix checks (`examples/demo/scenarios/audio_tone.txt`, `audio_flyby.txt` and
`audio_music.txt`, run in parallel on the default backend) run with `--audio-capture` and measure levels and
frequencies of the mix: the tone at 2 m (RMS 0.0625 and 1000 Hz), panned to the right, the three attenuation models
at 8 m, a pitch glide, the fly-by approaching (481 Hz from a 440 Hz siren fundamental) and receding (405 Hz), both
music tracks and the crossfade, a muted bus, no underruns or clipping, and stealing. The section's capture check
(`audio.txt`, Metal and Vulkan through MoltenVK) writes the mix as WAV too, and `demo_audio_wav_match` checks that
both backends write the same bytes.

Tests (`tests/audio/`): `audio.*` (24 cases, headless, also under Node.js: spatial formulas, bus gains, click-free
ramps, resampling and pitch, Doppler, panning and attenuation in the mix, voice stealing, one-shots and loops,
pause, handles, offline carry and the monitor, `PcmStream`, the thread stress case, streams across chunk
boundaries, on jobs, gapless loops, crossfades, missing files, decoders and their errors, the sound asset type,
the analysis functions) and `audio_device.*` (`null_device`: the null backend renders and suspends;
`default_output`: plays 0.5 s of a quiet 440 Hz tone through the default output, only with
`QUENCH_TEST_AUDIO_DEVICE=1` in the environment; `audio_session_category`: iOS only). No automated test plays
through the speakers: the app tests and capture checks run offscreen, where the mix is offline
([App shell](app.md#offscreen-runs)), or with `--audio-device null` or `--audio-capture`. The test
decoder files (`tests/audio/data/`, a FLAC and an MP3 tone) are CC0, made with sox and lame (`CREDITS.md` there).

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
- **3D**: stereo panning only (no HRTF, no front and back difference, no occlusion, no reverb or filters); one
  listener; no virtual voices (a stolen or rejected sound is gone).
- **Formats**: WAV, FLAC and MP3; no Vorbis or Opus. The MP3 decoder keeps the encoder's delay and padding (the test
  file decodes to 12672 frames for 11025 encoded), so gapless MP3 loops are not gapless: loop WAV or FLAC files.
- **Resampling** is linear interpolation (some aliasing at high pitch; cheap). The cook does not resample.
- **Offline mode** decodes streams on the main thread (meant for tests, captures and `--no-audio`, not for play).
- **Mix time** is measured per `render` call on the audio thread; there is no per-bus or per-voice profile.
