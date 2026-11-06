# Engine demo

`examples/demo` (target `quench_demo`, executable `quench-demo`, app name "Quench Demo") is one app on `quench_main`
and `quench_app` for the Mac and iOS: a menu of sections, each a small interactive scene of primitives and
procedural content that shows one area of the engine, with ImGui controls and the frame, CPU and GPU times. It was
started in stage 5 (user decision, 2026-11-05); every later stage adds its section, and stage 10 of the
[Roadmap](roadmap.md) finishes and polishes it. `examples/hello` stays the minimal sample.

Every section has a scripted check and golden captures, so the demo is also a test of the engine, and every stage
ends with a packaged build of it in `dist/` (`tools/package_demo.sh`, below).

## Sections

| # | Id | Shows | Controls besides the camera |
|---|---|---|---|
| 1 | `materials` | Dielectric and metal spheres from smooth to rough, a textured box, glass (alpha blend), an alpha-tested fence, an emissive lamp; the sun, three point lights and a spot; a game surface shader (the checker ground) and a game render pass (the light discs) | lights on and off, light intensity, animation, discs, checker |
| 2 | `shadows` | Sun shadow cascades over 90 m of columns, thin bars, a spinning torus, a bouncing ball | cascades (0 to 4), map size, distance, filter (1, 4, 12 taps), sun elevation and azimuth, turning sun |
| 3 | `sky` | The daylight palette for any sun elevation, the atmosphere model and its gradient fallback, the clear color without a sky, height fog; the sun disc above its glow (in SDR at a low sun both are white) | sun, run the day, sky, atmosphere, sun disc, fog density, falloff, base, maximum, sun scatter |
| 4 | `post` | Bloom, tone mapping and anti-aliasing at dusk: glowing rings and lamps, thin poles, spinning blades | none, FXAA, TAA (with the fallback shown), render scale, sharpen, bloom strength and threshold, ACES or AgX, exposure, vignette, dither |
| 5 | `hdr` | HDR display output: a sun at 14 degrees, seven emissive panels from 0.5 to 32 times the radiance at the HDR knee, a white card in the shade; with HDR the sun and the brighter panels go above paper white, the card and the UI do not | output mode (off, auto, on), the display's headroom and support and the output in use, the SDR curve for comparison, a custom peak instead of the display's, panel brightness, sun elevation |
| 6 | `physics` | Jolt through `quench::PhysicsWorld` at 120 Hz, drawn interpolated: a stack of boxes and balls | drop a box (B, gamepad X), drop a ball (N, gamepad Y), blast (P, gamepad B, or a click or tap on the floor), reset (R, gamepad Back) |
| 7 | `input` | Named actions over every device, shown live: a character that walks, runs, jumps and fires | WASD, left stick or a finger on the left half: walk; Left Shift or the left stick button: run; J, gamepad A or a tap on the right half: jump; F, gamepad X, a click or a tap: fire |
| 8 | `assets` | Cooked assets loaded through the app's asset manager ([Jobs, assets and the cook](assets.md)): a row of models (a crate from a glTF with an external texture, a pillar from a GLB with embedded JPEG and PNG textures, a vertex-colored ball, a corrupt file and a missing one) shows the load states: gray boxes while loading, magenta on failure. Behind it a 6 x 5 grid of tiles, each with one of eight BC7 textures and a model, streams around a moving focus point | simulated latency, texture cache budget (unlimited, 64, 16, 4 KB), uploads per frame (8 MB, 16 KB, 1 byte), focus angle, moving focus, models on and off, hot reload, reload all; counts of ready, loading, failed, queued and in-flight assets, reloads, evictions, memory per type against its budget, streamed cells |
| 9 | `audio` | The mixer and 3D sound ([Audio](audio.md)): a head with ears at the listener, facing the way the camera looks; an orange emitter circling it at ear height (a motor loop), a blue fly-by passing at speed (a siren, with Doppler), a green box that plays a tone the demo computes (a generator); streamed music A and B with a crossfade; all sounds procedural | emitters on and off, orbit and fly-by speed, tone frequency and position, attenuation model, minimum and maximum distance, Doppler scale; music off, A or B; per bus volume, mute and a peak meter; pause effects; a ping, a burst of 40 pings (voice stealing), a click or tap on the ground plays a ping there; voices, stolen and rejected sounds, underruns, clipped samples, mix time and the output device |

Every section has a "Frame and fallbacks" panel: the 3D resolution, draws, culled instances, lights, shadow draws,
what runs after the device fallbacks (cascades, depth clamp, anti-aliasing), the HDR and depth formats and the device
capabilities.

Common controls:

- Sections: `]` / `[`, Page Down / Page Up, the digits 1 to 9, the gamepad shoulder buttons, a click or tap on the
  list. ImGui keyboard and gamepad navigation is on (the demo's keys keep working while a window has focus).
- Camera (all sections but input): drag with the left or right mouse button or one finger to orbit, the wheel or a
  pinch to zoom, the right stick and the triggers on a gamepad.
- H or the gamepad Start button hides the windows.

Flags: `--section ID` starts in a section (an unknown id fails with a message); the engine's flags (`--help`, App
shell) work as everywhere, for the assets section also `--asset-latency SECONDS`, `--hot-reload` and
`--worker-threads N`, for the audio section `--audio-device NAME`, `--no-audio` and `--audio-capture FILE.wav`.

The assets section's content is procedural (`tools/assets/make_demo_assets.py`, CC0, `assets_src/CREDITS.md`):
`examples/demo/assets_src` (glTF, GLB, PNG; the GLB embeds a JPEG) is cooked at build time by
`quench_cook_assets(quench_demo_cooked ...)` and merged into the build's `assets/` next to the executable
(`quench_add_game(... COOKED_ASSETS quench_demo_cooked)`, target `quench_demo_assets`), and
`examples/demo/assets/models/broken.qmodel` is a deliberately corrupt file. With hot reload on, editing a file in
`assets_src` and building `quench_demo_assets` while the demo runs re-cooks that file, and the demo loads it again.

The audio section's sounds are not in git: `tools/assets/make_demo_sounds.py` (Python standard library, procedural,
CC0, `assets/CREDITS.md`) writes `sounds/motor.wav`, `siren.wav`, `ping.wav` (short loops and a one-shot, 24 and
48 kHz mono), `music_a.wav` and `music_b.wav` (6 s stereo loops at 22050 Hz: a drone with A minor eighths, and one
with D major triplets) into the build tree at build time, and `quench_cook_assets(quench_demo_sounds ... SOUNDS ...)`
checks and copies them into `assets/`. Building the demo therefore needs Python 3.

## Checks

| CTest test | What it runs |
|---|---|
| `demo_<section>_metal`, `demo_<section>_vulkan` | `--section <id> --script examples/demo/scenarios/<id>.txt` at 960 x 540 (`--window` and `--frame-size`), with GPU validation and `--present-mode immediate` (below); the scenario switches the section's options and checks the renderer's and the section's values, puts the start state back, and the frame at 2 s (input: 3 s, assets: 1.5 s) is compared with `tests/golden/<backend>/<device>/demo_<id>.png`. The scenarios point at the window in percent or at named points (`@menu:<section>`, `@floor`), so their checks also pass in a portrait phone window (the captures are compared on the Mac only) |
| `demo_hdr_on_metal`, `demo_hdr_on_vulkan` | the HDR section with `--hdr on --hdr-headroom 4` and `scenarios/hdr_on.txt`: the output is HDR extended linear with headroom 4 and the capture's peak is above 2 and at most 4 times paper white; the capture (clipped at paper white) is compared with `demo_hdr_on.png` |
| `demo_audio_metal`, `demo_audio_vulkan` | the audio section with `--audio-capture` (offline mixing on the frame clock, the WAV next to the capture) and `scenarios/audio.txt`: every emitter, the tone, the crossfade to music B and a burst of 40 pings, no underruns or clipped samples; the frame at 1.65 s is compared with `demo_audio.png` |
| `demo_audio_tone`, `demo_audio_flyby`, `demo_audio_music` | the checks on the mix itself (below), on the default backend only: `scenarios/audio_tone.txt`, `audio_flyby.txt` and `audio_music.txt` with `--audio-capture` and `--present-mode immediate`, no capture |
| `demo_audio_wav_match` | (with Vulkan through MoltenVK) the WAV files of `demo_audio_metal` and `demo_audio_vulkan` are the same bytes: the offline mix does not depend on the GPU backend, which is why the mix checks run on one backend |
| `demo_menu` | `scenarios/menu.txt`: switching sections by keys, gamepad, a click and a tap on the list; hiding the windows |
| `demo_version`, `demo_bad_section` | `--version` prints "Quench Demo <version>"; an unknown `--section` fails |

The assets check (`scenarios/assets.txt`, about 3.6 s per backend) loads with a simulated latency of 0.2 s and checks
the placeholders first (loads in flight, no model ready), then waits for the manager to be idle: three models ready,
the corrupt and the missing one failed (two failures in all), more than four streamed cells. It reloads every asset
(at least six reloads, the failures stay failures), lowers the texture cache to 4 KB, turns the focus to the other
side of the grid and waits for cells to unload and the cache to evict. It runs with FXAA, not TAA: loads finish in
real time, and with TAA the history of the frames before a load arrived made two runs differ.

The audio mix checks are three scenarios that run in parallel: `audio_tone.txt` (23 checks, about 1.7 s of app
time), `audio_flyby.txt` (11 checks, 2.2 s) and `audio_music.txt` (13 checks, 3.9 s). Each turns the orbit, the
fly-by and the music off, waits for the sounds and re-syncs to a fixed app time (loads finish in real time), measures
the mix with `audio_measure` ([App shell](app.md#scenarios)) and ends with no underruns or clipped samples. The tone:
1 kHz at 2 m in front (RMS 0.058 to 0.067, 998 to 1002 Hz), moved to the right (left channel below 0.002), at 8 m
with the inverse, linear and no attenuation, gliding to 1500 Hz, then a burst of 40 pings steals voices. The fly-by's
fundamental approaching (477 to 486 Hz from 440 Hz at 30 m/s) and receding (401 to 410 Hz). Music A (110 Hz), the
crossfade to B (two streams, then one, 147 Hz), the music bus at 0 (silent). All other demo tests run with
`--audio-device null`, so no test plays through the speakers.

The capture checks and the audio mix checks run with `--present-mode immediate`. Every frame of a scripted run is one
fixed step, and frames are held to 120 per second; with vsync they wait for the 60 Hz display as well, which doubles a
check's time without changing a pixel or a sample (measured 2026-11-06: the same capture and WAV, the old single
audio check 18.5 s with vsync, 9.8 s with immediate). Where a window is paced to its display even with `immediate`
([Platforms](platforms.md)), the checks are as slow as with vsync and otherwise the same. The hello checks and
`demo_menu` keep the default vsync.

The scenarios turn dithering off (`set dither 0`), which keeps the golden PNGs at 120 to 380 KB. Game values a
scenario can read: `section`, `sections`, `ui`, `aa` (0 none, 1 FXAA, 2 TAA, after fallbacks), `cascades`, `bloom`,
`draws`, `instances`, `culled`, `shadow_draws`, `shadow_instances`, `moved`, `lights`, `render_w`, `render_h`,
`camera_yaw`, `camera_pitch`, `camera_distance`, `clicks`, then the section's own (for example `bodies`, `blasts`,
`player_x`, `jumps`, `marker_draws`, `exposure`; the assets section's `assets_loading`, `assets_ready`,
`assets_failed`, `assets_idle`, `assets_loads`, `assets_reloads`, `assets_evictions`, `showcase_ready`,
`showcase_failed`, `streaming_active`, `streaming_loads`, `streaming_unloads`; the audio section's `sounds_ready`,
`music_track`, `flyby_x`, `flyby_pitch`, `pings`; the engine's `audio_*` values). Game commands: `section ID`,
`camera YAW PITCH DISTANCE` (degrees, meters), `set NAME VALUE` (render settings: `aa none|fxaa|taa`,
`tone aces|agx`, `cascades`, `shadow_size`, `shadow_filter`, `shadow_distance`, `scale`, `bloom`, `bloom_strength`,
`sky`, `dither`, `sharpen`, `vignette`; otherwise the section's options, for the assets section `latency SECONDS`,
`texture_budget BYTES|unlimited`, `upload_budget BYTES|unlimited`, `focus_angle DEGREES`, `move_focus`, `reload`,
`models`, `hot_reload`; for the audio section `orbit`, `flyby` (also restarts the pass), `tone`, `tone_hz`,
`tone_x`, `tone_z`, `orbit_speed`, `flyby_speed`, `attenuation none|inverse|linear|exponential`, `min_distance`,
`max_distance`, `doppler`, `music 0|1|2`, `bus_<name>` (volume), `mute_<name>`, `pause_effects`, `ping`,
`burst N`).

## Benchmark

`scenarios/benchmark.txt` measures the materials, shadows, sky, post-processing, HDR (in SDR, then with HDR output on),
physics, assets and audio sections for 5 s each in real time (budgets and numbers: [Performance](performance.md));
the assets region streams with the moving focus and the default budgets, after the first loads have finished; the
audio region plays the three emitters, the tone and the music. Automated runs add `--audio-device null` (the mix runs
on miniaudio's null device thread, without sound):

```sh
build/release/examples/demo/quench-demo --frame-size 1920x1080 --script examples/demo/scenarios/benchmark.txt \
    --audio-device null
```

## Packaged builds: `tools/package_demo.sh`

```sh
tools/package_demo.sh          # macOS: dist/QuenchDemo-<version>-<hash>-macos.zip
tools/package_demo.sh --ios    # also iOS: dist/QuenchDemo-<version>-<hash>-ios.zip
```

`<hash>` is the short commit hash, with `-dirty` when the working tree has changes (the build is then not exactly
that commit). The script builds `quench_demo` with the `release` preset (`--preset NAME` for another), runs the
`demo_*` tests, and then:

- **macOS**: assembles `QuenchDemo.app` (the executable, an Info.plist with the version and the binary's minimum
  macOS as `LSMinimumSystemVersion`, an icns made from `examples/demo/icon/icon.png`, `LICENSE` and
  `THIRD_PARTY_LICENSES` in `Contents/Resources`, the demo's cooked assets in `Contents/Resources/assets`, where
  SDL's base path of a bundled app points), checks that it links only system libraries, signs it ad hoc and
  zips it with `LICENSE` and `THIRD_PARTY_LICENSES` next to it. The archives carry no extended attributes: `ditto`
  would store them as AppleDouble `._` files, which a plain `unzip` turns into files inside the bundle that break its
  signature; the script fails on any `._` entry. It then unzips the archive with a plain `unzip` into a temporary
  directory, verifies the signature and starts the app from `/` with `scenarios/smoke.txt` (every section once, with
  `--audio-device null`: no sound): exit code 0, the scenario
  passed, and a capture (`dist/<name>-macos-smoke.png`) to look at. The app is signed ad hoc and not notarized: a
  copy downloaded on another Mac is quarantined, and Gatekeeper opens it only after it is allowed in System Settings,
  Privacy & Security (not tried on another Mac yet).
- **iOS** (`--ios [--team ID]`): builds `quench_demo` with the `ios-xcode` preset (Automatic signing with the team:
  `--team`, else `QUENCH_IOS_TEAM` from the environment, else the one `build/ios-xcode` is configured with; an Apple
  ID signed in to Xcode, which creates the development profile on its own) and packs the signed app as
  `QuenchDemo.ipa`, with the licences, into the zip. The team must be a paid one: the script refuses a free Personal
  Team (Xcode's account data marks it), whose profiles expire after 7 days, and checks that the app is signed by the
  team (`codesign`, `TeamIdentifier`), also after a plain `unzip` of the `.ipa`. It runs only on the devices in the
  profile, and the profile expires (the paid
  team's wildcard profile made on 2026-11-05 lasts until 2027-11-05, with 5 devices). The script prints the team, the
  signature, the profile's name, expiry and device count. Nothing is installed on a device by the script.

`dist/` is git-ignored.
