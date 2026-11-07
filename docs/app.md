# App shell

How a game runs on Quench (`quench_app`, stage 3): the game interface, the command line flags every game understands,
verification scenarios, captures and golden images, and how games are built and run on macOS and iOS. All
documentation: [index](README.md).

## Game interface

The engine owns `main` (`src/app/Main.cpp`, SDL main callbacks: no blocking loop, as browsers and mobile lifecycles
require). A game links `quench_main` through `quench_add_game()` and defines one function:

```cpp
Result<std::unique_ptr<quench::app::Game>> quench::app::createGame(AppDesc& desc,
                                                                   std::span<const std::string_view> args);
```

It runs before SDL is initialized, with the arguments the engine did not consume. The game fills `AppDesc` (name,
organization, version, asset directory, window size, fixed step, present mode) and returns its `Game`, or an error
for an argument it does not know (the process then exits with 1). A null `Game` without an error ends the program
with 0, for game flags that only print (TrailForge's `--paths`). `quench_add_game()` defines `QUENCH_GAME_NAME` and
`QUENCH_GAME_VERSION` for the game's sources.

`Game` (`include/quench/app/App.h`) has `init`, `fixedStep`, `update`, `ui`, `render`, `event`, `shutdown`, and
`scenarioValue` / `scenarioCommand` / `perfDetails` for scripts. Per frame:

1. SDL events: ImGui, then `Input`, then `Game::event`.
2. Input actions are updated; a running scenario executes its commands.
3. Fixed simulation ticks (`AppDesc::fixedStep`, default 1/120 s): none, one or several `fixedStep` calls.
   `App::alpha()` is the fraction of a tick since the last one, for interpolation in `render`.
4. `update` once, then the audio system's `update` (the frame's sound changes go to the mixer; without a device the
   frame's audio is mixed offline here), `ui` (ImGui windows), then `render` records the game's passes into
   `Frame::commands` and `Frame::target`; the engine draws ImGui on top and presents.

On iOS the app stops rendering between `SDL_EVENT_WILL_ENTER_BACKGROUND` and `SDL_EVENT_DID_ENTER_FOREGROUND`, and the
clock is reset on return, so no ticks pile up; the audio device is stopped in between and started again on return. A
minimized desktop window also skips rendering (its sound keeps playing).

`App` gives the device, the engine's job system (`jobs()`, one pool for the engine and the game) and asset manager
(`assets()`, updated every frame before `Game::update`; both created before `Game::init` and destroyed after the
game, [Jobs, assets and the cook](assets.md)), the audio system (`audio()`, created after the asset manager with
`AppDesc::audio`, its output from `AppDesc::audioDevice`; `audioDevice()` is null while the mix runs offline;
[Audio](audio.md)), the window, `Input`, sizes in points and pixels, the safe area
(notches, rounded corners),
time, frame and tick counters, frame statistics (`FrameStats`: frame time, CPU time until the submit without the wait
for a swapchain texture, GPU time where `gpu::Caps::gpuTiming`, and GPU time per timed range where
`gpu::Caps::gpuPassTiming`: the app times `game` around `Game::render`, `ui` and `blit`, the renderer its passes inside
`game`; [Performance](performance.md#gpu-time-per-pass); recorded only in frames that need them: a frame panel calls
`requestPassTiming()` each frame it shows them, and a scenario's perf region and `--gpu-pass-timing` turn them on, so
the ranges arrive a few frames later), the display output (`output()`, `hdrMode()`,
`hdrSupported()`, `displayHeadroom()`, `displayPotentialHeadroom()`), and actions: `requestCapture`,
`setPresentMode`, `setHdrMode`, text input,
`quit(exitCode)`. For tools inside a game: `beginPerf(label)` .. `endPerf()` measure a region as a scenario's
`perf_begin` .. `perf_end` does (real time, the same `perf:` line and `perf_*` values) and return its numbers as a
`PerfReport` (a game's benchmark mode); `arguments()` is the command line the app was started with, and
`relaunch(args)` starts the program again with other arguments and quits (`canRelaunch()`: desktops only; not phones,
tablets or the web), for example with another `--gpu-backend`; `mobile()` says the app runs on a phone or tablet
(iOS, also in the Simulator, and Android), for lighter quality defaults where sustained GPU load lowers the clocks
(the demo's ray tracing section; effects still fall back by `gpu::Caps`); `memoryFootprint()` is the memory the system charges
the process (Apple platforms; nullopt elsewhere). `Frame::output` says what the values written to `Frame::target` mean (SDR, or HDR with its
headroom); the renderer takes it as is ([HDR output](#hdr-output)).

The app has no animation system: a game that animates creates its own `animation::AnimationSystem` on
`assets()` and `jobs()` (it registers its asset types there; [Animation](animation.md)) and calls its `update(dt)`
in `Game::update`, after setting its parameters and before handing the palettes to the scene.

## Input

`quench::app::Input` reads SDL events: keyboard, mouse, the first connected gamepad and touch fingers. Games define
named actions bound to physical controls:

```cpp
const ActionId jump = input.addAction("jump", {*Binding::key("Space"), Binding::gamepad(GamepadButton::South)});
const ActionId left = input.addAction("left", {*Binding::key("A"), Binding::gamepadAxis(GamepadAxis::LeftX, false)});
```

- `value(action)` is 0..1 (keys and buttons 0 or 1, axis directions after the dead zone); `down`, `pressed`,
  `released`; `axis(negative, positive)` is -1..1.
- `deadZone` (0.15): radial for sticks, linear for triggers; the rest is rescaled to 0..1.
- While ImGui wants the keyboard or the mouse, key and mouse bindings read as up (the raw state stays readable).
- Fingers are in window points with a per-frame delta. SDL also synthesizes mouse events from fingers;
  `mouseFromTouch()` tells them apart. A Mac trackpad's touches are not fingers (SDL sends them with
  `SDL_MOUSE_TOUCHID`, at positions on the pad, while they already move the mouse): `Input` ignores them.
- Key names are SDL scancode names (`"W"`, `"Space"`, `"Left Ctrl"`); gamepad buttons follow SDL's order (South is A
  on Xbox, Cross on PlayStation).

## ImGui

Dear ImGui 1.92.9b with the engine's own renderer on `quench::gpu` (`src/app/ImGuiRenderer.cpp`): dynamic font
textures (`ImGuiBackendFlags_RendererHasTextures`), any swapchain format (sRGB-aware), clip rectangles in pixels. The
font and the style scale with the display's content scale, 1.5 times larger on iOS and Android. `imgui.ini` is not
written. `quench::app::imguiTexture(texture)` gives an id for `ImGui::Image`. The renderer is public
(`quench/app/ImGuiRenderer.h`), so a program with its own window and loop (a game on its way to the app shell) can
draw ImGui through `quench::gpu` without `App`: it creates the ImGui context and the SDL3 platform backend itself.

## Command line flags

`--help` prints them; arguments the engine does not know go to `createGame`.

| Flag | Meaning |
|---|---|
| `--quit-after S` | quit after S seconds of app time |
| `--capture FILE.png` | save one frame (with ImGui) as PNG; relative paths go to the user directory, absolute ones are used as given |
| `--capture-at S` | app time of the capture (default 3) |
| `--script FILE` | run a verification scenario; the exit code is 1 if a check failed. A host path; a relative path not found there is read from the user directory (on a device: copy the script there first) |
| `--window WxH` | window size in points (desktop) |
| `--frame-size WxH` | render every frame offscreen at W x H pixels and scale it to the window, so captures do not depend on the display density |
| `--offscreen` | no window on the screen ([Offscreen runs](#offscreen-runs)); the default on desktops with `--script`, `--capture` or `--quit-after` |
| `--onscreen` | a visible window also with `--script`, `--capture` or `--quit-after`: to watch a scenario, and for benchmarks |
| `--gpu-backend NAME` | `sdl` (SDL GPU) or `metal` (the native Metal backend, [Architecture](architecture.md#native-metal-backend); Apple platforms, elsewhere SDL GPU with a warning); default `AppDesc::gpuBackend`, which reads the `QUENCH_GPU_BACKEND` environment variable and otherwise takes SDL GPU |
| `--gpu-driver NAME` | SDL GPU's driver: `metal`, `vulkan` or `direct3d12` (the native backend has only `metal`) |
| `--gpu-debug` | GPU validation (the backend's debug mode) |
| `--gpu-pass-timing` | record the GPU time per pass in every frame (`gpu::Caps::gpuPassTiming`: the native Metal backend); without it only while the game asks (`App::requestPassTiming`) or a scenario's perf region runs, since recording costs CPU time ([Performance](performance.md#gpu-time-per-pass)) |
| `--present-mode MODE` | `vsync`, `immediate` or `mailbox`; falls back to vsync where the window does not support it |
| `--max-frame-rate HZ` | at most HZ frames per second on the screen, in whole display refreshes (60 on a 120 Hz display: every second one; `App::setMaxFrameRate`). iOS calls the app on every refresh of its display link and the app lets the early ones pass; desktops pace SDL's main loop (`SDL_HINT_MAIN_CALLBACK_RATE`). Offscreen frames are not limited. 0: the display's rate (the default) |
| `--hdr MODE` | HDR output: `off` (SDR), `on` (wherever the GPU backend can composite the window in HDR) or `auto` (while the display reports HDR range); default `AppDesc::hdr`, normally off ([HDR output](#hdr-output)) |
| `--hdr-headroom N` | peak of HDR output in multiples of paper white (1 to 1000) instead of the display's, for repeatable HDR runs |
| `--orientation NAME` | `portrait`, `landscape` or `any`: the screen orientations of the window on phones and tablets (`SDL_HINT_ORIENTATIONS`, within those of the app's Info.plist, `quench_add_game(... ORIENTATIONS)`; default: all of those); desktops ignore it |
| `--worker-threads N` | worker threads of the job system (0 to 64; default: hardware threads minus one); 0 runs jobs on the main thread, as in builds without threads |
| `--asset-latency S` | every asset load waits S seconds (0 to 60) before it starts, to see the placeholders |
| `--hot-reload` | load asset files again when they change on disk (the cooked files in the build tree on the Mac; on a device, files pushed into the asset overlay of a development build: [Hot reload on a device](assets.md#hot-reload-on-a-device)); on desktops also shaders when their sources change ([Shader hot reload](#shader-hot-reload)) |
| `--no-audio` | no audio device: the mix runs offline on the frame clock and is discarded |
| `--audio-device NAME` | `default`, `null` (miniaudio's null backend: a device thread without sound, for tests and benchmarks) or the first output whose name contains NAME; default `AppDesc::audioDevice` |
| `--audio-capture FILE.wav` | mix offline on the frame clock and write the whole mix as a 16-bit stereo WAV at exit (paths as for `--capture`); nothing is played |
| `--user-dir DIR` | the user directory (settings, saves, relative captures) instead of the system's, created if missing: tests start from an empty one |
| `--version` | print the version and exit |

When the audio device cannot be opened, the app logs a warning and mixes offline as with `--no-audio`. Offline mixing
advances the mix by each frame's time step, so in a deterministic run (below) the mix follows app time exactly and
the same script gives the same WAV file on every run. Offscreen runs mix offline unless `--audio-device` names a
device.

With `--script` or `--capture` the run is **deterministic**: every frame advances the app time by exactly one fixed
step and runs one tick, whatever the display rate. On the screen such a frame never takes less than the step in real
time (the engine waits for the rest); when frames take longer (60 Hz vsync), app time runs slower than real time.
Offscreen frames run as fast as they render, except while asset loads are in flight or delayed by
`--asset-latency`: loads take real time, and scripts wait for them in app time, so those frames keep the step's pace.
A scenario's `perf_begin` .. `perf_end` region runs in real time instead, with as many ticks per frame as in normal
play, so it measures the real frame cost (`App::deterministic()` is false there); offscreen it warns that the frames
are not presented, so benchmarks run with `--onscreen`. Offscreen regions still time each frame on the GPU (since
2026-11-06 also when the GPU is the bottleneck, where the frame waits for the oldest one in flight): unpaced, they suit
an A/B of GPU costs. A frame's GPU time is read once, when its fence has signalled (a frame or two later), and counts
in the region the frame was in; the frames still in flight at `perf_end` are not counted (`perf_gpu_samples`,
`perf_gpu_lost`). Games should hide timings in deterministic runs, so captures
compare across runs. `App::scripted()` tells a game that a scenario runs, for example to start from default settings
and keep the user's files untouched.

The user directory is `SDL_GetPrefPath(organization, name)`: `~/Library/Application Support/<organization>/<name>/` on
macOS, `Library/Application Support/<organization>/<name>/` in the app's data container on iOS; `--user-dir`
replaces it.

### Offscreen runs

Automated runs (`--script`, `--capture`, `--quit-after`) are offscreen on desktops unless `--onscreen` is given, and
`--offscreen` makes any run so. Nothing of the app reaches the screen or the user's devices:

- **No window:** the SDL window is created hidden and never shown or claimed by the GPU device; frames render into
  an offscreen texture of the window's pixel size (`--window` in points at a density of 1, or `--frame-size`) with
  the format the swapchain would have, and captures read it. On macOS the process gets no Dock icon and takes no
  focus, also as a bundle, whose Info.plist makes it a regular app (the prohibited activation policy, set before
  `SDL_Init` creates the application, with `LSBackgroundOnly` in the process's copy of the Info.plist;
  `src/app/ActivationApple.m`). The log
  says `app: offscreen: no window on the screen, frames WxH, no input from real devices`. `resize` in a script
  changes the frame size as it would the window's.
- **No input from real devices:** keyboard, mouse, touch and gamepad events from the system are dropped before they
  reach ImGui, `Input` or the game; only the script's events (tagged by the scenario runner) and its virtual gamepad
  get through, and ImGui's gamepad navigation reads only the events it is given. With `--script` this also holds on
  the screen.
- **No sound:** the mix runs offline, as with `--no-audio`, unless `--audio-device` names a device (`null` for
  miniaudio's null device thread).

What stays on the screen: the window tests of `quench_gpu_device_tests` (`window_present`,
`window_swapchain_compositions`) need a real window and a swapchain; they run only with `QUENCH_WINDOW_TESTS=1`, never
in a default run or in `tools/check.sh`; and the benchmarks, which run with `--onscreen` to measure the display's
pacing. Interactive runs (no automation flag) are unchanged.

## Scenarios

A scenario (`--script FILE`) is a text file with one command per line; `#` starts a comment. Scripted input goes
through real SDL events, so it takes the same path as a player's. Example: `examples/hello/scenarios/input.txt`.

| Command | Meaning |
|---|---|
| `wait S` | wait S seconds of app time |
| `until VAR OP VALUE [TIMEOUT]` | wait for a condition (default timeout 10 s); a timeout is a failure |
| `expect VAR OP VALUE` | check now; `OP` is `<`, `<=`, `>`, `>=`, `==`, `!=` (`==` and `!=` with a tolerance of 1e-3 × max(1, abs(value))) |
| `mark VAR` | remember a value; `d_VAR` is then the change since the mark |
| `log TEXT` | print a line |
| `include FILE` | run another scenario file (relative to the including one; up to 8 levels) as a part: its `time` starts at 0, its `quit` ends only the part, and the runner logs `part FILE` at its start and `part FILE: N passed, M failed` at its end |
| `quit` | end the scenario; the app exits with 1 if a check failed |
| `down KEY`, `up KEY`, `tap KEY` | keys by SDL scancode name; a tap releases on the next frame |
| `text STRING` | text input |
| `mouse P`, `click P [left\|right\|middle]`, `wheel DY` | mouse at the point P (below) |
| `touch P [DX DY STEPS]` | a tap at P, or a finger dragged by DX, DY (points or percent) over STEPS frames; like SDL on a device, the finger also sends mouse events with `SDL_TOUCH_MOUSEID` (unless `SDL_HINT_TOUCH_MOUSE_EVENTS` is off), so ImGui sees it and `Input::mouseFromTouch()` is set |
| `gamepad attach\|detach` | a virtual gamepad (SDL's virtual joystick) |
| `gamepad button NAME down\|up\|tap`, `gamepad axis NAME VALUE` | SDL names: `a`, `b`, `x`, `y`, `start`, `dpup`, ...; `leftx`, `lefty`, `rightx`, `righty` (-1..1), `lefttrigger`, `righttrigger` (0..1) |
| `resize W H` | resize the window (points) |
| `capture PATH` | capture the next frame; the script goes on once the file is written |
| `hdr off\|on\|auto` | `App::setHdrMode`; the output changes at the start of the next frame |
| `perf_begin LABEL`, `perf_end` | measure the frames in between, in real time; `perf_end` logs a `perf:` line (with the game's `Game::perfDetails` at the end) and sets the `perf_*` values. A frame's time runs from its start to the next frame's start: the frame before the region (a capture's encoding, for example) is not in it |
| `max_frame_rate HZ` | `App::setMaxFrameRate` (0: the display's rate); value `max_frame_rate` |
| `audio_measure S [MIN_HZ MAX_HZ]` | measure the newest S seconds (at most 4) of the mix: RMS per channel and of both, peak, and the strongest frequency between MIN_HZ and MAX_HZ (default 20 to 20000; a Hann-windowed FFT with interpolation between bins); logs a line and sets the `audio_rms*`, `audio_peak`, `audio_freq*` and `audio_measured` values. A script with this command keeps the last 4 s of the mix for it |

A point P is `X Y` in window points (`480 420`), in percent of the window's width and height (`50% 75%`; the two
forms can be mixed), or `@NAME`, a point the game names through `Game::scenarioPoint` (a UI item, a world point
projected into the window; the demo has `@menu:<section>` and the physics section's `@floor`). Points are tied to
one window size, so a script meant for every window (a portrait phone, a landscape Mac window) uses percent or names;
an unknown name fails the command.

Values: `time`, `frame`, `ticks`, `fps`, `frame_ms`, `cpu_ms`, `gpu_ms`, `window_w`, `window_h`, `pixel_w`, `pixel_h`,
`gamepad` (1 when connected), `fingers`, `action:NAME` (an action's value), `hdr` (1 while the output is HDR),
`hdr_supported`, `hdr_headroom` (of the output; 1 in SDR), `display_headroom`, `display_headroom_potential`,
`capture_peak` (of the last capture,
in multiples of paper white; -1 before one), after `perf_end` `perf_frames`,
`perf_frame_avg`, `perf_frame_p10`, `perf_frame_p50`, `perf_frame_p95`, `perf_frame_p99`, `perf_frame_max`,
`perf_cpu_avg`, `perf_cpu_p50`, `perf_cpu_p95`, `perf_cpu_p99`, `perf_gpu_avg`, `perf_gpu_min`, `perf_gpu_p10`,
`perf_gpu_p50`, `perf_gpu_p95` (milliseconds; -1 when
there were no samples, for example GPU time without `gpu::Caps::gpuTiming`), `perf_gpu_samples` (frames with a GPU
time), `perf_gpu_lost` (frames timed by the device without a sample, less those still in flight at the end: 0 unless
a sample went missing; 0 without `gpuTiming`), `perf_process_cpu` (the process's CPU time per frame, all threads)
and `perf_cpu_performance_share` (the percentage of it on performance cores; both -1 where unknown, reported on
Apple platforms; [Performance](performance.md#cpu-time-and-the-cores)), `perf_wait_p50` and `perf_wait_p95` (the
frame's wait for its swapchain texture, offscreen for the oldest frame in flight; not in the CPU time),
`perf_present_mode` (the mode in use: 0 vsync, 1 immediate, 2 mailbox; -1 offscreen) and `perf_display_hz` (the
window's display; 0 when unknown), with `gpu::Caps::gpuPassTiming` also
`perf_pass_<range>_p50` per timed range, `perf_pass_sum_p50`, `perf_pass_span_p50` and `perf_pass_share_p50`
([Performance](performance.md#gpu-time-per-pass)), and `perf_thermal_start`, `perf_thermal_end`, `perf_thermal_max`
(as `thermal_state`); after `audio_measure` `audio_rms`,
`audio_rms_l`, `audio_rms_r`, `audio_peak` (linear, 1 is full scale), `audio_freq` (Hz), `audio_freq_level` (about 1
for a full-scale sine) and `audio_measured` (seconds actually measured); at any time `audio_voices`,
`audio_hrtf_voices`, `audio_sounds`,
`audio_pending`, `audio_streams`, `audio_played`, `audio_stolen`, `audio_rejected`, `audio_failed`,
`audio_underruns`, `audio_clipped` (the `AudioStats` counters) and `audio_device` (1 with a device, 0 offline);
`memory_mb` (`App::memoryFootprint` in MiB; -1 where unknown) and `gpu_resources` (`gpu::Device::liveResources`:
textures, buffers, samplers, pipelines, transfer buffers and fences alive; fences and transfer buffers come and go
with the frames and uploads in flight), for leak checks with `mark`; `shader_reloads`, `shader_reload_failures` and
`shader_reload_pipelines` (`gpu::Device::shaderReloadStatus`: [Shader hot reload](#shader-hot-reload));
`thermal_state` (`FrameStats::thermal`: -1
unknown, 0 nominal, 1 fair, 2 serious, 3 critical; `until thermal_state <= 0 600` waits for a cool device); then the game's (`Game::scenarioValue`). Unknown commands are offered
to `Game::scenarioCommand`; a command nobody knows fails. Each check logs `ok` or `FAIL line N of FILE: ...`, and the
run ends with `N passed, M failed`; tests match `FAIL line`. Scripts are read from host paths; on a device a relative
path is read from the user directory, and an `include` there needs its file next to the including one.

## HDR output

The display output is SDR unless the game (`AppDesc::hdr`, `App::setHdrMode`) or `--hdr` asks for HDR. HDR is the
swapchain composition `gpu::SwapchainComposition::HdrExtendedLinear` (EDR on Apple platforms): an `Rgba16Float`
swapchain with linear BT.709 values, paper white at SDL's SDR white level (1.0 on Apple platforms), brighter values
up to the display's headroom. `on` uses it wherever the GPU backend can composite the window so, also on a display
without HDR range (values above paper white are then clipped by the display); `auto` only while the display can show
HDR range (a potential headroom above 1). Where the backend cannot (`gpu::Device::supportsComposition`), the output
stays SDR with one warning naming the driver.

- **Headroom:** two values per display. The current headroom is what the display shows now: values above it are
  clipped, and it changes with the brightness and while HDR content is on screen. The potential headroom is the most
  it can show. The output's headroom, the peak of the HDR tone curve, is the current one, following Apple's
  guidance ("Performing your own tone mapping", Metal documentation: the upper bound "may vary from moment to
  moment"; read the current maximum while processing a frame and map all values between 0 and it); `auto` decides
  by the potential one. On Apple platforms the app reads both every frame from the window's screen
  (`src/app/DisplayHeadroomApple.m`: iOS 16+ `UIScreen.currentEDRHeadroom` / `potentialEDRHeadroom`, macOS
  `NSScreen.maximumExtendedDynamicRangeColorComponentValue` /
  `maximumPotentialExtendedDynamicRangeColorComponentValue`), because SDL's `SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT` is
  re-read only on `SDL_EVENT_WINDOW_HDR_STATE_CHANGED` and `SDL_EVENT_WINDOW_DISPLAY_CHANGED`, and on iOS SDL 3.4.18
  reads it once, when it adds the display (`SDL_uikitmodes.m`). Elsewhere SDL's property is used for both values, on
  those events. `--hdr-headroom N` replaces the current headroom in the output. A change of the output's composition
  or paper white is logged at once: `app: display output: HDR extended linear (on), headroom 4.00, paper white 1.00,
  swapchain Rgba16Float; display headroom 1.00 (potential 1.00), HDR supported`; a change of the headroom alone
  (a brightness ramp changes it over many frames) at most once a second, with the value it settles on. On the
  iPhone 18 Pro Max the demo starts HDR at `display headroom 1.20 (potential 8.00)`, and 1.7 s later iOS has raised
  it: `app: display headroom 3.16 (potential 8.00); output headroom 3.16`.
- **The frame:** in HDR the game renders into an offscreen `Rgba16Float` frame (`Frame::format`), ImGui is drawn into
  an 8-bit layer, and one full-screen pass composites the layer over the frame into the swapchain
  (`src/app/UiComposite.*`): the UI is blended on display values as in SDR and shown at paper white, and frame values
  above paper white stay, dimmed under the UI by its coverage. The SDR path is unchanged.
- **Captures** stay SDR PNGs: an HDR frame is divided by paper white, clipped at 1 and encoded with gamma 2.2, so the
  PNG shows what an SDR display would show of the HDR signal (everything above paper white is white). The capture log
  line and the scenario value `capture_peak` give the brightest value before clipping, in multiples of paper white.
  With `--hdr on --hdr-headroom 1` the capture matches the SDR one (renderer test: at most 2 levels apart), so golden
  images stay SDR; the demo's `demo_hdr_on_*` tests check the peak of a headroom-4 frame. What HDR looks like above
  paper white can only be judged on an HDR display, by eye.
- **Per platform** (what was run: [Platforms](platforms.md)): Metal on macOS and iOS supports extended linear and, on
  macOS only, `Hdr10` (PQ); SDL's Vulkan backend needs `VK_EXT_swapchain_colorspace`. The renderer writes no PQ, so
  `Hdr10` output gets SDR values ([Render module](render.md#tone-mapping-and-hdr-output)).

## Captures and golden images

A capture renders the frame into an offscreen texture (or uses the `--frame-size` target), draws ImGui, downloads it
and writes an RGBA PNG with opaque alpha (HDR frames: [HDR output](#hdr-output)). Golden images live in `tests/golden/<backend>/<device>/<name>.png`, where
`<device>` is the Mac's CPU brand in lower case with dashes (`apple-m4-pro`; CMake cache `QUENCH_GOLDEN_DEVICE`).

The apps' checks run in **batches** (`tests/CMakeLists.txt`, `quench_add_batch`): one offscreen app process per GPU
backend runs a scenario that `include`s every check as a part, with a fresh `--user-dir` that collects the parts'
captures (`build/<preset>/tests/captures/<batch>/`, the log next to it as `<batch>.log`). The batch is a CTest fixture
(`<app>_batch_<backend>`, `tests/RunBatch.cmake`: it fails when the app crashes or the log has no result line), and
each part is a test of its own (`<app>_<part>_<backend>`, `tests/CheckPart.cmake`): the part's checks passed, and its
capture matches the golden image. Running one part's test runs its batch first. For hello, `hello_batch_metal`,
`hello_batch_metal_native` and `hello_batch_vulkan` run `examples/hello/scenarios/check.txt` (`hello --window 640x360
--frame-size 1280x720 --gpu-debug`): `hello_frame_*` compares the frame at 1 s with `hello.png`, `hello_input_*` runs
the scripted input. The demo's batches: [Engine demo](demo.md#checks). Batches are registered where the captures are
compared: `QUENCH_TEST_CAPTURES`, on in Release builds; other builds run the demo's smoke run only.

Captures are compared with `quench_image_compare` (`tools/imagecompare`): a pixel differs when a channel differs by
more than 16; the test fails when the mean difference exceeds 0.5 or more than 0.1% of the pixels differ. On failure
`<name>_diff.png` (differences amplified four times) is left next to the capture. Without a golden image for this
device the test is skipped.

Updating a golden image: run the test, look at the capture (never commit one unseen), copy it to `tests/golden/...`,
and say in the devlog why it changed. Two runs of the same binary on Metal are identical (0 pixels differ); Metal and
Vulkan differ in the driver name the sample prints, so each backend has its own image.

The native Metal backend's batches (`*_metal_native`) compare with the same `metal` golden images as SDL GPU's Metal
driver: the native backend renders the same pixels (in Release identical, 2026-11-06), so it has no images of its
own, except where ray tracing runs (`demo_raytracing_native.png`).

## Building games: `quench_add_game()`

```cmake
quench_add_game(<target> NAME <display name> SOURCES <file>...
                [OUTPUT_NAME <file name>] [BUNDLE_ID <reverse dns>] [VERSION <x.y.z>]
                [ASSETS <dir>] [COOKED_ASSETS <quench_cook_assets name>...]
                [ICON <square png, 1024 px or larger>] [ORIENTATIONS landscape|portrait|all] [CUSTOM_MAIN])
quench_add_shaders(<target> NAME <set> SOURCE <file.hlsl> ENTRIES <entry>:vertex <entry>:fragment <entry>:compute ...)
quench_cook_assets(<name> SOURCE_DIR <dir> [TEXTURES <file>...] [MODELS <file>...] [SOUNDS <file>...]
                   [SKELETONS <file>...] [ANIMATIONS <file>...] [SKELETON <file>] [RETARGET <file>] ...)
```

`ASSETS` is copied as it is; `COOKED_ASSETS` names `quench_cook_assets()` targets, whose output (KTX2 textures and
`.qmodel` models from glTF, GLB, PNG, JPEG and KTX2 sources; `.qskel` skeletons and `.qanim` clip sets from glTF
and GLB; WAV, FLAC and MP3 sounds, checked and copied) is merged
into the same `assets/`. The cook runs at build
time, per file and only when a source changed ([Jobs, assets and the cook](assets.md#quench_cook)).

- **macOS**: a plain executable; `ASSETS` and the cooked assets are copied to `assets/` next to it. Run it from the
  build tree.
- **iOS with Ninja** (`ios-sim-release`, `ios-device-release`): `<OUTPUT_NAME>.app` with an Info.plist from
  `cmake/Info.ios.plist.in`, the icon compiled by `actool`, the assets at `assets/` in the bundle, signed with
  `QUENCH_IOS_SIGN_IDENTITY` (`-`, ad-hoc: enough for the Simulator).
- **iOS with Xcode** (`ios-xcode`): an Xcode project with Automatic signing (`QUENCH_IOS_TEAM`); bundle ids get
  `QUENCH_IOS_BUNDLE_ID_SUFFIX` (default `.<login name>`), because bundle ids are global across Apple accounts and a
  free team cannot register one another account uses. A paid team signs any id with its wildcard profile, so
  `-DQUENCH_IOS_BUNDLE_ID_SUFFIX=` (empty) gives the plain ids (`org.quench.hello`) ([iOS runs](#ios-runs)).
- Info.plist: SDL's scene delegate, game controller keys, `CADisableMinimumFrameDurationOnPhone` (without it iOS caps
  phones at 60 Hz; with it the iPhone 18 Pro Max runs hello at 120 Hz). `ORIENTATIONS all` allows all four
  orientations on iPad; a fixed orientation sets `UIRequiresFullScreen`, deprecated since iOS 26 but still the only
  way to keep one orientation on an iPad.
- `CUSTOM_MAIN`: the target defines `main` itself (the device test app).

New games start from `templates/game`: `python3 tools/new_game.py "My Game" <directory>` copies it, renames it, pins
the engine to a commit of this checkout and checks that the game builds and starts (see the script's `--help`). It
also copies the agent skill `.agents/skills/quench-game/` into the game: how to build a game on the engine (the
modules, cooked assets, scenarios, captures and golden images, iOS builds, pitfalls), for coding agents in the
game's repository.

## Shader hot reload

With `--hot-reload` on a desktop, the app calls `gpu::Device::enableShaderHotReload()` and polls it every frame:
an edit of an HLSL source, or of a file it includes, is compiled again while the game runs, and the pipelines that
use the set are created again behind the same handles, the engine's (the renderer's sets) and the game's alike.
Nothing changes in the game's code.

- **The manifest.** `quench_add_shaders()` writes `quench_shaders/<set>/manifest.json` in the build tree: the source,
  the build's depfiles (the included files) and the build's own compile commands, with `{source}` and `{output}` for
  the paths. Each embedded blob names it relative to the build directory (`ShaderBlob::manifest`), so the binary holds
  no absolute paths. The app looks for the build directory (`CMakeCache.txt`) at or above the executable's
  directory. A packaged app has none and logs "no shader hot reload". Cross builds (iOS) and multi-configuration
  generators write no manifests.
- **Watching and compiling.** Every 0.5 s the times of the watched files are compared. A changed set is compiled
  entry by entry (up to 4 processes at a time, through `SDL_CreateProcess`): `quench_shaderc` for SPIR-V, MSL and
  the reflection, then `xcrun metal` and `metallib` where the device uses metallibs. The frame never waits for them.
  The outputs go to `quench_shaders/<set>/reload/`, not over the build's.
- **Swapping.** When every entry of the set compiled, its blobs are replaced, and every live pipeline that uses one
  of them is created again. Pipelines created afterwards get the new code too. The log says "'<set>' reloaded,
  N pipelines created again".
- **Errors.** A failed compile logs the compiler's first lines ("the old code of '<set>' stays: ...") and keeps
  every pipeline as it was. `ShaderReloadStatus::lastError` has the command and its whole output. A pipeline that
  cannot be created from the new code keeps its old one (logged).
- **Limits.** Edits that change a shader's resources (a new texture or uniform block) need the C++ side that binds
  them, so a rebuild. New entries or sets need a configure. The engine's own sets reload like a game's: an edit of
  `shaders/quench/Brdf.hlsli` recompiles every set that includes it, one set after another. Compiling the
  renderer's `render_frame` (15 entries) took 3.7 to 4.2 s on the M4 Pro.

Checked by `gpu_device.shader_hot_reload` on the Mac's three backends (SDL GPU on Metal, Vulkan through MoltenVK,
the native Metal backend). It edits copies of a test set's source and include: red, then blue after the source
edit, green after the include edit, still green after a syntax error. The scenario values are `shader_reloads`,
`shader_reload_failures` and `shader_reload_pipelines`.

## iOS runs

Simulator (host paths work there, so `--capture` and `--script` take Mac paths):

```sh
cmake --preset host-tools && cmake --build --preset host-tools
cmake --preset ios-sim-release && cmake --build --preset ios-sim-release
xcrun simctl install booted build/ios-sim-release/examples/hello/hello.app
xcrun simctl launch --console --terminate-running-process booted org.quench.hello --capture /tmp/hello.png --quit-after 3.5
```

Device: an Apple ID signed in to Xcode (Settings, Accounts) with a team. A paid Apple Developer Program team is
recommended; the engine's device runs use one since 2026-11-05 (an Individual team). Xcode's Automatic signing then
uses the team's wildcard profile ("iOS Team Provisioning Profile: *", valid for a year), so no App ID has to be
registered. A free Personal Team also works, with two limits: its profiles last 7 days, and at most three apps with
a free developer profile can be on a device at a time.

```sh
cmake --preset ios-xcode -DQUENCH_IOS_TEAM=<team id> -DQUENCH_IOS_BUNDLE_ID_SUFFIX=  # empty suffix: paid team only
xcodebuild -project build/ios-xcode/Quench.xcodeproj -scheme quench_hello -configuration Release \
    -destination id=<device udid> -allowProvisioningUpdates -allowProvisioningDeviceRegistration build
DEVICECTL=/Library/Developer/PrivateFrameworks/CoreDevice.framework/Versions/A/Resources/bin/devicectl
$DEVICECTL device install app --device <udid> build/ios-xcode/examples/hello/Release-iphoneos/hello.app
$DEVICECTL device process launch --device <udid> --console --terminate-existing \
    --environment-variables '{"MTL_DEBUG_LAYER":"1"}' org.quench.hello --gpu-debug --capture hello.png
$DEVICECTL device copy from --device <udid> --domain-type appDataContainer --domain-identifier org.quench.hello \
    --source "Library/Application Support/Quench/Quench Hello/hello.png" --destination /tmp/hello.png
```

In the Xcode build a game's plain and cooked assets are merged into one folder (`<target>_assets`,
`cmake/StageAssets.cmake`), and a script phase after the app target's other phases (`CMake PostBuild Rules`, on
every build) copies the files that changed into the bundle's `assets/`, at the path Xcode passes to the phase
(`TARGET_BUILD_DIR`), and signs the bundle again when it copied something, with Xcode's identity and entitlements
(Xcode signs only when a file it copied or linked changed, so changed assets alone broke the signature: "a sealed
resource is missing or invalid"). Until 2026-11-07 the merged folder was a folder reference that Xcode copied
itself, and Xcode looks at such a folder before the build that changes it: on 2026-11-07 the demo's textures,
cooked again as ASTC, were in the merged folder after a build but reached the bundle only in the next one (Xcode
27.2, CMake 4.4.4). Checked on 2026-11-07 with the script phase: a texture source changed and changed back reached
the bundle as the cooked file in the build that cooked it, twice, with a valid signature
(`codesign --verify --deep --strict`) and the entitlements in it. A build in which CMake regenerates the project (a
changed `CMakeLists.txt`) still runs the old commands, so its app can be stale (a shader entry added in CMake was
missing from it on 2026-11-06, and the app crashed at its first pipeline); the next build runs the new ones. After a
CMake change, run `cmake --preset ios-xcode` before `xcodebuild` (`tools/package_demo.sh` does) or build twice.
`tools/package_demo.sh` compares the bundle's `assets/` with the merged folder after its build.

The bundle ids are the `BUNDLE_ID` of each target plus `QUENCH_IOS_BUNDLE_ID_SUFFIX`: with the default suffix,
`org.quench.hello.<login name>`. Team ids are listed in Xcode's accounts, or with
`defaults read com.apple.dt.Xcode IDEProvisioningTeamByIdentifier` (`isFreeProvisioningTeam = 1` marks a Personal
Team). `codesign -dvv <app>` shows the team an app was signed by (`TeamIdentifier`). Another suffix means other
bundle ids: the apps installed under the old ids stay on the phone until they are uninstalled.

CoreDevice's own `devicectl` is used because `xcrun devicectl` hung in `xcodebuild -runFirstLaunch` on the
development Mac (seen on 2026-11-02). With a free Personal Team a fourth install fails with `CoreDeviceError 3002`
("This device has reached the maximum number of installed apps using a free developer profile", listing the
installed ones), and a launch of the app that was not installed then fails with `10002` ("is not installed").
`10002` also comes from a locked phone ("Unable to launch ... because the device was not, or could not be,
unlocked"): the error's reason tells them apart. Uninstall one of the engine's own apps first
(`devicectl device uninstall app --device <udid> <bundle id>`; that deletes its data container); never a game that
holds the user's data. Scripts go the other way with `device copy to` into the same directory, then
`--script <relative path>`.

**Device tests**: in the `ios-xcode` preset the tests build as one app, `quench_device_tests` (`QuenchTests.app`):
every case that does not need the build tree (core, physics, audio and the audio device, app, GPU device), run in one
process as `SDL_main`, so windows work. The audio device cases there are the null backend and the audio session
category; the one that plays through the speaker runs only with `QUENCH_TEST_AUDIO_DEVICE=1` in the environment
(not run on the iPhone yet). Launch it with `--console` and the environment `{"MTL_DEBUG_LAYER":"1","QUENCH_TEST_GPU_DRIVER":"metal"}`;
the last line is `N cases: P passed, F failed, S skipped` and the exit code is 0 only without failures. Agents cannot
see the device screen: what is visible on it (rendering, touch, rotation) is checked by the user and recorded as a
user report in the devlog.
