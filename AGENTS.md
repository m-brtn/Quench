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

**Current state (2026-11-05): the repository holds only these rules, the documentation and the devlog. There is no
engine code, no build and no test yet.** `docs/migration.md` describes what exists in the two games; everything else in
`docs/` is design and plan. Update this paragraph whenever that changes.

The stack: SDL3 (window, input), SDL GPU (Metal on Apple platforms) with one HLSL shader source compiled at build
time, Jolt Physics, Dear ImGui (diagnostics), GLM, miniaudio. Planned additions and their licences:
`docs/dependencies.md`.

## Platforms

macOS and iOS come first: the available hardware is a Mac with an Apple M4 Pro and an iPhone 18 Pro Max. Windows,
Linux, Android and the web (WebAssembly) are later targets. Their foundations are laid now (see "Portability rules"),
but they are not worked on until the user asks.

Claim only platforms that were actually run, and say how (for example "iOS Simulator only" or "Vulkan through MoltenVK
on the Mac"). Status, constraints and open questions per platform: `docs/platforms.md`.

How each backend is verified:

- **Metal**: on the Mac and on the iPhone. Agents cannot see the device screen: ask the user to run device checks and
  record their report as a user report in the devlog.
- **Vulkan**: on the Mac only through MoltenVK, a translation layer over Metal. It checks the SPIR-V path and the
  validation layers, not real Vulkan drivers: say so whenever Vulkan results are mentioned.
- **Web**: the pinned SDL (3.4.18) has no WebGPU backend in SDL GPU (only an experimental pull request,
  libsdl-org/SDL#16020). Until one is usable, web compatibility is verified only for headless modules, built with
  Emscripten and run as CTest under Node.js on the Mac.

## Build and test

Not set up yet. The planned presets, sample app and test layout are in `docs/roadmap.md` (stage 1). Replace this
paragraph with the real commands when the first build exists.

All checks run locally; there is no CI (user decision, 2026-11-05). Gates, in force as soon as code exists:

- **Every code change:** build `debug` and `release`, run `ctest` for both, check formatting (`clang-format
  --dry-run --Werror` on the changed files), and run the sample's capture check against its golden images, looking
  at the image (do not assume).
- **Core, job system, assets or anything threaded:** also `ctest` under the `asan` preset (AddressSanitizer and
  UndefinedBehaviorSanitizer); job system and threading changes also under `tsan` (ThreadSanitizer).
- **Platform, GPU or app code:** also build for iOS; ask the user to run on the iPhone when the change can behave
  differently on the device.
- **Rendering or frame-time-sensitive code:** run the benchmark and compare with the numbers before the change (see
  "Verification").
- **Engine code a game uses, after each change:** `tools/check_games.sh` (planned) builds every game against the local
  engine and checks only that each one starts, renders and quits with exit code 0.
- **At the end of the work, before the commits:** the games' full gates once (TrailForge: `debug` + `release`,
  `ctest`, `tools/scenarios/full_check.txt`; Ashfall: `debug` + `release`, `ctest`, the capture check in its
  `AGENTS.md`) and their captures compared before and after with `compare_captures.py`.

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
  capabilities (`GpuCaps`), never by platform `#ifdef` (for example shadow maps or ray-traced shadows, SSR or
  ray-traced reflections).
- **Shaders.** HLSL compiled at build time to SPIR-V, MSL / metallib and DXIL (WGSL later). Shared shader code comes
  from the engine shader library by `#include`. Uniform structs are defined once, in headers read by both C++ and
  HLSL; uniform blocks use only 16-byte members (`vec4`, `mat4`), so the C++ and HLSL layouts match without padding
  rules.
- **Main loop.** SDL main callbacks (`SDL_AppInit`, `SDL_AppIterate`, `SDL_AppEvent`, `SDL_AppQuit`): the engine owns
  `main`, a game implements the engine's game interface. No blocking `while (running)` loop: browsers and mobile
  lifecycles do not allow it.
- **Files.** Engine and game runtime code reads assets and writes user files only through `quench::fs` (SDL_IOStream
  underneath: assets from the bundle, the APK or the browser's virtual file system; writes only to the user directory
  from `SDL_GetPrefPath`). No `std::ifstream`, `std::ofstream` or `fopen` for assets or saves. Host-only tools and
  tests may use the standard library.
- **Threads.** Only through the engine job system (enkiTS, planned), which also runs Jolt's jobs through a
  `JPH::JobSystemWithBarrier` implementation, so there is one thread pool. It must also work with a single thread (the
  web without cross-origin isolation). Worker threads run at user-interactive QoS on Apple platforms: the frame thread
  waits for them, and a lower QoS would be a priority inversion.
- **No ECS in the core, no plugin system.** Engine APIs take plain handles; a game may put an ECS on top (Flecs is the
  candidate, as an optional module). Modules are linked statically.
- **No abstraction without a concrete need.** The GPU layer above is the decided exception, for the reasons given.
- Resources are released via RAII; member order defines destruction order.
- Fixed simulation step 1/120 s by default; rendering interpolates between ticks.

## Portability rules

These keep the later platforms reachable without rewriting the games. They apply now, even while only macOS and iOS
are built:

- Nothing outside a platform backend may assume Apple (or any single OS): platform code lives in clearly named
  backend files or behind `QUENCH_PLATFORM_*` checks in one place.
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
- Engine targets build with `-Wall -Wextra -Werror` (`/W4 /WX` when MSVC is added). Public headers must also compile
  without warnings under the games' flags. Third-party code is built without `-Werror`.

### Errors and assertions

- Engine APIs do not throw. A failure the caller can handle is returned as a value: `quench::Result<T>` (a value or an
  error message), to be replaced by `std::expected` if the engine moves to C++23.
- Bad data at run time (a missing or corrupt asset, an unsupported format or device feature) is an error with a
  fallback (placeholder, disabled feature) and a log line, never a crash.
- Programming errors (broken invariants, wrong use of an API) are `QUENCH_ASSERT(condition, "message")`: active in
  debug and development builds, compiled out in release; it logs the file, line and message and stops in the debugger.
- Exceptions from third-party code do not cross engine APIs. Whether engine targets build with `-fno-exceptions` is
  decided in stage 1, after checking every dependency.

### Logging

- Engine runtime code logs only through `quench::log`: levels error, warning, info and debug, a category per module
  (`gpu`, `assets`, `physics`, `audio`, `app`, ...). No `printf`, `std::cout` or `std::cerr` in engine runtime code;
  host tools and tests may print.
- Output goes to SDL_Log (the Xcode console and the device log on iOS, logcat on Android, the browser console on the
  web) and to stderr on desktop. Games can add sinks (an on-screen console, a file in the user directory).
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

- **Golden captures.** Reference images of the sample and of the visual tests live in `tests/golden/`: small PNGs of
  procedural content only, stored in git directly. They are compared with `tools/render/compare_captures.py` (to be
  moved from TrailForge) against thresholds set per image (mean absolute difference, share of pixels differing by more
  than 8 levels). A golden image changes only with an intentional rendering change, in the same commit, after looking
  at both images, with the before and after numbers in the devlog. References are per backend and device (Metal on
  the Mac first). Captures of the same binary differ slightly between runs: identity claims need a same-process A/B.
- **Performance.** A benchmark scenario in the sample (fixed camera path, fixed frame count) reports CPU frame time
  (median and 99th percentile) and GPU time per pass. Budgets per device (Mac M4 Pro, iPhone 18 Pro Max) are set when
  the sample exists and kept in `docs/performance.md`. A change that makes the median or the 99th percentile more than
  5% worse is explained in the devlog or not made. Timings drift between runs: compare before and after in the same
  session. iPhone numbers come from the user's runs.
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
- An engine change that breaks a game's build is fixed in the same session or not made.
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
- Push with a normal push. Rewrite history or force push only when the user explicitly asks.

## Layout

```
AGENTS.md       these rules
README.md       short overview
LICENSE         MIT licence of the engine
.clang-format   code format; .editorconfig for editors
docs/           documentation (index docs/README.md): architecture, platforms, dependencies, migration, roadmap
devlog/         daily development log (template in devlog/README.md)
```

The planned source layout is in `docs/architecture.md`; move it here when it exists.
