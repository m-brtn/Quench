# Engine demo

`examples/demo` (target `quench_demo`, executable `quench-demo`, app name "Quench Demo") is one app on `quench_main`
and `quench_app` for the Mac and iOS: a menu of sections, each a small interactive scene of primitives and
procedural content that shows one area of the engine, with ImGui controls and the frame, CPU and GPU times. It was
started in stage 5 (user decision, 2026-11-05); every later stage added its section, and stage 10 of the
[Roadmap](roadmap.md) finished it (2026-11-06): one panel layout and one camera for every section, a compact layout
for phones, a tour, a check mode and a benchmark of every section, an About panel, saved settings. `examples/hello`
stays the minimal sample.

Every section has a scripted check and golden captures, so the demo is also a test of the engine, and every stage
ends with a packaged build of it in `dist/` (`tools/package_demo.sh`, below).

## Code

| File | What |
|---|---|
| `Demo.h` | the `Section` interface (id, title, description, `init`, `enter` / `leave`, the steps, `controls()`, `stats()`, scenario values, commands and points), `OrbitCamera`, `Context` and the helpers sections share |
| `sections/<Name>Section.cpp` | one file per section: its scene, controls, numbers and scenario hooks |
| `DemoGame.h`, `Demo.cpp` | the app around the sections (`quench::app::Game`): sections made at the start and initialized when first shown, switching, the settings, the scenario hooks, the flags (`createGame`) |
| `Ui.cpp` | the two layouts, the menu, the Performance and Log panels, the section's panel, About and Settings |
| `Camera.cpp` | the camera controls every 3D section shares |
| `Modes.cpp`, `Checks.h`, `Checks.cpp` | the tour, the check mode with its items and `checks.json`, the benchmark of every section |
| `Settings.h`, `Settings.cpp` | `settings.json` |
| `Common.cpp` | the "Frame and fallbacks" panel and the helpers of `Demo.h` (meshes, placement, ground picks, actions, parsing) |
| `BuildInfo.cmake` | the build type and the commit for About (`demo_build_info.h`, written at build time) |

A section is made when the app starts (only its camera and render settings) and initialized when it is first shown:
0.3 to 3.3 ms per section in Release on the M4 Pro (measured 2026-11-06; assets then load on the job system). It
stays loaded until the app ends or it is reset; "Reset section" replaces it with a new object of its kind, as on its
first visit. Leaving a section removes its render passes; its scene stays.

## Sections

| # | Id | Shows | Controls besides the camera |
|---|---|---|---|
| 1 | `materials` | Dielectric and metal spheres from smooth to rough, a textured box, glass (alpha blend) with its translucent, tinted shadow, an alpha-tested fence whose shadow has its holes, an emissive lamp; the sun, three point lights and a spot; a game surface shader (the checker ground) and a game render pass (the light discs) | lights on and off, light intensity, animation, discs, checker |
| 2 | `shadows` | Sun shadow cascades over 90 m of columns, thin bars, a spinning torus, a bouncing ball | cascades (0 to 4), map size, distance, filter (1, 4, 12 taps), sun elevation and azimuth, turning sun |
| 3 | `sky` | The daylight palette for any sun elevation, the atmosphere model and its gradient fallback, the clear color without a sky, height fog; the sun disc above its glow (in SDR at a low sun both are white) | sun, run the day, sky, atmosphere, sun disc, fog density, falloff, base, maximum, sun scatter |
| 4 | `post` | Bloom, tone mapping and anti-aliasing at dusk: glowing rings and lamps, thin poles, spinning blades | none, FXAA, TAA (with the fallback shown), render scale, sharpen, bloom strength and threshold, ACES or AgX, exposure, vignette, dither |
| 5 | `hdr` | HDR display output: a sun at 14 degrees, seven emissive panels from 0.5 to 32 times the radiance at the HDR knee, a white card in the shade; with HDR the sun and the brighter panels go above paper white, the card and the UI do not | output mode (off, auto, on), the display's headroom and support and the output in use, the SDR curve for comparison, a custom peak instead of the display's, panel brightness, sun elevation |
| 6 | `physics` | Jolt through `quench::PhysicsWorld` at 120 Hz, drawn interpolated: a stack of boxes and balls | drop a box (B, gamepad X), drop a ball (N, gamepad Y), blast (P, gamepad B, or a click or tap on the floor), reset (R, gamepad Back) |
| 7 | `input` | Named actions over every device, shown live: a character that walks, runs, jumps and fires | WASD, left stick or a finger on the left half: walk; Left Shift or the left stick button: run; J, gamepad A or a tap on the right half: jump; F, gamepad X, a click or a tap: fire |
| 8 | `assets` | Cooked assets loaded through the app's asset manager ([Jobs, assets and the cook](assets.md)): a row of models (a crate from a glTF with an external texture, a pillar from a GLB with embedded JPEG and PNG textures, a vertex-colored ball, a corrupt file and a missing one) shows the load states: gray boxes while loading, magenta on failure. Behind it a 6 x 5 grid of tiles, each with one of eight BC7 textures and a model, streams around a moving focus point | simulated latency, texture cache budget (unlimited, 64, 16, 4 KB), uploads per frame (8 MB, 16 KB, 1 byte), focus angle, moving focus, models on and off, hot reload, reload all; counts of ready, loading, failed, queued and in-flight assets, reloads, evictions, memory per type against its budget, streamed cells |
| 9 | `audio` | The mixer and 3D sound ([Audio](audio.md)): a head with ears at the listener, facing the way the camera looks; an orange emitter circling it at ear height (a motor loop), a blue fly-by passing at speed (a siren, with Doppler), a green box that plays a tone the demo computes (a generator); streamed music A and B with a crossfade; all sounds procedural | emitters on and off, orbit and fly-by speed, tone frequency and position, attenuation model, minimum and maximum distance, Doppler scale; music off, A or B; per bus volume, mute and a peak meter; pause effects; a ping, a burst of 40 pings (voice stealing), a click or tap on the ground plays a ping there; voices, stolen and rejected sounds, underruns, clipped samples, mix time and the output device |
| 10 | `animation` | Skeletal animation ([Animation](animation.md)) of a procedural mannequin: the hero walks a circle by root motion with a speed blend of idle, walk and run, jumps, waves (a layer masked to the right arm), leans (an additive layer), turns its head toward the camera (look-at), holds a torch on its left hand (an attachment) and plays a step sound at each footstep event (a louder one at the jump's landing); two characters stand with foot placement on a ramp and across a 0.2 m step (rays into a Jolt world); a crowd walks in rings | speed, lean, jump (J, gamepad A), wave (K, gamepad X), restart; foot IK, look-at and footstep sound on and off; ramp angle (0 to 30 degrees), crowd size (0 to 512); the hero's state and transition, characters, joints, clip samples, ground rays, events, skinned instances and vertices, the update time and threads, the skinning GPU time |
| 11 | `raytracing` | Ray tracing where `gpu::Caps` allow it ([Render module](render.md#ray-traced-shadows-and-ambient-occlusion)), on the native Metal backend (`--gpu-backend metal`): soft ray-traced sun shadows through a fence's diamond holes (cut-out), tinted amber by a spinning glass torus (translucent) and cast by the animation section's mannequin walking a circle (skinned, refitted every frame), ambient occlusion under a crate, a sphere and a column arch, the sun turning; MetalFX spatial and temporal upscaling. On SDL GPU (Metal or Vulkan through MoltenVK) the same scene with the fallbacks and a line saying why | sun shadows (shadow maps or ray traced), shadow rays, softness, ambient occlusion with its rays and radius, upscaling (bilinear, MetalFX spatial, MetalFX temporal) and render scale, each with the fallback it got; turn the sun, walk, sun elevation; the top level's instances, builds and refits (or the shadow draws), and outside deterministic runs the GPU time of the rays, the shadow maps, MetalFX and the frame |

Every section's panel has the same parts in this order: the controls, the section's numbers ("Stats"), the
"Frame and fallbacks" header (the scene's counts, the 3D resolution, draws, culled instances, lights, shadow draws,
what runs after the device fallbacks: sun shadows, cascades, depth clamp, ambient occlusion, anti-aliasing,
upscaling, translucent shadows, bloom and sky; the HDR and depth formats; every fallback with its reason, from
`Renderer::fallbacks()`; the GPU backend and its capabilities, and why there is no ray tracing where there is none),
and "Reset section" and "Reset camera". The description of what to look at is a caption under the scene (in the
compact layout, the top of the controls sheet).

## Coverage

What of the engine the demo shows, and where (audit of 2026-11-06; "added" marks what stage 10 added):

| Engine area | Where | How it shows |
|---|---|---|
| Retained scene, PBR materials, point, spot and sun lights | materials | spheres from smooth to rough, metals, glass, an emissive lamp, lights on and off; the scene's counts in "Frame and fallbacks" (added) |
| Game surface shaders and render passes | materials | the checker ground, the light discs, each on and off |
| Cut-out and translucent shadows | materials | the fence's holes, the glass's tinted shadow; the transmittance map or its dithered fallback |
| Cascaded sun shadows | shadows | cascades 0 to 4, map size, distance, filter, the turning sun; depth clamp or its fallback |
| Sky, atmosphere, fog | sky | the day cycle, the atmosphere and its gradient fallback, no sky, height fog and its sun scatter |
| Bloom, tone curves, render scale | post | bloom, ACES and AgX, exposure, vignette, dither, render scale, sharpen |
| Anti-aliasing | post | none, FXAA, TAA, with the fallback the device gave |
| HDR display output | hdr | off, auto, on; the display's headroom; a custom peak; the SDR curve beside it |
| Physics on Jolt | physics | bodies dropped and blasted at 120 Hz, drawn interpolated |
| Input actions | input; every 3D section | actions on keys, mouse, gamepad and touch with their times; the shared camera controls (added: pan, the touch gestures, the keyboard) |
| Asset loading, placeholders, failures | assets | gray boxes while loading, magenta for the corrupt and the missing model; ready and failed counts |
| Cache budgets, upload budget | assets | budgets per type, evictions, uploads per frame |
| Streaming by distance | assets | the tile grid around a moving focus; the load and keep rings on the ground and every model's load state (added) |
| Hot reload (Mac) | assets | the hot reload switch, "Reload all", and a note "Assets reloaded: N" over the scene (added) |
| Audio | audio | buses, 3D sound, Doppler, a generator, streamed music with a crossfade, voice stealing, meters |
| Animation | animation | blending, state machine, layers, IK, foot placement, attachments, events, root motion, a crowd on the job system, GPU skinning |
| Ray tracing, MetalFX | raytracing | ray-traced shadows and occlusion, MetalFX spatial and temporal, each with its fallback and the reason |
| The job system | the Performance panel | threads and pending jobs, a job test (256 items on the pool, its time and the items per thread) (added); physics, animation and loads run on it |
| Frame, CPU and GPU times | the Performance panel | fps, frame, CPU and GPU time, a frame-time graph (added), the GPU time per pass on the native Metal backend; hidden in deterministic runs (captures) |
| Memory and GPU resources | the Performance panel | the process's memory and the live GPU resources, the section's load and switch times (added) |
| GPU backend, device, fallbacks | every section's "Frame and fallbacks"; About | the backend, driver, device and capabilities, every fallback with its reason (added); a relaunch with the other backend on the Mac (added) |
| Display scale, safe area, orientation | the layouts | sizes from the display's scale, windows inside the safe area, the compact layout in portrait and landscape (added) |
| Logging | the Log panel | the demo's log lines with the warning and error counts (added) |
| User files through `quench::fs` | Settings, the check mode, the benchmark | `settings.json`, `checks.json`, `benchmark.json`, screenshots (added) |
| Scenarios, captures, perf regions | the checks | every section's check; the benchmark of every section on the same perf regions (added) |

Not shown, because the engine does not have it yet: shader hot reload, normal maps, ASTC or Basis Universal
textures, asset hot reload on a device, HRTF, reverb, retargeting, blend spaces, ray-traced reflections and indirect
light ([Roadmap](roadmap.md)).

## Layouts

Sizes follow the display's scale (the engine's ImGui style), times the UI scale of the settings.

- **Wide** (a desktop window, a tablet): the menu at the top left (the sections numbered 1 to 11, the mode buttons
  Tour, Checks and Benchmark, About, Settings, Hide, the Performance and Log panels, and a hint of the keys under
  them: "Sections: keys 1-9, 0 = 10", "Previous, next: PgUp PgDn LB RB"); the section's window at the top right, or
  under the menu where the window is too narrow for both; the description as a caption at the bottom; the windows of
  the modes at the bottom in its place. Both windows collapse with their title's arrow.
- **Compact** (a safe area under 40 x 26 em: phones in either orientation, small windows): a bar at the top (Menu,
  previous, the section's title, next, Controls) and one sheet at a time: the menu on the left; the section's panel
  docked at the bottom in portrait and on the right in landscape, so the scene stays visible; About and Settings over
  the screen. A drag on a sheet's background scrolls it.
- Both keep every window inside the safe area (the notch, the home indicator, rounded corners).
- Touch-sized controls (taller items, wider scroll bars and grabs, a touch margin) where the device has a touch
  screen (iOS), or always or never by the setting. Touches on a Mac trackpad are not a touch screen.

## Controls

| | Mouse and keyboard | Gamepad | Touch |
|---|---|---|---|
| Orbit | left drag; the arrows | right stick | one finger |
| Pan | right or middle drag | left stick | two fingers |
| Zoom | the wheel; + and - | the triggers | pinch |
| Act on the scene (blast, ping, fire) | a click | the section's button | a tap |
| Next, previous section | `]` `[`, Page Down, Page Up | the shoulder buttons | the arrows of the bar (compact) |
| A section | keys 1 to 9 for sections 1 to 9, 0 for section 10 (11 by next or previous); a click on the menu | | a tap on the menu |
| Menu by keys and buttons | Tab, then the arrows and Enter; Escape goes back | the D-pad, then A; B goes back | |
| Hide the windows | H | Start | Hide in the menu |

The camera's keys and sticks work while the scene has the keyboard and the gamepad: Tab or the D-pad hand them to
the windows (ImGui navigation, focus on the current section), Escape, B or a click on the scene hand them back. The
input section's character takes W A S D, the left stick and the fingers, so there only the mouse orbits and pans
(every device still zooms); a section's own keys are in its description. The same list is in About, under Controls.

## Modes

The menu's buttons or a flag start them; one runs at a time, and its window at the bottom shows where it is.

- **Tour** (`--tour [SECONDS]`, default 8; the Tour button takes the Tour time of the settings): every section in turn for
  SECONDS each, round and round, from the current one. Any key, click, touch, button or stick stops it.
- **Check mode** (`--checks`): the checks of the devlog that wait for a person ("Checks waiting for the user"), 24
  items, one at a time: it switches to the item's section, sets it up (a camera, a setting) and says what to look at
  or listen to. OK, Problem or Skip, with a note, marks the item and goes to the next; the arrows go back and
  forward, End stops. Every mark is written to `checks.json` in the user directory at once, with the backend and the
  time; the next run starts at the first item not marked yet, with the earlier marks (a file that cannot be read
  starts empty, with a warning). The items: the fence's holes and the glass's tinted shadow (materials), the
  cascades (shadows), the day and fog (sky), anti-aliasing (post), HDR on an HDR display (hdr), the bodies
  (physics), the devices (input), loading and streaming (assets), panning, Doppler, the tone without clicks, the
  music's crossfade, the buses and the pings (audio, by ear), feet that do not slide, footsteps in sync, layers and
  look-at, foot placement and the crowd (animation), noise, MetalFX ghosting and the fallback (raytracing), the frame
  rate and the layout on the device.
- **Benchmark** (`--benchmark-all [SECONDS]`, default 5; the Benchmark button: 5 s): every section measured in turn
  as `scenarios/benchmark.txt` measures it: a warm-up of a second (or SECONDS, if shorter) and until the section's
  assets are in (at most 6 s), then a perf region of SECONDS in real time (`App::beginPerf` .. `endPerf`). One report
  for all of them, `benchmark.json` in the user directory; `--benchmark-all` quits when it is written (exit code 1
  if it could not be). Use Release, vsync and a fixed window size for numbers that compare
  ([Performance](performance.md)).

### Flags

| Flag | |
|---|---|
| `--section ID` | start in a section: `materials`, `shadows`, `sky`, `post`, `hdr`, `physics`, `input`, `assets`, `audio`, `animation`, `raytracing` (default: the last one, from the settings); an unknown id fails |
| `--tour [SECONDS]` | the tour, SECONDS per section (0.1 to 600) |
| `--checks` | the check mode |
| `--benchmark-all [SECONDS]` | the benchmark of every section (0.1 to 120 s each), then quit |
| `--no-settings` | neither read nor write `settings.json` |
| `--settings-file NAME` | the settings file in the user directory (default `settings.json`); in a scripted run the only way to use one |
| `--checks-file NAME` | the check results (default `checks.json`) |
| `--benchmark-file NAME` | the benchmark report (default `benchmark.json`) |

`--tour`, `--checks` and `--benchmark-all` exclude each other. The engine's flags (`--help`,
[App shell](app.md#command-line)) work as everywhere: for the assets section also `--asset-latency SECONDS`,
`--hot-reload` and `--worker-threads N`, for the audio section `--audio-device NAME`, `--no-audio` and
`--audio-capture FILE.wav`; `--gpu-backend metal` for the native Metal backend, `--user-dir DIR` for another user
directory, `--orientation portrait|landscape` on a phone.

### Files

All three are JSON with `"version": 1`, in the user directory (`~/Library/Application Support/Quench/Quench Demo/`
on the Mac, `Library/Application Support/Quench/Quench Demo/` in the app's data container on iOS) through
`quench::fs`.

- `settings.json`: `section` (the last one), `show_ui`, `ui_scale` (0.5 to 3), `touch_controls` (`auto`, `on`,
  `off`), `hdr` (`off`, `auto`, `on`), `present_mode` (`vsync`, `immediate`, `mailbox`), `tour_seconds` (1 to 120).
  Written half a second after a change and at the end. A file that cannot be read or parsed, or of another version,
  gives the defaults and a warning, and the next save writes a good one. `--hdr` and `--present-mode` on the command
  line win over the file. Scripted runs (`--script`) ignore the file unless `--settings-file` names one, so checks
  start from the defaults and leave the user's file alone.
- `checks.json`: `app`, `app_version`, `build` (the build type and the commit), `backend`, `driver`, `device`,
  `platform`, `written` (UTC); `items`, one per check: `id`, `section`, `title`, `status` (`pending`, `ok`,
  `problem`, `skipped`), `note`, `backend` and `time` of the mark; `summary`: the count per status.
- `benchmark.json`: the same run information, `seconds`, `window` (pixels); `sections`, one per section: `section`,
  `frames`, `frame_avg_ms`, `frame_p50_ms`, `frame_p99_ms`, `frame_max_ms`, `cpu_avg_ms`, `cpu_p50_ms`,
  `cpu_p99_ms`, `gpu_avg_ms`, `gpu_p50_ms`, `gpu_samples`, `pass_p50_ms` (the median per timed GPU range, native
  Metal backend), `details` (the renderer's state, as on a `perf:` line).

## About and settings

About (the menu's button) shows the version, the build type and commit, the platform, the GPU backend, driver and
device with ray tracing, MetalFX and GPU timing, the window and its pixel density, the display's headroom, the audio
device and the job threads; the controls; the engine's licence and the third-party licences (shipped as assets,
`quench_cook_assets(... FILES ...)`, read when About first opens). On the Mac it has a button that starts the demo
again with the other GPU backend (`App::relaunch`: the same command line with another `--gpu-backend`; the settings
are saved first). iOS cannot start an app again from inside: the backend is chosen at launch, with
`--gpu-backend metal` or the environment variable `QUENCH_GPU_BACKEND=metal` (below).

Settings: the UI scale (applied when the slider is let go), touch-sized controls (auto, on, off), HDR output, the
present mode, the tour's time, Defaults, and Screenshot (`screenshot-N.png` in the user directory).

## Check mode on the iPhone

Build and install the demo as in [App shell](app.md#ios-runs) (bundle id `org.quench.demo` with the paid team's
empty suffix; the phone unlocked), then either open the menu and tap Checks, or start it with the flag:

```sh
cmake --preset host-tools && cmake --build --preset host-tools
cmake --preset ios-xcode -DQUENCH_IOS_TEAM=<team id> -DQUENCH_IOS_BUNDLE_ID_SUFFIX=
xcodebuild -project build/ios-xcode/Quench.xcodeproj -scheme quench_demo -configuration Release \
    -destination id=<device udid> -allowProvisioningUpdates build
DEVICECTL=/Library/Developer/PrivateFrameworks/CoreDevice.framework/Versions/A/Resources/bin/devicectl
$DEVICECTL device install app --device <udid> build/ios-xcode/examples/demo/Release-iphoneos/quench-demo.app
$DEVICECTL device process launch --device <udid> --console --terminate-existing org.quench.demo --checks
# the native Metal backend (ray tracing, MetalFX, GPU times per pass) for the ray tracing items:
$DEVICECTL device process launch --device <udid> --console --terminate-existing \
    --environment-variables '{"QUENCH_GPU_BACKEND":"metal"}' org.quench.demo --checks
# the results back to the Mac:
$DEVICECTL device copy from --device <udid> --domain-type appDataContainer --domain-identifier org.quench.demo \
    --source "Library/Application Support/Quench/Quench Demo/checks.json" --destination /tmp/checks.json
```

The marks survive between runs, so the SDL GPU items and the native Metal items can be done in two launches. On the
Mac: `build/release/examples/demo/quench-demo --checks` (and `--gpu-backend metal`); the file is in
`~/Library/Application Support/Quench/Quench Demo/`.

## Content

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
checks and copies them into `assets/`; `sounds/step.wav` (a short footstep, stage 8) is one more. Building the demo
therefore needs Python 3.

The animation section's character is not in git either: `tools/assets/make_demo_character.py` (Python standard
library, procedural, CC0) writes `characters/mannequin.gltf` and `.bin` (one skin of 20 joints, 836 vertices, clips
idle, walk, run, jump, wave and lean, the legs solved by IK from foot paths so planted feet stay put) and
`mannequin.anim.json` (loops, the additive lean, footstep and landing events, root motion on `root`) at build time;
`quench_cook_assets(quench_demo_character ... MODELS ... SKELETONS ... ANIMATIONS ...)` cooks the model, the
skeleton and the clips. The graph, `examples/demo/assets/characters/mannequin.graph.json`, is in git. The section has
its own asset manager on the app's job system (one `SceneAssets` per manager, [Jobs, assets and the
cook](assets.md#textures-and-models-in-a-scene)) and its own `AnimationSystem`; it restarts on `set restart 1`,
which makes its capture independent of when the loads finished.

The licences About shows are the engine's `LICENSE` and `THIRD_PARTY_LICENSES`, copied into `assets/` by
`quench_cook_assets(quench_demo_licences ... FILES ...)`; the commit it shows comes from `BuildInfo.cmake`, which
writes `demo_build_info.h` at build time only when the commit changes ("unknown" without git; uncommitted changes
are not marked).

## Checks

The demo's checks run in three batches ([App shell](app.md#captures-and-golden-images)): one offscreen app process
per GPU backend, `--window 960x540 --gpu-debug --hdr-headroom 4`, runs every check as a part of one scenario.

| Batch | Scenario | Backend |
|---|---|---|
| `demo_batch_metal` | `batch_metal.txt`: `batch.txt`, then `cycle.txt` | SDL GPU's Metal driver, with the Metal API validation layer |
| `demo_batch_metal_native` | `batch_native.txt`: `batch.txt` with `raytracing.txt` in place of `raytracing_fallback.txt`, then `cycle.txt` | the native Metal backend, with the validation layer; compared with the Metal golden images |
| `demo_batch_vulkan` | `batch.txt` | Vulkan through MoltenVK with the Khronos validation layer (an error or warning fails the batch) |

`batch.txt` includes, in this order: materials, shadows, sky, post, hdr, hdr_on, physics, input, assets, audio,
animation, raytracing_fallback, ui, compact_portrait, compact_landscape, menu, audio_tone, audio_flyby, audio_music.
Every part selects its section first (and resets it where it needs the defaults), so it does not depend on the parts
before it, and its `time` starts at 0. A test per part and backend, `demo_<part>_<backend>`, checks the part's line
in the batch's log (`part <file>: N passed, 0 failed`) and compares its capture with
`tests/golden/<backend>/<device>/<golden>.png`. The batches run in Release builds (`QUENCH_TEST_CAPTURES`); Debug
and the sanitizer builds run `demo_smoke` instead (`smoke.txt`: every section once, offscreen, with assertions or
under the sanitizers). The scenarios point at the window in percent or at named points (`@menu:<section>`,
`@floor`), so their checks also pass in a portrait phone window (the captures are compared on the Mac only).

The section captures show the scene only: before each, the part sends `windows off`, which hides the menu, the
section's panel, the caption, the hints and the top bar, and `windows on` after it. A section's own on-screen
elements stay where they are what is checked. The menu and the layouts have captures of their own.

| Part | What it checks | Capture |
|---|---|---|
| `materials`, `shadows`, `sky`, `post`, `hdr`, `physics` | the section's options switched one by one, the renderer's and the section's values, the start state put back | `demo_<part>.png` at 2 s: the scene |
| `hdr_on` | `hdr on`: the output is HDR extended linear with headroom 4 and the capture's peak is above 2 and at most 4 times paper white; `hdr off` returns to SDR | `demo_hdr_on.png` at 2 s (clipped at paper white): the scene |
| `input` | keyboard, gamepad and touch actions, the virtual stick | `demo_input.png` at 3 s: the scene (the touch overlay, fingers and stick, is drawn only while a finger is down) |
| `assets` | below | `demo_assets.png` at 1.5 s: the scene with the section's overlay (each model's load state, the streaming rings, the reload note), which is what the section shows |
| `audio` | every emitter, the tone, the crossfade to music B and a burst of 40 pings, no underruns or clipped samples (the mix runs offline on the frame clock, as in every offscreen run) | `demo_audio.png` at 1.65 s: the scene |
| `animation` | below | `demo_animation.png` at 2.4 s: the scene |
| `raytracing` (native batch) | ray-traced shadows and occlusion run (no cascades, no shadow draws, at least 12 instances in the top level, the character skinned), shadow maps come back when asked, MetalFX temporal at scale 0.5 resolves in TAA's place, spatial runs, bilinear at scale 1 again | `demo_raytracing_native.png` at 2.5 s (rays and occlusion at full size, the sun and the character moving): the scene |
| `raytracing_fallback` (Metal, Vulkan) | the requests fall back (cascades, no occlusion, no top level, bilinear at scale 0.5), and the renderer lists the fallbacks | `demo_raytracing.png` at 2.5 s: the scene |
| `ui` | the wide layout with its windows | `demo_ui.png`: the materials section with the menu, its panel and the caption |
| `compact_portrait`, `compact_landscape` | `compact.txt` in a phone-sized frame (400 x 720 and 720 x 320 points): the compact layout, the bar's arrows, the menu sheet, About and its licences, the controls sheet | `demo_compact_portrait.png`, `demo_compact_landscape.png`: the controls sheet over the materials section |
| `menu` | switching sections by keys, gamepad, a click and a tap on the list; the menu by Tab, the arrows and Enter and by the D-pad, A and B; hiding the windows | none |
| `audio_tone`, `audio_flyby`, `audio_music` | the checks on the mix itself (below) | none |
| `cycle` (the Metal batches) | every section three times (created or reset, switched, reset), with the memory, the live GPU resources, the scene's instances and the error count marked after the first round: no growth (the GPU resources within 3, fences and transfer buffers come and go; memory below 32 MB; no instance more; no new error) | none |

The cycle does not run on Vulkan through MoltenVK: there the memory grows by hundreds of MB in the reset round (459
MB in the batch with the validation layer, 2026-11-06; the cycle never ran on Vulkan before the batches), which is
not explained yet. Its first round resets every section, so the settings the parts before it changed do not count
as growth.

Every section has a clock of its own (`Section::sectionTime`), which runs only while the section is shown: its
animations and the surface shaders' time follow it, so a capture does not depend on how long the batch ran before
the part. The renderer's own state still does in part: the native ray tracing capture from a run of its own and
from the batch differ by a mean of 0.32 (0.09% of the pixels by more than 16), within the thresholds. Two batch runs
of the same binary give the same bytes in every capture (48 captures, 2026-11-06).

Outside the batches, with runs of their own because they need flags or files of their own:

| CTest test | What it runs |
|---|---|
| `demo_tour` | `--tour 0.1` with `scenarios/tour.txt`: a full round back to the first section, every section initialized, a key stops it, `mode tour` starts it again from the current section |
| `demo_checks`, `demo_settings`, `demo_benchmark` | `tests/DemoUserFiles.cmake` runs the demo on a fresh `--user-dir` and reads the file it wrote: the check mode's marks by command and by the buttons, read back by a second run, a corrupt `checks.json` starts empty (`checks*.txt`); settings saved, read back, a corrupt file gives the defaults and is written again, `--no-settings` leaves it (`settings_*.txt`); `mode benchmark 0.1` writes a report with 11 sections (`benchmark_all.txt`) |
| `demo_version`, `demo_bad_section`, `demo_bad_flag` | `--version` prints "Quench Demo <version>"; an unknown `--section` fails; `--tour` with `--checks` fails |

The batches write no WAV file: `demo_audio_wav_match`, which compared the WAV files of the Metal and the Vulkan audio
check, is gone. The audio part's checks run on all three backends.

The assets check (`scenarios/assets.txt`, about 3.6 s per backend) loads with a simulated latency of 0.2 s and checks
the placeholders first (loads in flight, no model ready), then waits for the manager to be idle: three models ready,
the corrupt and the missing one failed (two failures in all), more than four streamed cells. It reloads every asset
(at least six reloads, the failures stay failures), lowers the texture cache to 4 KB, turns the focus to the other
side of the grid and waits for cells to unload and the cache to evict. It runs with FXAA, not TAA: loads finish in
real time, and with TAA the history of the frames before a load arrived made two runs differ.

The animation check (`scenarios/animation.txt`, 21 checks) waits for the character to load, restarts the section at
0.5 s of app time with a crowd of 24 at 1.6 m/s (27 characters, all bound), triggers a jump (the state changes, the
landing event comes, the state returns to the blend), checks the footstep events, the foot placement (both legs of
the ramp stand reach, the step stand's raised leg is grounded about 0.2 m up and the other on the floor, the ramp
stand's pelvis moves down), that the torch stays in the hand (`held_distance`) and the look-at, then waves and is
captured at 2.4 s. It runs with TAA like the others: the section restarts at a fixed time, so the capture does not
depend on load timing (two runs gave the same image on each backend).

The audio mix checks are three scenarios that run in parallel: `audio_tone.txt` (23 checks, about 1.7 s of app
time), `audio_flyby.txt` (11 checks, 2.2 s) and `audio_music.txt` (13 checks, 3.9 s). Each turns the orbit, the
fly-by and the music off, waits for the sounds and re-syncs to a fixed app time (loads finish in real time), measures
the mix with `audio_measure` ([App shell](app.md#scenarios)) and ends with no underruns or clipped samples. The tone:
1 kHz at 2 m in front (RMS 0.058 to 0.067, 998 to 1002 Hz), moved to the right (left channel below 0.002), at 8 m
with the inverse, linear and no attenuation, gliding to 1500 Hz, then a burst of 40 pings steals voices. The fly-by's
fundamental approaching (477 to 486 Hz from 440 Hz at 30 m/s) and receding (401 to 410 Hz). Music A (110 Hz), the
crossfade to B (two streams, then one, 147 Hz), the music bus at 0 (silent). Every test runs offscreen, where the mix
is offline, so no test plays through the speakers.

Named points: `menu:<section id>` (a menu entry while the menu is shown), `ui:<name>` (the windows' buttons:
`ui:sections`, `ui:prev`, `ui:title`, `ui:next`, `ui:controls`, `ui:close`, `ui:tour`, `ui:checks`,
`ui:benchmark`, `ui:about`, `ui:settings`, `ui:hide`, `ui:job_test`, `ui:reset_section`, `ui:reset_camera`,
`ui:relaunch`, `ui:stop`, `ui:check_ok`, `ui:check_problem`, `ui:check_skip`, `ui:check_prev`, `ui:check_next`,
`ui:check_end`, `ui:check_note`), and the section's (`floor`, ...).

Offscreen frames are not paced ([App shell](app.md#command-line-flags)), except while assets load: the three
batches together take about 9 s (2026-11-06, in parallel; each 7 to 9 s for 290 to 310 checks). On the screen
(`--onscreen`) a scripted run holds every frame to 120 per second, and with vsync also to the display.

The scenarios turn dithering off (`set dither 0`), which keeps the golden PNGs at 120 to 380 KB. Deterministic runs
(`--script`, `--capture`; not inside a perf region) hide the timings, the memory and the Log panel's counts, which change from run to run.
Game values a scenario can read: `section`, `sections`, `ui`, `aa` (0 none, 1 FXAA, 2 TAA, after fallbacks), `cascades`, `bloom`,
`draws`, `instances`, `culled`, `shadow_draws`, `shadow_instances`, `transmittance_draws`, `translucent_shadows` (0
transmittance map, 1 dithered, after fallbacks), `moved`, `lights`, `render_w`, `render_h`,
`camera_yaw`, `camera_pitch`, `camera_distance`, `clicks`, `skinned_instances`, `skinned_vertices`, `sun_shadows`
(0 maps, 1 ray traced), `ambient_occlusion` (0 none, 1 ray traced), `upscaling` (0 bilinear, 1 spatial, 2 temporal),
`render_scale`, `ray_instances`, `ray_builds`, `ray_refits` (all after fallbacks), `fallbacks` (their count),
`camera_x`, `camera_z` (the target); the demo's own: `section_ready`, `sections_ready`, `init_ms`, `switch_ms`,
`scene_instances`, `compact`, `sheet` (0 none, 1 the menu, 2 the controls, 3 About, 4 Settings), `menu_focus`,
`about`, `licence_bytes`, `ui_scale`, `touch_ui`, `settings_loaded` (1 read, 0 not, -1 unusable), `settings_writes`,
`mode` (0 none, 1 tour, 2 checks, 3 benchmark), `tours`, `check_index`, `check_items`, `check_writes`,
`checks_pending`, `checks_ok`, `checks_problem`, `checks_skipped`, `benchmark_sections`, `benchmark_written`,
`log_errors`, `log_warnings`, `job_test_ms` (-1 until a job test finished); then the
section's own (for example `bodies`, `blasts`,
`player_x`, `jumps`, `marker_draws`, `exposure`; the assets section's `assets_loading`, `assets_ready`,
`assets_failed`, `assets_idle`, `assets_loads`, `assets_reloads`, `assets_evictions`, `showcase_ready`,
`showcase_failed`, `streaming_active`, `streaming_loads`, `streaming_unloads`; the audio section's `sounds_ready`,
`music_track`, `flyby_x`, `flyby_pitch`, `pings`; the engine's `audio_*` values; the animation section's
`anim_ready`, `anim_failed`, `anim_characters`, `anim_bound`, `anim_ground_rays`, `hero_state` (0 the blend, 1 the
jump), `hero_transition`, `hero_look_at`, `hero_angle`, `footsteps`, `landings`, `crowd`, `held_distance`,
`slope_pelvis`, and per stand and leg `slope_leg0_offset` (also `_reached`, `_grounded`; `slope` or `step`, leg 0
or 1); the ray tracing section's `rt_ready`, `sun_azimuth`). Game commands: `section ID`, `reset_section`, `reset_camera`,
`sheet none|sections|controls|about|settings`, `mode off|tour|checks|benchmark [SECONDS]`,
`check ok|problem|skip [NOTE]`, `check next|prev`, `job_test`, `settings_save` (now, not half a second later),
`settings_corrupt` (overwrites the settings file with text that is not JSON),
`camera YAW PITCH DISTANCE` (degrees, meters), `set NAME VALUE` (`ui_scale`, `touch_controls auto|on|off`; render settings: `aa none|fxaa|taa`,
`tone aces|agx`, `cascades`, `shadow_size`, `shadow_filter`, `shadow_distance`, `translucent_shadows map|dithered`,
`sun_shadows maps|rt`, `shadow_rays`, `shadow_softness`, `ao none|rt`, `ao_rays`, `ao_radius`,
`upscaling bilinear|spatial|temporal`, `scale`, `bloom`, `bloom_strength`,
`sky`, `dither`, `sharpen`, `vignette`; otherwise the section's options, for the assets section `latency SECONDS`,
`texture_budget BYTES|unlimited`, `upload_budget BYTES|unlimited`, `focus_angle DEGREES`, `move_focus`, `reload`,
`models`, `hot_reload`; for the audio section `orbit`, `flyby` (also restarts the pass), `tone`, `tone_hz`,
`tone_x`, `tone_z`, `orbit_speed`, `flyby_speed`, `attenuation none|inverse|linear|exponential`, `min_distance`,
`max_distance`, `doppler`, `music 0|1|2`, `bus_<name>` (volume), `mute_<name>`, `pause_effects`, `ping`,
`burst N`; for the animation section `restart`, `speed`, `lean`, `jump`, `wave`, `foot_ik`, `look_at`,
`footstep_sound`, `slope DEGREES`, `crowd N`; for the ray tracing section `turn_sun`, `walk`, `sun_azimuth`,
`sun_elevation`).

## Benchmark

`scenarios/benchmark.txt` measures the materials, shadows, sky, post-processing, HDR (in SDR, then with HDR output on),
physics, assets, audio, animation and ray tracing sections for 5 s each in real time (budgets and numbers:
[Performance](performance.md)); the assets region streams with the moving focus and the default budgets, after the
first loads have finished; the audio region plays the three emitters, the tone and the music; the animation region
has a crowd of 256 (259 characters), and its perf line ends with the section's own times over its last 240 frames:
`anim_update_ms` (mean) and `anim_update_max_ms` of `AnimationSystem::update`, `anim_threads`, and
`skinning_gpu_ms`, the mean of the skinning pass's GPU time (-1 without GPU timing). It runs in a window on the
screen (`--onscreen`: scripted runs are offscreen otherwise, where frames are not presented) with `--audio-device
null` (the mix runs on miniaudio's null device thread, without sound):

```sh
build/release/examples/demo/quench-demo --onscreen --frame-size 1920x1080 \
    --script examples/demo/scenarios/benchmark.txt --audio-device null
```

The benchmark mode (`--benchmark-all`, above) measures every section the same way, with the windows as they are
(shown by default, as in the scenario), and writes the numbers as JSON; the scenario stays the reference for comparisons in the devlog, with its own regions (HDR on,
the crowd of 256).

`scenarios/raytracing_benchmark.txt` measures the ray tracing section's options one by one, 5 s each: rays for
shadows and occlusion, shadows alone, hard shadows (1 ray), shadow maps, then MetalFX temporal at scale 1, 0.67 and
0.5, spatial and bilinear at 0.5. Run it with `--onscreen --gpu-backend metal` (on SDL GPU every region measures the
fallbacks); the perf lines carry the GPU time per timed range ([Performance](performance.md#measurements)).

## Packaged builds: `tools/package_demo.sh`

```sh
tools/package_demo.sh          # macOS: dist/QuenchDemo-<version>-<hash>-macos.zip
tools/package_demo.sh --ios    # also iOS: dist/QuenchDemo-<version>-<hash>-ios.zip
```

`<hash>` is the short commit hash, with `-dirty` when the working tree has changes (the build is then not exactly
that commit). The script builds `quench_demo` with the `release` preset (`--preset NAME` for another), runs the
demo's tests (`ctest -L demo`: the batches and the runs with files of their own; `--no-checks` skips them when
`tools/check.sh release` has just run them on the same build), and then:

- **macOS**: assembles `QuenchDemo.app` (the executable, an Info.plist with the version and the binary's minimum
  macOS as `LSMinimumSystemVersion`, an icns made from `examples/demo/icon/icon.png`, `LICENSE` and
  `THIRD_PARTY_LICENSES` in `Contents/Resources`, the demo's cooked assets in `Contents/Resources/assets`, where
  SDL's base path of a bundled app points), checks that it links only system libraries, signs it ad hoc and
  zips it with `LICENSE` and `THIRD_PARTY_LICENSES` next to it. The archives carry no extended attributes: `ditto`
  would store them as AppleDouble `._` files, which a plain `unzip` turns into files inside the bundle that break its
  signature; the script fails on any `._` entry. It then unzips the archive with a plain `unzip` into a temporary
  directory, verifies the signature and starts the app from `/` with `scenarios/smoke.txt` (every section once,
  offscreen: no window on the screen, no sound): exit code 0, the scenario
  passed, and a capture (`dist/<name>-macos-smoke.png`) to look at. The app is signed ad hoc and not notarized: a
  copy downloaded on another Mac is quarantined, and Gatekeeper opens it only after it is allowed in System Settings,
  Privacy & Security (not tried on another Mac yet).
- **iOS** (`--ios [--team ID]`): builds `quench_demo` with the `ios-xcode` preset (Automatic signing with the team:
  `--team`, else `QUENCH_IOS_TEAM` from the environment, else the one `build/ios-xcode` is configured with; an Apple
  ID signed in to Xcode, which creates the development profile on its own), after `quench_demo_assets` on its own
  (see [Assets](assets.md)), checks that the app's `assets/` matches the staged folder (`diff -r`), and packs the
  signed app as `QuenchDemo.ipa`, with the licences, into the zip. The team must be a paid one: the script refuses a free Personal
  Team (Xcode's account data marks it), whose profiles expire after 7 days, and checks that the app is signed by the
  team (`codesign`, `TeamIdentifier`), also after a plain `unzip` of the `.ipa`. It runs only on the devices in the
  profile, and the profile expires (the paid
  team's wildcard profile made on 2026-11-05 lasts until 2027-11-05, with 5 devices). The script prints the team, the
  signature, the profile's name, expiry and device count. Nothing is installed on a device by the script.

`dist/` is git-ignored.
