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
for an argument it does not know (the process then exits with 1). `quench_add_game()` defines `QUENCH_GAME_NAME` and
`QUENCH_GAME_VERSION` for the game's sources.

`Game` (`include/quench/app/App.h`) has `init`, `fixedStep`, `update`, `ui`, `render`, `event`, `shutdown`, and
`scenarioValue` / `scenarioCommand` for scripts. Per frame:

1. SDL events: ImGui, then `Input`, then `Game::event`.
2. Input actions are updated; a running scenario executes its commands.
3. Fixed simulation ticks (`AppDesc::fixedStep`, default 1/120 s): none, one or several `fixedStep` calls.
   `App::alpha()` is the fraction of a tick since the last one, for interpolation in `render`.
4. `update` once, `ui` (ImGui windows), then `render` records the game's passes into `Frame::commands` and
   `Frame::target`; the engine draws ImGui on top and presents.

On iOS the app stops rendering between `SDL_EVENT_WILL_ENTER_BACKGROUND` and `SDL_EVENT_DID_ENTER_FOREGROUND`, and the
clock is reset on return, so no ticks pile up. A minimized desktop window also skips rendering.

`App` gives the device, the window, `Input`, sizes in points and pixels, the safe area (notches, rounded corners),
time, frame and tick counters, frame statistics (`FrameStats`: frame time, CPU time until the submit without the wait
for a swapchain texture, GPU time where `gpu::Caps::gpuTiming`), and actions: `requestCapture`, `setPresentMode`,
text input, `quit(exitCode)`.

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
  `mouseFromTouch()` tells them apart.
- Key names are SDL scancode names (`"W"`, `"Space"`, `"Left Ctrl"`); gamepad buttons follow SDL's order (South is A
  on Xbox, Cross on PlayStation).

## ImGui

Dear ImGui 1.92.9b with the engine's own renderer on `quench::gpu` (`src/app/ImGuiRenderer.cpp`): dynamic font
textures (`ImGuiBackendFlags_RendererHasTextures`), any swapchain format (sRGB-aware), clip rectangles in pixels. The
font and the style scale with the display's content scale, 1.5 times larger on iOS and Android. `imgui.ini` is not
written. `quench::app::imguiTexture(texture)` gives an id for `ImGui::Image`.

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
| `--gpu-driver NAME` | `metal`, `vulkan` or `direct3d12` |
| `--gpu-debug` | GPU validation (the backend's debug mode) |
| `--present-mode MODE` | `vsync`, `immediate` or `mailbox`; falls back to vsync where the window does not support it |
| `--version` | print the version and exit |

With `--script` or `--capture` the run is **deterministic**: every frame advances the app time by exactly one fixed
step and runs one tick, whatever the display rate. Frames still come as fast as the present mode allows, so app time
runs slower or faster than real time. Games should hide timings in deterministic runs (`App::deterministic()`), so
captures compare across runs.

The user directory is `SDL_GetPrefPath(organization, name)`: `~/Library/Application Support/<organization>/<name>/` on
macOS, `Library/Application Support/<organization>/<name>/` in the app's data container on iOS.

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
| `quit` | end the scenario; the app exits with 1 if a check failed |
| `down KEY`, `up KEY`, `tap KEY` | keys by SDL scancode name; a tap releases on the next frame |
| `text STRING` | text input |
| `mouse X Y`, `click X Y [left\|right\|middle]`, `wheel DY` | mouse in window points |
| `touch X Y [DX DY STEPS]` | a tap, or a finger dragged by DX, DY over STEPS frames |
| `gamepad attach\|detach` | a virtual gamepad (SDL's virtual joystick) |
| `gamepad button NAME down\|up\|tap`, `gamepad axis NAME VALUE` | SDL names: `a`, `b`, `x`, `y`, `start`, `dpup`, ...; `leftx`, `lefty`, `rightx`, `righty` (-1..1), `lefttrigger`, `righttrigger` (0..1) |
| `resize W H` | resize the window (points) |
| `capture PATH` | capture the next frame |
| `perf_begin LABEL`, `perf_end` | measure the frames in between; `perf_end` logs a `perf:` line and sets the `perf_*` values |

Values: `time`, `frame`, `ticks`, `fps`, `frame_ms`, `cpu_ms`, `gpu_ms`, `window_w`, `window_h`, `pixel_w`, `pixel_h`,
`gamepad` (1 when connected), `fingers`, `action:NAME` (an action's value), after `perf_end` `perf_frames`,
`perf_frame_avg`, `perf_frame_p50`, `perf_frame_p95`, `perf_frame_p99`, `perf_frame_max`, `perf_cpu_avg`,
`perf_cpu_p50`, `perf_cpu_p95`, `perf_cpu_p99`, `perf_gpu_avg`, `perf_gpu_p50`, `perf_gpu_p95` (milliseconds; -1 when
there were no samples, for example GPU time without `gpu::Caps::gpuTiming`); then the game's (`Game::scenarioValue`). Unknown commands are offered
to `Game::scenarioCommand`; a command nobody knows fails. Each check logs `ok` or `FAIL line N: ...`, and the run ends
with `N passed, M failed`; tests match `FAIL line`.

## Captures and golden images

A capture renders the frame into an offscreen texture (or uses the `--frame-size` target), draws ImGui, downloads it
and writes an RGBA PNG with opaque alpha. Golden images live in `tests/golden/<backend>/<device>/<name>.png`, where
`<device>` is the Mac's CPU brand in lower case with dashes (`apple-m4-pro`; CMake cache `QUENCH_GOLDEN_DEVICE`).

The capture tests (`hello_metal`, `hello_vulkan`) run `hello --window 640x360 --frame-size 1280x720 --capture-at 1
--quit-after 1 --gpu-debug` and compare with `quench_image_compare` (`tools/imagecompare`): a pixel differs when a
channel differs by more than 16; the test fails when the mean difference exceeds 0.5 or more than 0.1% of the pixels
differ. On failure `<name>_diff.png` (differences amplified four times) is left next to the capture in
`build/<preset>/tests/captures/`. Without a golden image for this device the test is skipped.

Updating a golden image: run the test, look at the capture (never commit one unseen), copy it to `tests/golden/...`,
and say in the devlog why it changed. Two runs of the same binary on Metal are identical (0 pixels differ); Metal and
Vulkan differ in the driver name the sample prints, so each backend has its own image.

## Building games: `quench_add_game()`

```cmake
quench_add_game(<target> NAME <display name> SOURCES <file>...
                [OUTPUT_NAME <file name>] [BUNDLE_ID <reverse dns>] [VERSION <x.y.z>]
                [ASSETS <dir>] [ICON <square png, 1024 px or larger>] [ORIENTATIONS landscape|portrait|all]
                [CUSTOM_MAIN])
quench_add_shaders(<target> NAME <set> SOURCE <file.hlsl> ENTRIES <entry>:vertex <entry>:fragment ...)
```

- **macOS**: a plain executable; `ASSETS` is copied to `assets/` next to it. Run it from the build tree.
- **iOS with Ninja** (`ios-sim-release`, `ios-device-release`): `<OUTPUT_NAME>.app` with an Info.plist from
  `cmake/Info.ios.plist.in`, the icon compiled by `actool`, the assets at `assets/` in the bundle, signed with
  `QUENCH_IOS_SIGN_IDENTITY` (`-`, ad-hoc: enough for the Simulator).
- **iOS with Xcode** (`ios-xcode`): an Xcode project with Automatic signing (`QUENCH_IOS_TEAM`); bundle ids get
  `QUENCH_IOS_BUNDLE_ID_SUFFIX` (`.<login name>`), because bundle ids are global per Apple account and a free team
  cannot register one another account uses.
- Info.plist: SDL's scene delegate, game controller keys, `CADisableMinimumFrameDurationOnPhone` (without it iOS caps
  phones at 60 Hz; with it the iPhone 18 Pro Max runs hello at 120 Hz). `ORIENTATIONS all` allows all four
  orientations on iPad; a fixed orientation sets `UIRequiresFullScreen`, deprecated since iOS 26 but still the only
  way to keep one orientation on an iPad.
- `CUSTOM_MAIN`: the target defines `main` itself (the device test app).

New games start from `templates/game`: `python3 tools/new_game.py "My Game" <directory>` copies it, renames it, pins
the engine to a commit of this checkout and checks that the game builds and starts (see the script's `--help`).

## iOS runs

Simulator (host paths work there, so `--capture` and `--script` take Mac paths):

```sh
cmake --preset host-tools && cmake --build --preset host-tools
cmake --preset ios-sim-release && cmake --build --preset ios-sim-release
xcrun simctl install booted build/ios-sim-release/examples/hello/hello.app
xcrun simctl launch --console --terminate-running-process booted org.quench.hello --capture /tmp/hello.png --quit-after 3.5
```

Device (an Apple ID signed in to Xcode; a free Personal Team works, its profiles last 7 days):

```sh
cmake --preset ios-xcode -DQUENCH_IOS_TEAM=<team id>
xcodebuild -project build/ios-xcode/Quench.xcodeproj -scheme quench_hello -configuration Release \
    -destination id=<device udid> -allowProvisioningUpdates build
DEVICECTL=/Library/Developer/PrivateFrameworks/CoreDevice.framework/Versions/A/Resources/bin/devicectl
$DEVICECTL device install app --device <udid> build/ios-xcode/examples/hello/Release-iphoneos/hello.app
$DEVICECTL device process launch --device <udid> --console --terminate-existing \
    --environment-variables '{"MTL_DEBUG_LAYER":"1"}' org.quench.hello.<login name> --gpu-debug --capture hello.png
$DEVICECTL device copy from --device <udid> --domain-type appDataContainer \
    --domain-identifier org.quench.hello.<login name> \
    --source "Library/Application Support/Quench/Quench Hello/hello.png" --destination /tmp/hello.png
```

CoreDevice's own `devicectl` is used because `xcrun devicectl` hung in `xcodebuild -runFirstLaunch` on the
development Mac (seen on 2026-11-02). Scripts go the other way with `device copy to` into the same directory, then
`--script <relative path>`.

**Device tests**: in the `ios-xcode` preset the tests build as one app, `quench_device_tests` (`QuenchTests.app`):
every case that does not need the build tree (core, physics, app, GPU device), run in one process as `SDL_main`, so
windows work. Launch it with `--console` and the environment `{"MTL_DEBUG_LAYER":"1","QUENCH_TEST_GPU_DRIVER":"metal"}`;
the last line is `N cases: P passed, F failed, S skipped` and the exit code is 0 only without failures. Agents cannot
see the device screen: what is visible on it (rendering, touch, rotation) is checked by the user and recorded as a
user report in the devlog.
