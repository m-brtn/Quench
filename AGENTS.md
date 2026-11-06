# AGENTS.md

Instructions for coding agents working on Quench. Human-facing documentation is in `README.md` (short overview) and
`docs/` (index `docs/README.md`). The daily development log is in `devlog/`.

## Project overview

Quench is a C++20 game engine for games of different genres. Its first code comes from two games by the same author,
which carry their own copy of an engine today:

- **TrailForge** (`/Users/mbrtn/Projects/trailforge-public`, https://github.com/m-brtn/trailforge, public): a driving
  simulator. The engine was written for it (first commit 2026-10-24). Namespace `tf`, CMake prefix `TRAILFORGE_`,
  shader entry guards `TF_ENTRY_<name>`.
- **Ashfall** (`/Users/mbrtn/Projects/strategy`, https://github.com/m-brtn/strategy, private): a top-down action
  shooter. Its engine part was copied from TrailForge on 2026-11-02 and has diverged since. Namespace `af`, prefix
  `ASHFALL_`, guards `AF_ENTRY_<name>`. It is private only because of the licences of its bought model and texture
  packs, which live in its git-ignored `assets_local/`: its code and documentation may be moved and quoted here, the
  packs may not (see "Asset licences").

Both games are to build on the engine, and future games start from it. Long-term features (hardware ray tracing,
skeletal animation, navigation, asset streaming) are listed in `docs/roadmap.md`.

**Current state (2026-11-05, version 0.4.0): stages 1 to 3 and 5 of `docs/roadmap.md` are in place, stage 4 for
TrailForge only, stage 6 except shader hot reload: the CMake skeleton and presets,
`quench_core` (`Result`, `QUENCH_ASSERT`, `quench::log`, `FixedStepClock`, JSON, `quench::fs` with its standard
library backend, worker thread priority, the job system `quench::JobSystem` on enkiTS), `quench_physics`
(`PhysicsWorld` on Jolt, its jobs on the engine's job system), `quench_assets` (asset IDs, the `AssetManager` cache
with reference counts, placeholders, budgets, loads on jobs, a per-frame upload budget, hot reload on the Mac,
streaming by distance; KTX2 textures and cooked `.qmodel` models), `quench_cook` (the host tool: glTF, GLB, PNG, JPEG,
KTX2 to BC7 or RGBA8 KTX2 with mips and meshoptimizer models; `quench_cook_assets()`), the shader build
(`quench_shaderc`, `quench_add_shaders()`) and the engine shader library in
`shaders/quench/` (uniform structs shared by C++ and HLSL), `quench_gpu` (the GPU layer over SDL GPU, with swapchain
compositions for HDR), `quench_app` and `quench_main` (SDL main callbacks, the game interface, input actions, ImGui,
common flags, scenarios with points in window points, percent or names a game gives (`Game::scenarioPoint`), HDR
display output (`--hdr auto|on|off`, an HDR tone curve up to the display's headroom, read every frame on Apple
platforms; the UI at paper white; SDR captures), captures), `quench_render` (the retained scene: meshes, textures,
materials, instances and lights by handle; the standard frame of cascaded sun shadows, the HDR pass with reversed
depth, sky and atmosphere, height fog, bloom, ACES or AgX tone mapping and TAA or FXAA, with fallbacks by device
capability; game passes and surface shaders; `SceneAssets` puts asset textures and models into the scene) and
`quench_add_game()` (macOS executables, iOS bundles, cooked assets).
`examples/hello` is the minimal sample, `examples/demo` the engine demo (a section per feature, each with a scripted
check and golden captures; `tools/package_demo.sh` packages it into `dist/`); `templates/game` and
`tools/new_game.py` start a new game. Tests run on the Mac (Metal, and Vulkan through MoltenVK), in the iOS Simulator,
on the user's iPhone (one signed test app; signed by the user's paid team; not yet with the stage 6 code) and, for
the headless modules, as WebAssembly under Node.js. HDR output was looked at on the iPhone (the user, 2026-11-05:
"everything is fine"), never on a Mac display (this Mac's has no HDR range). TrailForge builds on the engine's
`quench_core` (`FixedStepClock`,
JSON), `quench_physics` and shader build, renders its own passes through `quench_gpu` (not through `quench_render`),
and runs on `quench_main` / `quench_app` (the engine's window, loop, flags, scenario runner and captures; pinned
`d708783`, not yet moved to a release) with its own input layer fed through `Game::event` and its own physics clock;
Ashfall is not being migrated for now (user decision, 2026-11-05) and still uses its own code;
`tools/check_games.sh` builds both games against this checkout and starts them; neither game uses the job system or
the asset system yet. No shader hot reload, ASTC or Basis Universal textures, normal maps in the standard surface,
asset hot reload on a device, audio or animation yet (stage 6 remaining work, stages 7 and 8).**
`docs/migration.md` describes what exists in the two games; `docs/app.md`, `docs/render.md`, `docs/assets.md`,
`docs/demo.md`, `docs/performance.md` and `docs/platforms.md` describe what is built; the rest of `docs/` is design
and plan beyond stage 6. Update this paragraph whenever that changes.

The stack: SDL3 (window, input), SDL GPU (Metal on Apple platforms) with one HLSL shader source compiled at build
time, Jolt Physics, Dear ImGui (diagnostics), GLM, enkiTS (jobs), xxHash (asset IDs), meshoptimizer and bc7enc_rdo's
BC7 decoder (assets), miniaudio (sound decoding and device output); the cooker adds fastgltf and stb_image at build
time. Pins, planned additions and their licences: `docs/dependencies.md`.

## Platforms

macOS and iOS come first: the available hardware is a Mac with an Apple M4 Pro and an iPhone 18 Pro Max. Windows,
Linux, Android and the web (WebAssembly) are later targets. Their foundations are laid now (see "Portability rules"),
but they are not worked on until the user asks.

Claim only platforms that were actually run, and say how (for example "iOS Simulator only" or "Vulkan through MoltenVK
on the Mac"). Status, constraints and open questions per platform: `docs/platforms.md`.

How each backend is verified:

- **Metal**: on the Mac and on the iPhone. Agents can install and launch apps on the iPhone through `devicectl` (ask
  the user first; the phone must be unlocked) and read their console and the files they write, but cannot see the
  device screen: ask the user to check what is on it and record their report as a user report in the devlog.
- **Vulkan**: on the Mac only through MoltenVK, a translation layer over Metal (Homebrew's `vulkan-loader`,
  `molten-vk` and `vulkan-validationlayers`; the `gpu_vulkan.*` CTest variant). It checks the SPIR-V path and the
  Khronos validation layer's core checks, not real Vulkan drivers: say so whenever Vulkan results are mentioned.
- **Web**: the pinned SDL (3.4.18) has no WebGPU backend in SDL GPU (only an experimental pull request,
  libsdl-org/SDL#16020). Until one is usable, web compatibility is verified only for headless modules, built with
  Emscripten and run as CTest under Node.js on the Mac (the `wasm-node` preset, single-threaded, with stack overflow
  checks). Node.js is not a browser: say "under Node.js" whenever web results are mentioned.

## Build and test

Requirements: CMake 3.28+, Ninja, Xcode with its command line tools, and the Metal toolchain for precompiled
metallibs (`xcodebuild -downloadComponent MetalToolchain`; without it the MSL source is embedded and compiled at
startup). The first configure of every preset downloads the pinned dependencies.

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug -j8   # also release, asan, tsan
cmake --preset headless && cmake --build --preset headless && ctest --preset headless -j8   # no SDL, no GPU
tools/format.sh --check            # tools/format.sh without arguments formats in place
# iOS: the shaders and assets are compiled by a macOS quench_shaderc and quench_cook from the host-tools preset
cmake --preset host-tools && cmake --build --preset host-tools
cmake --preset ios-sim-release && cmake --build --preset ios-sim-release        # or ios-device-release (libraries)
# Tests in the iOS Simulator, run through `xcrun simctl spawn booted` (build and ctest need a booted Simulator)
xcrun simctl boot "iPhone 18 Pro Max"
cmake --preset ios-sim-tests && cmake --build --preset ios-sim-tests && ctest --preset ios-sim-tests -j4
# Web check: headless tests as WebAssembly under Node.js, single-threaded (needs host-tools and Homebrew's
# emscripten; the preset points EMSDK_PYTHON at Homebrew's python3, Xcode's is too old)
emcmake cmake --preset wasm-node && cmake --build --preset wasm-node && ctest --preset wasm-node -j8
# The sample: flags with --help; the benchmark (docs/performance.md) in Release with vsync; automated runs make no
# sound (--audio-device null or --no-audio)
build/release/examples/hello/hello --frame-size 1920x1080 --script examples/hello/scenarios/benchmark.txt \
 --audio-device null
# iPhone: an Xcode project with Automatic signing; install and launch with devicectl (docs/app.md, "iOS runs").
# The user's paid team (Xcode's accounts) with plain bundle ids; a free Personal Team: 3 apps, 7-day profiles
cmake --preset ios-xcode -DQUENCH_IOS_TEAM=<team id> -DQUENCH_IOS_BUNDLE_ID_SUFFIX=
xcodebuild -project build/ios-xcode/Quench.xcodeproj -scheme quench_device_tests -configuration Release \
    -destination id=<device udid> -allowProvisioningUpdates build                  # also -scheme quench_hello
# A new game from templates/game, built against this checkout and started once
python3 tools/new_game.py "My Game" ../my-game
# The engine demo (docs/demo.md), one section or the menu; its package for dist/ at the end of a stage
build/release/examples/demo/quench-demo --section shadows
tools/package_demo.sh                    # --ios also builds a development-signed .ipa (ios-xcode, not installed)
# The asset cooker (docs/assets.md); games and the demo cook at build time through quench_cook_assets()
build/debug/quench_cook --help
```

Every test case is its own CTest test (`core.json_round_trip`, `physics.physics_sphere_settles`, ...). A test
executable runs one case by name (`build/debug/tests/quench_core_tests json_round_trip`), all cases without
arguments, and lists them with `--list`. A case that cannot run in a build (threads in the single-threaded web build)
calls `QUENCH_SKIP` and is reported as skipped, not passed.

GPU tests (`quench_gpu_device_tests`) render small offscreen targets and check the pixels they read back. On the Mac
every case runs twice: `gpu_device.*` on the default driver (Metal, with the Metal API validation layer,
`MTL_DEBUG_LAYER=1`) and `gpu_vulkan.*` on Vulkan through MoltenVK (registered when Homebrew's `vulkan-loader` is
found; with `vulkan-validationlayers` installed, a validation error or warning in the output fails the case). With
the driver named in `QUENCH_TEST_GPU_DRIVER` (set by CTest) a missing device fails the case instead of
skipping it. `gpu_device.window_present` opens a small window for a moment; it is skipped in the iOS Simulator test
executables (no app lifecycle there) and runs in the iPhone test app, which runs every case as `SDL_main`.

The sample's checks are CTest tests too: `hello_metal` and `hello_vulkan` render it at 1280 x 720 and compare the
capture (`build/<preset>/tests/captures/<test>.png`) with `tests/golden/<backend>/<device>/hello.png`;
`hello_input` runs `examples/hello/scenarios/input.txt` (scripted keyboard, gamepad and touch input). Flags,
scenario commands, captures and golden images: `docs/app.md`. The demo's checks are the same kind:
`demo_<section>_metal` and `demo_<section>_vulkan` run `examples/demo/scenarios/<section>.txt` and compare the
capture at its end with `tests/golden/<backend>/<device>/demo_<section>.png`; `demo_menu` checks the menu
navigation (`docs/demo.md`). The renderer's own cases are `render.*` and `render_vulkan.*`.

All checks run locally; there is no CI (user decision, 2026-11-05). Gates:

- **Every code change:** build `debug` and `release`, run `ctest` for both (it includes the capture checks of the
  sample and the demo against their golden images), `tools/format.sh --check`, and look at the captures in
  `build/debug/tests/captures/` (do not assume).
- **Core, job system, assets or anything threaded:** also `ctest` under the `asan` preset (AddressSanitizer and
  UndefinedBehaviorSanitizer); job system and threading changes also under `tsan` (ThreadSanitizer).
- **Headless modules (core, physics, assets, animation, navigation, audio mixer):** also the `wasm-node` web check.
- **Platform, GPU or app code:** also build for iOS and run `ios-sim-tests`; when the change can behave differently
  on the device, run the device test app and the sample on the iPhone (`ios-xcode`, `devicectl`) after asking the
  user, and ask the user to check what is on the screen.
- **`cmake/QuenchGame.cmake`, the template or anything a game's build uses:** also `tools/new_game.py` into a
  temporary directory: it builds the new game against this checkout and starts it.
- **Rendering or frame-time-sensitive code:** run the benchmark and compare with the numbers before the change (see
  "Verification").
- **The games are not built or tested in engine work** (user decision, 2026-11-05 21:34): neither
  `tools/check_games.sh` nor the games' own gates run unless the user asks. The engine is verified by its own tests,
  the samples and the demo. When asked: `tools/check_games.sh` builds every game against the local engine and checks
  only that each one starts, renders and quits with exit code 0 (`[--preset NAME] [trailforge] [ashfall]`; checkouts
  from `TRAILFORGE_DIR` and `ASHFALL_DIR`, default `../trailforge-public` and `../strategy`; each game builds in its
  own `build/quench-check-<preset>`); the games' full gates are TrailForge: `debug` + `release`, `ctest`,
  `tools/scenarios/full_check.txt`; Ashfall: `debug` + `release`, `ctest`, the capture check in its `AGENTS.md`;
  captures compared before and after with `compare_captures.py`. Problems inside a game are not fixed in engine
  work.
- **At the end of each stage:** the stage adds its section to the demo (`examples/demo`, with a scripted check and
  golden captures for Metal and Vulkan through MoltenVK), and `tools/package_demo.sh` builds the demo into `dist/`
  (user decision, 2026-11-05): Release, `dist/QuenchDemo-<version>-<shorthash>-macos.zip` with the app, `LICENSE`
  and `THIRD_PARTY_LICENSES`; it runs the demo's checks, unzips the archive, starts the app once and saves a capture
  next to it (look at it). `--ios` also builds a development-signed `.ipa` into
  `dist/QuenchDemo-<version>-<shorthash>-ios.zip` (`ios-xcode`, signed by a paid team: `--team ID` or
  `QUENCH_IOS_TEAM`; it refuses a free Personal Team and installs nothing). `dist/` is not committed; say in the
  devlog which files it holds.

## Architecture rules

Details and rationale: `docs/architecture.md`.

- **Designed for any game, not copied from one.** The games are where code comes from, not the model for the engine.
  Code moved from a game is generalized: game concepts (vehicles, levels, combat, fixed material enums, game
  command-line flags) stay in the game, and the engine's names, layout and APIs are designed for any genre. A game
  keeps its behaviour when it switches to engine code (its gates and captures show it); the engine side does not have
  to keep the game's structure.
- **Names.** Namespace `quench`; CMake prefix `QUENCH_`; targets `quench_<module>`; shader entry guards
  `QUENCH_ENTRY_<name>` and `QUENCH_STAGE_VERTEX` / `QUENCH_STAGE_FRAGMENT`. Games keep their own namespaces and
  prefixes.
- **Headless core.** Simulation, physics, assets, animation, navigation and the audio mixer must not depend on
  windowing, the GPU or rendering, so they run in CTest without a window (and under Node.js for the web check).
- **Graphics goes through `quench::gpu`.** Backends: SDL GPU (portable, the first one) and native Metal (planned).
  The Metal backend exists for hardware ray tracing, GPU timestamps and MetalFX: SDL GPU does not offer them, and its
  maintainers decline to expose native object handles (libsdl-org/SDL#11147, #12094). SDL GPU types never appear in
  public engine headers, and games do not call SDL GPU directly. A new backend or a native feature is added only for a
  named need and verified on real hardware.
- **Rendering.** The engine renders a standard frame (shadows, main HDR pass, sky, post-processing chain) assembled
  from modules a game enables; games add their own passes and material shading through engine APIs. The scene is
  retained in the engine: meshes, instances, materials and lights are registered, not resubmitted as per-frame draw
  lists, so that ray tracing can build acceleration structures. Effects are slots with fallbacks chosen by device
  capabilities (`gpu::Caps`), never by platform `#ifdef` (for example shadow maps or ray-traced shadows, SSR or
  ray-traced reflections).
- **Shaders.** HLSL compiled at build time to SPIR-V, MSL / metallib and DXIL (WGSL later). Shared shader code comes
  from the engine shader library by `#include`. Uniform structs are defined once, in headers read by both C++ and
  HLSL; uniform blocks use only 16-byte members (`vec4`, `mat4`), so the C++ and HLSL layouts match without padding
  rules.
- **Main loop.** SDL main callbacks (`SDL_AppInit`, `SDL_AppIterate`, `SDL_AppEvent`, `SDL_AppQuit`): the engine owns
  `main`, a game implements the engine's game interface. No blocking `while (running)` loop: browsers and mobile
  lifecycles do not allow it.
- **Files.** Engine and game runtime code reads assets and writes user files only through `quench::fs`: relative
  paths with '/' and no '..', assets read-only, writes only to the user directory. Its default backend uses the
  standard library (host tests and tools); the app installs an SDL_IOStream backend (assets from the bundle, the APK
  or the browser's virtual file system; the user directory from `SDL_GetPrefPath`). No `std::ifstream`,
  `std::ofstream` or `fopen` for assets or saves elsewhere. Host-only tools and tests may use the standard library.
- **Threads.** Only through the engine job system (`quench::JobSystem` on enkiTS, `App::jobs()` in a game), which
  also runs Jolt's jobs through a `JPH::JobSystemWithBarrier` implementation, so there is one thread pool
  (`PhysicsWorld(&jobs)`; a `PhysicsWorld` without one runs single-threaded). It must also work with a single thread
  (the web without cross-origin isolation). Every worker thread calls `quench::raiseWorkerThreadPriority()`
  (user-interactive QoS on Apple platforms): the frame thread waits for the workers, and a lower QoS would be a
  priority inversion. The one exception is the audio device's callback thread, owned by the operating system's audio
  API (Core Audio through miniaudio) and scheduled by it with a real-time deadline: it only mixes
  (`AudioSystem::render`), with no allocation, freeing, locks, logging, file access or waits, and talks to the game
  thread through lock-free queues and atomics; decoding and stream refills run on the job system
  (`docs/architecture.md`, "Threads and jobs"; `docs/audio.md`, "Threads").
- **No ECS in the core, no plugin system.** Engine APIs take plain handles; a game may put an ECS on top (Flecs is the
  candidate, as an optional module). Modules are linked statically.
- **No abstraction without a concrete need.** The GPU layer above is the decided exception, for the reasons given.
- Resources are released via RAII; member order defines destruction order.
- Fixed simulation step 1/120 s by default; rendering interpolates between ticks.

## Portability rules

These keep the later platforms reachable without rewriting the games. They apply now, even while only macOS and iOS
are built:

- Nothing outside a platform backend may assume Apple (or any single OS): platform code lives in clearly named
  backend files or behind `QUENCH_PLATFORM_*` checks, which only `src/core/Platform.h` defines.
- Keep the SPIR-V, DXIL and MSL outputs of the shader build working even where only Metal is run.
- Texture formats are chosen by device capability: BC7 today; ASTC or Basis Universal transcoding is needed for Android
  and the web.
- No blocking waits on the main thread for loading; long work goes to the job system and finishes across frames.

## Coordinate system and units

- Right-handed, **Y up**; SI units: meters, kilograms, seconds.
- Jolt and GLM matrices are column-major; NDC depth range 0..1 (`GLM_FORCE_DEPTH_ZERO_TO_ONE`).
- The scene is rendered with reversed depth (near 1, far 0) for depth precision; shadow maps use standard depth.

## Code rules

### Style

- `.clang-format` at the repository root is the format of all engine code (`xcrun clang-format` from Xcode works).
  Code moved from a game is formatted as part of the move.
- Naming: types and enum values `PascalCase`; functions and variables `camelCase`; members `m_name`; constants
  `kName`; file-local globals `g_name` in an anonymous namespace; macros `QUENCH_UPPER_CASE`. A file is named after its
  main type (`PhysicsWorld.h`, `PhysicsWorld.cpp`).
- Headers use `#pragma once`. Include order, separated by blank lines: the file's own header, engine headers,
  third-party headers, the standard library.
- Engine targets build with `-Wall -Wextra`, plus `-Werror` when the engine is the top-level project (`QUENCH_WERROR`;
  inside a game, a warning from a newer compiler must not break the game's build). `/W4 /WX` when MSVC is added.
  Public headers must also compile without warnings under the games' flags (both use `-Wall -Wextra` today).
  Third-party code is built without `-Werror`.

### Errors and assertions

- Engine APIs do not throw. A failure the caller can handle is returned as a value: `quench::Result<T>` (a value or an
  error message), to be replaced by `std::expected` if the engine moves to C++23.
- Bad data at run time (a missing or corrupt asset, an unsupported format or device feature) is an error with a
  fallback (placeholder, disabled feature) and a log line, never a crash.
- Programming errors (broken invariants, wrong use of an API) are `QUENCH_ASSERT(condition, "message")`: active in
  debug and development builds, compiled out in release; it logs the file, line and message and stops in the debugger.
- Exceptions from third-party code do not cross engine APIs. Engine targets build with exceptions enabled (decided in
  stage 1): both games build Jolt with exceptions and throw their own (TrailForge's renderer throws
  `std::runtime_error`), and a game exception passing through engine frames must still run their destructors. Engine
  runtime code does not throw; host tools may (SPIRV-Cross reports errors by throwing).

### Logging

- Engine runtime code logs only through `quench::log`: levels error, warning, info and debug, a category per module
  (`gpu`, `assets`, `physics`, `audio`, `app`, ...). No `printf`, `std::cout` or `std::cerr` in engine runtime code;
  host tools and tests may print.
- Output goes to stderr (a default sink) and, once the app module adds its sink, to SDL_Log (the Xcode console and the
  device log on iOS, logcat on Android, the browser console on the web). Games can add sinks (an on-screen console, a
  file in the user directory).
- One line per message, with the values that explain it (path, size, count, device feature). Messages that could
  repeat every frame are rate-limited. The debug level is compiled out in release builds.

### Public API

- Public headers live in `include/quench/<module>/`; sources and internal headers in `src/<module>/`. Games include
  only `include/`.
- Public headers may use the standard library, GLM, and the library a module wraps where a wrapper would add nothing
  (Jolt in `quench_physics`, ImGui in the debug UI). Never SDL GPU types and never platform headers (Metal, Windows,
  Android).
- Every public header compiles on its own (a test builds each header in its own translation unit).
- The engine is linked statically: no ABI promises. While the version is 0.x, breaking API changes are allowed; each
  one is listed under "Changes for games" in the release notes.

## Verification

- **Golden captures.** Reference images of the sample and of the visual tests live in
  `tests/golden/<backend>/<device>/`: small PNGs of procedural content only, stored in git directly. They are compared
  with `quench_image_compare` (`tools/imagecompare`: mean absolute difference at most 0.5, at most 0.1% of the pixels
  differing by more than 16 levels; the tool takes other thresholds as flags). A golden image changes only with an
  intentional rendering change, in the same commit, after looking at both images, with the before and after numbers
  in the devlog. References are per backend and device (Metal and Vulkan through MoltenVK on the Mac today). Two
  Metal runs of the same binary have been identical so far; TrailForge's captures differ slightly between runs, so
  identity claims about a game need a same-process A/B.
- **Performance.** The sample's benchmark scenario (`examples/hello/scenarios/benchmark.txt`, a 10 s region in real
  time, so the frame count follows the display) reports frame, CPU and GPU time (average and percentiles; GPU time per
  frame, per pass later). Budgets
  per device and how to compare runs: `docs/performance.md`. A change that makes the CPU median or 99th percentile
  more than 5% worse is explained in the devlog or not made. Timings drift between runs: compare before and after in
  the same session. iPhone numbers come from runs on the user's iPhone (launched through `devicectl` after asking the
  user, or by the user).
- **Sanitizers.** Presets `asan` (AddressSanitizer and UndefinedBehaviorSanitizer) and `tsan` (ThreadSanitizer) run
  the headless tests on the Mac; when they are required is in "Build and test".

## Dependencies

- Pinned archives of a specific commit + SHA256 through CMake FetchContent; no floating branches. The first configure
  needs internet.
- Shared dependencies keep the pins of the games; when a pin changes, say which games are affected.
- Permissive licences only (MIT, zlib, BSD, Apache-2.0, public domain). The licence texts (including Apache-2.0
  NOTICE files) are collected in `THIRD_PARTY_LICENSES` and shipped with every package.
- The current and planned list with versions and verified capabilities: `docs/dependencies.md`. When citing what a
  library can do, check its source or documentation first, not memory.

## How games use the engine

- Games fetch this repository with FetchContent at a pinned commit (normally a release tag's commit). For local work
  they set `FETCHCONTENT_SOURCE_DIR_QUENCH=/Users/mbrtn/Projects/engine`. An engine change must not work only with
  the local override.
- Games stay on their pinned engine commit, so an engine change cannot break them until they move the pin. Breaking
  API changes are listed under "Changes for games" in the release notes; a game is moved to a newer engine only when
  the user asks, with its own gates.
- Changes inside a game follow that game's `AGENTS.md` (its devlog, its gates, its commit rules). The migration
  procedure is in `docs/migration.md`.
- Commit order: the engine first, then each game with its engine pin updated.

## Versioning and releases

- The version is set in `project(Quench VERSION x.y.z)`. While it is 0.x: minor for a finished stage or a breaking
  change, patch for fixes only.
- A release is a git tag `v<version>` with notes in `docs/releases/v<version>.md`: what changed, what was verified
  (numbers from the devlog) and "Changes for games" (renamed or removed APIs and what each game has to change).
  Releases are source only, no binaries.
- Tags and GitHub releases are created only when the user asks.

## Licence

The engine is MIT licensed (`LICENSE`). Code moved from the games comes from the same author and is released under the
engine's licence when it lands here. Code derived from third-party sources (library samples, papers with code) keeps
its original licence notice; record the source in a comment and in `THIRD_PARTY_LICENSES`.

## Conventions

### Language

All repository content is English only: code, comments, identifiers, UI strings, log and console messages, tests,
commit messages, docs and the devlog.

### Devlog

Every agent doing work in the project keeps a devlog in `devlog/YYYY-MM-DD.md` (local date).

- If there is no file for today, create it from the template in `devlog/README.md`.
- If the file exists, append to it. Never rewrite or delete other agents' entries; you may only add to them (for
  example, replace an **IN PROGRESS** status with the outcome and the time).
- Sections, in this order:
  1. `# Devlog — YYYY-MM-DD (weekday)`
  2. `## Goal of the day`
  3. `## Stages` — chronological, with `HH:MM` times
  4. `## Problems and solutions` — problem, cause, solution
  5. `## Results` — only what was actually verified, with numbers
  6. `## Limitations and open questions` — unverified items, anomalies, simplifications
  7. `## Next steps`
- Keep verified and unverified items clearly separate. Never present a plan or an expectation as a result.
- Update the entry after each significant stage (build, fix, tests, new feature) and at the end of the work. Mark an
  unfinished stage as **IN PROGRESS**.
- Work done inside a game is logged in that game's devlog; the engine devlog links to it in one line.
- Write briefly and factually.

Example stage entry:

```markdown
- **14:30 — Physics wrapper moved.** `PhysicsWorld` from TrailForge `0188a3a`; TrailForge Release ctest 113/113,
  full_check 0 failed.
```

### Asset licences

The repository will be public. Every asset in it (test models, textures, sounds, fonts, animations) must be CC0
(Poly Haven, ambientCG, Kenney, Quaternius) or carry a licence that allows redistribution, listed in a `CREDITS.md`
next to the files. Check the licence of every file, not only of the pack. Do not add content of unknown licence,
content extracted from games or mods, or models of real vehicles with manufacturer marks, not even temporarily: it
stays in the git history after deletion. Never copy anything from Ashfall's `assets_local/` (bought packs licensed
for use only) or TrailForge's `external_assets/` into this repository. Prefer small procedural test content; use Git
LFS only when unavoidable and keep it small.

### Honesty

- Do not claim features, platforms or performance that were not verified. Results come with numbers; visual quality
  claims only from a capture that was actually looked at.
- Say when something is a stub, a fallback, a heuristic or a simplification.
- When code moves from a game, record the source repository and commit in the devlog.

### Commits and pushing

- Commit and push only when the user asks. Small, focused commits with a descriptive English message (imperative
  subject line, body with what was verified).
- Commit only after the gates above pass; mention the verified numbers in the devlog.
- Do not commit build output, captures or local files (`build/`, `dist/`, `*.bmp`, `*.png` outside `tests/golden/`
  and asset folders, `imgui.ini`, `compile_commands.json`).
- Never commit secrets (API keys live only in the environment).
- Never write personal identifiers into repository files, commit messages or tag and release texts: the user's name,
  Apple ID emails, Apple team IDs, certificate IDs, device UDIDs or device names (user decision, 2026-11-05). They go
  only into configure options and the environment (`-DQUENCH_IOS_TEAM=<team id>`, `QUENCH_IOS_TEAM`, `--team`);
  docs and the devlog use placeholders (`<team id>`, `<udid>`, "the user's paid team", "the user's iPhone"), also
  when they quote tool output (`codesign`, `devicectl`, Xcode's accounts, `tools/package_demo.sh`'s team line).
- Push with a normal push. Rewrite history or force push only when the user explicitly asks.

## Layout

```
AGENTS.md              these rules
README.md              short overview
LICENSE                MIT licence of the engine; THIRD_PARTY_LICENSES: licences of the libraries it ships
CMakeLists.txt         engine targets; CMakePresets.json: configure, build and test presets
cmake/                 Dependencies (runtime pins), ShaderToolchain (shader compiler pins), CookToolchain (asset
                       cooker pins), QuenchShaders (quench_add_shaders), QuenchCook (quench_cook_assets),
                       EmbedShaders, QuenchTargets (warnings, include directories), QuenchGame (quench_add_game)
                       and Info.ios.plist.in, patches/ (SdlIosSimulator: the SDL GPU patch for the iOS Simulator)
include/quench/        public headers per module: core/, physics/, assets/, audio/, gpu/, app/, render/
src/                   sources and internal headers per module (src/core/Platform.h: QUENCH_PLATFORM_*;
                       src/core/Jobs.cpp: the job system; src/physics/JoltJobSystem: Jolt's jobs on it;
                       src/assets/: the asset manager, KTX2, cooked models, streaming; src/audio/: the mixer,
                       decoders, streams, the device and the iOS audio session (AudioSessionApple.m);
                       src/gpu/sdlgpu/: the SDL GPU backend; src/app/Main.cpp: quench_main; src/render/: the scene,
                       SceneAssets, the frame modules and their shaders)
shaders/quench/        the engine shader library (#include "quench/..."): BRDF, shadows, sky, fog, tone curves,
                       surface shaders; SceneUniforms.h, the uniform structs shared by C++ and HLSL
examples/hello/        the sample game: lit mesh, ImGui, input and benchmark scenarios, icon
examples/demo/         the engine demo: a menu of sections (rendering features, HDR output, physics, input,
                       assets, audio), per section a scenario with a golden capture; smoke and benchmark scenarios;
                       assets_src/ (procedural sources cooked at build time) and assets/ (files used as they are)
templates/game/        starting point for a new game (copied by tools/new_game.py)
tools/shaderc/         quench_shaderc, the build-time shader compiler
tools/cook/            quench_cook, the build-time asset cooker (glTF, GLB, PNG, JPEG, KTX2 -> KTX2, .qmodel;
                       WAV, FLAC, MP3 checked or converted to WAV)
tools/assets/          make_demo_assets.py: writes the demo's procedural source assets; make_demo_sounds.py: the
                       demo's procedural sounds (run at build time)
tools/imagecompare/    quench_image_compare: capture against golden image
tools/icon/            make_icon.py: draws the icons of the sample and the demo
tools/new_game.py      creates a game from templates/game
tools/check_games.sh   builds every game against this checkout, starts it, checks exit code 0 and a capture
tools/package_demo.sh  builds the demo into dist/ (macOS .app in a zip; --ios: a development-signed .ipa)
tools/format.sh        clang-format over the C and C++ sources (--check)
tests/                 harness/, core/ (job system included), physics/, assets/, audio/ (mixer, streams, decoders,
                       device; data/: CC0 test sounds), cook/ (and CookCli.cmake), gpu/ (shader tests; GPU device
                       tests and their shaders), app/, render/ (scene, scene assets and frame tests), header check,
                       RunCapture.cmake (capture checks); golden/ (reference captures)
docs/                  documentation (index docs/README.md): architecture, app shell, jobs, assets and the cook,
                       audio, render module, engine demo, performance, platforms, dependencies, migration, roadmap,
                       releases/
dist/                  demo packages from tools/package_demo.sh (not committed)
devlog/                daily development log (template in devlog/README.md)
.clang-format          code format; .editorconfig for editors
```

Modules and directories that do not exist yet are planned in `docs/architecture.md`.
