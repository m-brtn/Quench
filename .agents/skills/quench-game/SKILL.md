---
name: quench-game
description: >-
  Builds, extends and verifies a game on the Quench C++20 engine: tools/new_game.py and templates/game, the pinned
  engine (FetchContent and the local override), the quench::app::Game interface and fixed-step loop,
  quench_add_game, input actions, ImGui, the retained render::Scene, cooked assets (quench_cook_assets,
  SceneAssets), weather, clouds, rain, snow, wind and lightning, physics, audio, animation, navigation, jobs,
  quench::fs, iOS builds, scripted offscreen checks with captures and golden images, and Tracy profiles recorded without its GUI. Use when creating a
  game on Quench, working in a repository whose CMakeLists.txt fetches Quench or calls quench_add_game, or when the
  user asks how to make a game with the engine.
---

# Building a game on Quench

Quench owns `main` and the frame loop. A game is one `quench::app::Game` in an executable built with
`quench_add_game()`. This skill takes a game from nothing to a running, checked build. Working code for every module
is in [reference.md](reference.md).

## Where the engine is

- In the engine repository: its root. In a game: the commit pinned by `GIT_TAG` in the game's `CMakeLists.txt`;
  after the first configure its source is `build/<preset>/_deps/quench-src/`, or the checkout named by
  `-DFETCHCONTENT_SOURCE_DIR_QUENCH`.
- Paths below (`docs/`, `include/quench/`, `cmake/`, `tools/`, `templates/`, `examples/`) are in that engine source.
  The headers are the truth for names and signatures: read the header before using a call.
- Depth: `docs/app.md` (game interface, flags, scenarios, captures, `quench_add_game()`, iOS runs), `docs/render.md`,
  `docs/assets.md`, `docs/audio.md`, `docs/animation.md`, `docs/navigation.md`, `docs/weather.md`,
  `docs/performance.md`, `docs/platforms.md`. Working code: `templates/game/src/Game.cpp` and
  `examples/hello/Hello.cpp` (drawing with `quench::gpu`), `examples/demo/sections/` (renderer, physics, audio,
  animation, assets, navigation, weather).

## Workflow

```
- [ ] 1. Create the game, build it, run its smoke check
- [ ] 2. Shape the Game: simulation in fixedStep, drawing in render
- [ ] 3. Add the modules it needs (render, assets, weather, physics, audio, animation, navigation)
- [ ] 4. A scenario with checks and a capture for every feature, run by ctest
- [ ] 5. Gates: debug and release builds, ctest, captures looked at, devlog
```

### 1. Create the game

From the engine checkout:

```sh
python3 tools/new_game.py "My Game" ../my-game   # also --id --bundle-id --organization --engine-commit --no-check
cd ../my-game && git init
```

`tools/new_game.py` copies `templates/game`, replaces its names, pins `GIT_TAG` to the engine's HEAD (or
`--engine-commit`), copies this skill into `.agents/skills/quench-game/`, then configures and builds `debug` against
the checkout and starts the game once with `--quit-after 1` (offscreen). A pin that is not on a remote branch builds
only with the local override; the script says so.

| Path in the game | What |
|---|---|
| `CMakeLists.txt` | FetchContent of the engine at `GIT_TAG`, `quench_add_game()`, `quench_add_shaders()`, the `smoke` test |
| `CMakePresets.json` | `debug`, `release`, `host-tools`, `ios-sim-release`, `ios-xcode` |
| `src/Game.cpp` | `createGame()` and the `Game`: a cube on a grid drawn with `quench::gpu`, input actions, ImGui, settings |
| `shaders/` | HLSL; `GameUniforms.h` holds the uniform structs shared with C++ |
| `assets/` | files copied as they are (`settings.json`), read through `quench::fs` |
| `scenarios/smoke.txt` | the scripted check `ctest` runs |
| `AGENTS.md`, `devlog/` | the game's agent rules and development log |

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
build/debug/my_game --capture /tmp/my_game.png --capture-at 1 --quit-after 1   # offscreen; then look at the PNG
build/debug/my_game --help                                                     # every engine flag
```

Engine and game changed together: configure with `-DFETCHCONTENT_SOURCE_DIR_QUENCH=<engine checkout>`. The value
stays in the build directory's cache. Before committing the game, pin a pushed engine commit and build in a fresh
build directory without the override.

### 2. The game interface

`include/quench/app/App.h`; `docs/app.md`, "Game interface".

- `quench::app::createGame(AppDesc& desc, std::span<const std::string_view> args)` runs before SDL and the GPU
  exist. Fill `desc` (`name = QUENCH_GAME_NAME`, `organization`, `version = QUENCH_GAME_VERSION`, `fixedStep`,
  `audio`, `hdr`), handle the game's own flags (print them when `--help` comes), return an `Error` for unknown ones.
  Create nothing on the GPU there or in the `Game` constructor: keep such members in `std::optional` or
  `std::unique_ptr` and create them in `init`.
- `Game::init(App&)` creates the game's objects; an `Error` ends the app with a failure exit code.
- Per frame: events, input, scenario, `fixedStep` zero or more times (`AppDesc::fixedStep`, 1/120 s by default),
  `update` once (after the app updated `app.assets()`), the audio update, `ui` (ImGui), `render` into
  `Frame::target`, then ImGui on top and present.
- Simulate only in `fixedStep(app, dt)`. Keep the previous and the current state of what moves and interpolate with
  `app.alpha()` when drawing. Read one-frame input (`pressed`, `released`) in `update`, not in `fixedStep`: a frame
  runs zero or several ticks.
- `shutdown` releases GPU resources at the latest. The game's destructor runs after it, before the engine's audio,
  assets, jobs and device go. Members are destroyed in reverse order of declaration.
- Scripts reach the game through `scenarioValue` (names for `expect` and `until`), `scenarioCommand`,
  `scenarioPoint` (`@NAME` points) and `perfDetails`.
- `App` (main thread only): `device()`, `jobs()`, `assets()`, `audio()`, `input()`, `time()`, `alpha()`,
  `deterministic()`, `scripted()`, `stats()`, `safeArea()`, `requestCapture()`, `beginPerf()`, `endPerf()`,
  `setMaxFrameRate()`, `thermalGovernor()`, `frameRateLimit()`, `mobile()`, `quit()`.

### 3. Modules

Link what the game uses beside `quench_add_game()`:
`target_link_libraries(my_game PRIVATE quench_render quench_physics quench_animation)`. `quench_main` already brings
`quench_app`, `quench_assets`, `quench_audio`, `quench_gpu`, ImGui and GLM.

- **Drawing.** The template draws with `quench::gpu` (its own pipeline; HLSL through `quench_add_shaders()`, uniform
  structs in `shaders/*.h` with 16-byte members only). For lit 3D use `quench_render`: `render::Renderer::create`,
  a retained `render::Scene` (meshes, materials, instances, lights and the environment registered once by handle;
  per frame only what moved), and `Renderer::render(scene, view, frame.commands, frame.target, frame.output)` in
  `Game::render`. Effects are `RenderSettings`; the renderer falls back by `gpu::Caps` and `effects()` and
  `fallbacks()` say what runs, so never choose effects by platform. The default backend is the native Metal one on
  Apple platforms (SDL GPU elsewhere, and with `--gpu-backend sdl`); the default effects (shadow maps, nothing
  traced) run on both. Ray-traced effects need a `RenderSettings` choice on the native backend and a GPU with ray
  tracing. Own shading: surface shaders
  (`Renderer::addSurface`, `docs/render.md`, "Surface shaders"; in ray hits too with `quench_add_ray_hits()` and
  `SurfaceDesc::rayHitShaders`, [reference.md](reference.md), "Own surface shading") and game passes
  (`Renderer::addPass`).
- **Assets.** Sources are cooked at build time by `quench_cook_assets()` (`cmake/QuenchCook.cmake`: `TEXTURES`,
  `DATA_TEXTURES`, `MODELS`, `SOUNDS`, `SKELETONS`, `ANIMATIONS`, `NAVMESHES`, `FILES`) and merged into the game's
  assets by `quench_add_game(... COOKED_ASSETS <name>)`; `ASSETS <dir>` copies files as they are. The runtime path is
  the path under `SOURCE_DIR` with the cooked extension: `models/crate.gltf` becomes `models/crate.qmodel`, textures
  `.ktx2`, navmeshes `.qnav`, sounds keep theirs. Load through `render::SceneAssets` (textures, models),
  `app.audio()` (sounds), `animation::AnimationSystem` or `navigation::NavMeshAsset`: loads run on jobs and show
  placeholders until ready (`AssetRef::ready()`); never wait for them. `--hot-reload` loads changed cooked files
  again. Textures are BC7 on the Mac and ASTC 4x4 in iOS builds by default (`TEXTURE_FORMAT auto`);
  `TEXTURE_FORMAT basis` cooks one Basis Universal set for every device, transcoded at load time (smaller with
  `BASIS_CODEC etc1s`, lower quality; slower to cook). `docs/assets.md`, "Basis Universal".
- **Weather and clouds.** `weather::Weather` (`include/quench/weather/Weather.h`, headless, linked with
  `quench_render`): `Settings` (coverage, cloud type, wind, evolution, precipitation, cirrus), `Cell`s a game places
  (a single raining cloud), `update(dt)` in `Game::update`, then `scene.setWeather(weather)` every frame (a copy).
  Clouds are off by default: `RenderSettings::clouds = render::Clouds::Volumetric` (or `Layer`, the cheap 2D layer
  and the fallback), with `cloudResolution`, `cloudSteps`, `cloudShadows` and the sky cube for reflections. The game
  asks the weather what is above a point: `precipitation`, `distanceToPrecipitation`, `cover` (`coverAbove`: only the
  clouds above it), `windAt`. A world above the clouds (a sea of clouds below the camera): cloud bases are at y >= 0,
  so put the ground higher (bases at 700 m, the ground at 2.2 km), and turn `cloudShadows` off. Rain is off
  by default too: `RenderSettings::rain = render::Rain::Medium` (`Low` on phones) draws streaks, splashes, wet
  surfaces with puddles (`MaterialDesc::puddles`, `water`; `wetness = 0` or `QUENCH_SURFACE_NO_RAIN` keeps one dry)
  and, with clouds, shafts from afar; call `weather.setWetnessFocus(...)` with the camera's ground point. Its sound:
  `audio::WeatherAudio` fed each frame from the weather and `stats().rainCover`, the defaults cooked by
  `quench_weather_sounds(<name>)` and listed in `COOKED_ASSETS`, or a game's own (`setSound`, `setEnabled(false)`).
  Snow: `Settings::temperature` (15 degrees C by default: no snow) below freezing and `RenderSettings::snow =
  render::Snow::Medium` (`Low` on phones) draw flakes and a snow cover on surfaces that face up
  (`MaterialDesc::snow = 0` or `QUENCH_SURFACE_NO_SNOW` keeps one bare); give the sound `.snow =
  weather.snowShare(listener)`. Wind: `Wind::gusts` and `turbulence` (0 by default) move the drops and flakes;
  plants sway through a surface shader's `QUENCH_SURFACE_VERTEX` calling `quenchWindSway` (`quench/Wind.hlsli`);
  `quench::WindDrag` pushes the physics bodies a game adds; `.wind = glm::length(weather.windAt(listener))` gives
  the sound its wind. Lightning: `RenderSettings::lightning = true` lights the storm, and each flash from
  `weather.strikes(...)` passed once to `WeatherAudio::strike(distance)` thunders later.
  [reference.md](reference.md), "Weather and clouds"; `docs/weather.md`.
- **Physics.** `quench::PhysicsWorld` on `&app.jobs()` (Jolt, its jobs on the engine's pool); bodies through
  `bodies()` with `PhysicsLayers::kStatic` or `kMoving`; `step(float(dt))` in `fixedStep`.
- **Audio.** `app.audio()`: `load`, `play` with `PlayParams` (buses `"music"`, `"effects"`, `"ui"` by default;
  `spatial` with an `Emitter`), `setListener`, `playMusic`. The app calls its `update()`. `docs/audio.md`.
- **Animation.** The game owns an `animation::AnimationSystem` on `app.assets()` and `app.jobs()` and calls its
  `update(dt)` in `Game::update`; skinned models come through `SceneAssets`. `docs/animation.md`, "Use".
- **Navigation.** Link `quench_navigation`. `navigation::NavMesh::build` on `app.jobs()` from the level's triangles
  (or a cooked `.qnav` through `navigation::NavMeshAsset`), `update()` every frame (it never waits; tiles appear as
  their jobs finish), `navigation::Crowd` stepped in `fixedStep`, `navigation::NavQuery` for paths. The mesh
  outlives its crowds and queries (declare it first). `docs/navigation.md`, "Use".
- **Jobs.** `app.jobs()`: `run`, `parallelFor`; poll `Job::done()` across frames for long work, `wait` only for work
  the frame needs now. Jobs must not throw. `docs/assets.md`, "Job system".
- **Input.** `app.input().addAction(name, {bindings})` with `Binding::key("Space")` (returns an optional:
  dereference it), `Binding::gamepad`, `Binding::gamepadAxis`, `Binding::mouse`; read `value`, `down`, `pressed`,
  `released`, `axis(negative, positive)`; `fingers()` for touch. Key names are SDL scancode names.
  `docs/app.md`, "Input".
- **ImGui.** Windows in `Game::ui` (`#include <imgui.h>`), placed inside `app.safeArea()`. Hide timings when
  `app.deterministic()`, so captures compare.
- **Files.** `quench::fs` (`include/quench/core/FileSystem.h`): `readAsset`, `readAssetText` for the game's
  assets; `readUser`, `readUserText`, `writeUser`, `writeUserText` for settings and saves in the user directory
  (`SDL_GetPrefPath(organization, name)`; `--user-dir DIR` replaces it). In scripted runs (`app.scripted()`) start
  from defaults and leave the user's files alone.
- **Logging.** `quench::log::info("game", "...", args...)`, also `error`, `warning`, `debug`; `log::Throttle` for
  messages that could repeat every frame.
- **Heat (phones).** The engine reports the thermal state (`stats().thermal`) and changes nothing by itself. A game
  that wants to react enables `app.thermalGovernor()` (`ThermalGovernorDesc`: limits per state, the hold before
  they are lifted, an `onChange` callback), ideally behind a player setting (the template's `adapt_to_heat`, off).
  The App applies its frame rate limit on top of `setMaxFrameRate` (`frameRateLimit()` is the result); the game
  renders at `thermalGovernor().renderScale(playerScale)` and turns its own effects down while `reduceEffects()`.
  Never overwrite the player's settings with the limits. `docs/app.md`, "Thermal state".
- **Profiling.** `#include <quench/core/Profile.h>`: `QUENCH_PROFILE_ZONE("game.ai")` (a zone to the end of the
  scope, a literal name), `QUENCH_PROFILE_ZONE_DYNAMIC`, `QUENCH_PROFILE_ZONE_TEXT`, `QUENCH_PROFILE_PLOT`,
  `QUENCH_PROFILE_MESSAGE`, `QUENCH_PROFILE_THREAD_NAME`; a job's zone gets its name from
  `app.jobs().run(name, fn)`. They are nothing unless the engine is configured with `-DQUENCH_TRACY=ON`; the app
  already marks frames and times its loop, the renderer's passes and the engine's modules.
  [reference.md](reference.md), "Profiling".

### 4. Verify

`docs/app.md`, "Scenarios", "Offscreen runs", "Captures and golden images".

- **Offscreen.** Runs with `--script`, `--capture` or `--quit-after` show no window, take no input from real devices
  and play no sound (the mix runs offline). Never start a run with `--onscreen` or without those flags (it opens a
  window and plays sound) without asking the user.
- **Scenarios** (`--script FILE`): `wait`, `until`, `expect`, `mark` (then `d_NAME`), `down`, `up`, `tap`, `touch`,
  `click`, `mouse`, `gamepad`, `resize`, `capture`, `audio_measure`, `perf_begin`, `perf_end`, `max_frame_rate`,
  `thermal_state` (a fake thermal state, `system` ends it; also `--thermal-state NAME`), `include`, `quit`.
  Values: the game's (`scenarioValue`), `time`, `action:NAME`, `audio_played`, `audio_rms`, `gpu_resources`,
  `thermal_reaction`, `frame_rate_limit` and more.
  Each check logs `ok` or `FAIL line N`; the exit code is 1 when one failed. Scripted runs are deterministic: one
  fixed step per frame. Points in percent (`50% 50%`) or `@NAME` work at any window size.
- **ctest.** One test per scenario with `FAIL_REGULAR_EXPRESSION "FAIL line"`, as the template's `smoke`.
- **Captures.** `--capture FILE.png --capture-at S --quit-after S`, or `capture NAME.png` in a scenario (relative
  paths go to the user directory: give tests `--user-dir`). `--frame-size WxH` makes captures independent of the
  display density. Look at every new capture (open the PNG) before calling a feature done.
- **Golden images.** Compare a capture with a reference in the game's repository with the engine's image compare
  tool built in the game (CMake in [reference.md](reference.md), "Scenario checks, captures and golden images").
  References depend on the GPU backend and the device; change one only on purpose, after looking at both images.
- **Performance.** `perf_begin LABEL` .. `perf_end` in a scenario (real time; logs a `perf:` line and sets `perf_*`
  values) or `App::beginPerf` / `endPerf`. Measurements need `--onscreen`: ask the user first. `docs/performance.md`.
  Where the time goes: a Tracy capture of an offscreen scenario with the engine's `tools/tracy_capture.py` (zones per
  perf region in a CSV, no GUI; never start Tracy's GUI, it opens a window). `docs/performance.md`, "Profiling with
  Tracy".

### 5. Gates

Before a commit in the game: build `debug` and `release`, `ctest --preset debug` and `ctest --preset release`, take a
capture and look at it, and log the numbers in the game's devlog (`devlog/README.md`). A change that can behave
differently on a phone also runs on the device (ask the user first; agents cannot see the device screen).

## iOS

The template's presets: `host-tools` (the macOS `quench_shaderc` and `quench_cook` the cross builds run),
`ios-sim-release` (Simulator, ad-hoc signed), `ios-xcode` (device: Xcode project with Automatic signing,
`-DQUENCH_IOS_TEAM=<team id>`). Commands in [reference.md](reference.md), "iOS"; device runs and signing in
`docs/app.md`, "iOS runs". Cooked textures are ASTC 4x4 in iOS builds and BC7 on the Mac (`TEXTURE_FORMAT auto`).
Never write team ids, device UDIDs or personal names into the repository.

## Rules a game keeps

- No blocking loads and no blocking loop: assets, sounds and long work finish across frames; the engine owns `main`.
- Threads only through `app.jobs()`: no `std::thread`, no second thread pool. Jolt runs on it through
  `PhysicsWorld`.
- Files only through `quench::fs`: assets read-only, writes only into the user directory; no `std::ifstream`,
  `std::ofstream` or `fopen` in game code.
- Graphics only through `quench::gpu` and `quench::render`: no SDL GPU calls or types, no Metal or other platform
  headers in game code; effects by `gpu::Caps`, never by `#ifdef`.
- Right-handed, Y up, meters, kilograms, seconds; reversed depth (near 1, far 0).
- Errors are values (`quench::Result`, `Error`); bad data gets a fallback and a log line, never a crash.
- Every asset has a licence that allows its use, listed in a `CREDITS.md` next to it.

## Common pitfalls

- Replacing the template's player cube breaks `scenarios/smoke.txt`: it checks the template's values (`x`, `z`,
  `speed`). Change the scenario with the game.
- `Binding::key(name)` returns `std::optional<Binding>`: an unknown name is empty, and dereferencing it is undefined.
- `SceneAssets::update()` is the game's job, once per frame in `Game::update`; without it placed models stay
  placeholders.
- Declare `Scene`, then `SceneAssets`, then the `AssetRef`s, or reset them in reverse in `shutdown`:
  `SceneAssets` must go after every reference to its assets and before the scene.
- One `Renderer::render` per scene and frame; set `View::cut` when the camera jumps or the scene switches.
- Timings drawn in ImGui make captures differ between runs; hide them when `app.deterministic()`.
- A capture path without a directory lands in the user directory, which is the real one unless `--user-dir` is
  given.
- `FETCHCONTENT_SOURCE_DIR_QUENCH` stays in the cache: a build that looks pinned may still use the local checkout.
- The first configure of a build directory downloads every pinned dependency; the first configure on a machine
  also builds DXC for the ray query shaders (minutes, once, kept in `~/Library/Caches/Quench`).
- iOS builds need `host-tools` built first; a game that cooks assets needs `quench_cook` there as well (the
  template's presets build and pass both).

## Keeping this skill current

The source is `.agents/skills/quench-game/` in the engine repository; games get a copy from `tools/new_game.py`.
In the engine, a change to what a game builds on (public API, `quench_add_game()`, `cmake/QuenchGame.cmake`,
`cmake/QuenchCook.cmake`, `templates/game/`, `tools/new_game.py`, common flags, scenario commands, asset cooking)
updates this skill in the same commit, and `tools/check_skill.py` checks the paths and names it uses. In a game,
copy the skill again from the engine source when the engine pin moves.
