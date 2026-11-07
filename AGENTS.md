# AGENTS.md

Instructions for coding agents working on Quench. Human-facing documentation is in `README.md` (the front page, for
people who want to make games: what the engine does and where it runs; verification notes, dates and internals go to
`docs/` and the release notes) and `docs/` (index `docs/README.md`). The daily development log is in `devlog/`.

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

**Current state (2026-11-07, version 0.10.0): stages 1 to 3 and 5 to 10 of `docs/roadmap.md` are in place, stage 4
for TrailForge only, stage 11 (full ray tracing) and stage 12 (area lights and anisotropic GGX) except their runs on
the iPhone 18 Pro Max and the checks by eye; the plan the user left (stages 6 to 10) and stages 11 and 12 have been
worked through, apart from the remaining work listed at the end of this paragraph, the stage 11 and 12 runs on the
iPhone 18 Pro Max and the checks by eye and ear:
the CMake
skeleton and presets,
`quench_core` (`Result`, `QUENCH_ASSERT`, `quench::log`, `FixedStepClock`, JSON, `quench::fs` with its standard
library backend, worker thread priority, the job system `quench::JobSystem` on enkiTS), `quench_physics`
(`PhysicsWorld` on Jolt, its jobs on the engine's job system), `quench_assets` (asset IDs, the `AssetManager` cache
with reference counts, placeholders, budgets, loads on jobs, a per-frame upload budget, hot reload on the Mac and,
in development builds, on a device through an asset overlay in the user directory filled by `tools/push_assets.sh`
(checked on the iPhone 17), streaming by distance; KTX2 textures (also Basis Universal ETC1S and UASTC, transcoded
on the loading job to BC7, ASTC 4x4, ETC2 or RGBA8 by `gpu::Caps`; ETC2 on the native backend only; not yet run on
an iPhone) and cooked `.qmodel` models, version 4 with skins, blend shapes and MikkTSpace tangents in a
64-byte vertex), `quench_cook` (the host
tool: glTF, GLB, PNG, JPEG, KTX2 to BC7, ASTC 4x4 (iOS builds, through astc-encoder), RGBA8 or, as an option, Basis
Universal KTX2 with mips and meshoptimizer models with tangents and blend shapes; sounds; skeletons and clips
(with morph target weights) from glTF, clips retargeted onto another skeleton by a bone map; `quench_cook_assets()`), the shader build (`quench_shaderc`, `quench_add_shaders()`, compute entries,
ray query entries through DXC built from source, a manifest per set for shader hot reload) and the engine shader
library in `shaders/quench/` (uniform structs
shared by C++ and HLSL), `quench_gpu` (the GPU layer over SDL GPU, with swapchain compositions for HDR and compute;
shader hot reload on desktops with `--hot-reload`, pipelines created again behind their handles, checked on the
Mac's three backends;
beside it, the default on Apple platforms since 2026-11-07 (SDL GPU with `--gpu-backend sdl`, and where the native
one fails to start), a native Metal backend with GPU timestamps per pass, acceleration
structures with top-level refits and compaction, ray queries, and MetalFX upscaling and denoising), `quench_app`
and `quench_main` (SDL main callbacks,
the game interface, input actions, ImGui, common flags (`--orientation`, `--user-dir` among them), offscreen
automated runs (no window on the screen, no input from real devices, `--onscreen` to show one), scenarios with
parts (`include`) and points in window points, percent or names a game gives (`Game::scenarioPoint`), HDR display
output (`--hdr auto|on|off`, an HDR tone curve up to the display's headroom, read every frame on Apple platforms; the UI at paper
white; SDR captures), captures, perf regions a game starts (`App::beginPerf` / `endPerf`), the process's memory
footprint, the system's thermal state (read on Apple platforms) and a game's opt-in reaction to it
(`App::thermalGovernor()`: frame rate and render scale limits with hysteresis, never above the player's; a fake
state through `--thermal-state` and the scenario command `thermal_state`; checked offscreen only, not on a hot
phone), a frame rate limit (`App::setMaxFrameRate`,
`--max-frame-rate`), a relaunch with other arguments on desktops), `quench_render` (the
retained scene: meshes, textures, materials, instances and lights by handle; the standard frame of cascaded sun
shadows (cut-out shadows with holes, translucent tinted shadows through a transmittance map or dithered), the HDR
pass with reversed depth, sky and atmosphere, height fog, normal and emissive maps, bloom, ACES or AgX tone mapping
and TAA or FXAA, with fallbacks by device capability; where `gpu::Caps` allow it, ray-traced sun shadows and ambient
occlusion and MetalFX upscaling, and since stage 11 ray-traced shadows of up to 4 local lights, reflections,
indirect light by a probe grid (DDGI-like; on request traced in turns, in cascades, with probes moved out of geometry
and left out in the open) or per-pixel rays, the engine's temporal denoiser (the default; a spatial
one), MetalFX's denoising upscaler and a reference path tracer, falling back to shadow maps, no occlusion (or, as an
option, screen-space GTAO),
unshadowed local lights (or, as an option, shadow maps for point, spot and area lights), the sky's reflection (or, as an option, screen-space reflections) and ambient light and bilinear scaling; since stage 12 area lights
(rect, disk, tube: LTC shading, soft traced shadows, in the path tracer; unshadowed as the fallback, or, as an
option, soft shadows from shadow maps read in screen space) and anisotropic
GGX; game passes and surface
shaders (with a G-buffer entry for the rays and, through `quench_add_ray_hits()`, their own shading in ray hits); GPU skinning by compute (linear or dual quaternion; blend shapes before it; unchanged poses copied); `SceneAssets` puts asset textures and models
into the scene),
`quench_audio` (headless: a float stereo 48 kHz mixer, buses with volume, mute, pause and meters, reverb buses with
per-sound sends, sounds as an asset
type (WAV, FLAC, MP3, Ogg Vorbis), streamed music with crossfades, game-generated sources, 3D sound with Doppler and voice stealing,
panned or through an HRTF for headphones (off by default), WAV writing
and signal analysis), `quench_audio_device` (output through miniaudio, the iOS audio session; `App::audio()`,
`--no-audio`, `--audio-device`, `--audio-capture`, the scenario command `audio_measure`), `quench_animation`
(headless, on ozz-animation: skeletons, clips and graphs as assets, blend trees, 2D blend spaces, JSON state machines with
cross-fades, events, root motion, masked and additive layers, float channels in clips for blend shape weights, two-bone and aim IK, look-at, foot placement, bone
attachments, the update in parallel on the job system, animation LOD by screen size)
and `quench_add_game()` (macOS executables, iOS bundles,
cooked assets, sounds, skeletons and clips).
`examples/hello` is the minimal sample, `examples/demo` the engine demo (finished in stage 10: a section per feature,
each in its own file with a scripted check and golden captures of the scene, checked in one batch run per GPU
backend; one panel layout and one camera control for every
section; a wide layout and a compact one for phones with touch-sized controls; a tour, a check mode over the checks
waiting for a person (`checks.json` in the user directory, kept across launches) and a benchmark of every section
(`benchmark.json`); About, versioned settings; `tools/package_demo.sh` packages it into `dist/`); `templates/game` and
`tools/new_game.py` start a new game. Tests run on the Mac (Metal, and Vulkan through MoltenVK), in the iOS Simulator,
on the user's iPhones (one signed test app; signed by the user's paid team; on both backends, 2026-11-06: the stage
10 code on the iPhone 18 Pro Max and the iPhone 17, the stage 11 code on the iPhone 17) and, for the headless modules, as WebAssembly under Node.js. HDR output was looked at on the
iPhone (the user, 2026-11-05: "everything is fine"), never on a Mac display (this Mac's has no HDR range). Audio has
run only on miniaudio's null backend and offline (mix checks by measurement); nobody has listened to it yet.
Animation is checked by numbers and still captures; nobody has looked at it in motion yet. The native Metal backend,
ray tracing and MetalFX ran on the Mac and on both iPhones (2026-11-06: the device tests, the demo's benchmarks at
120 Hz; ray-traced shadows with occlusion 8.0 ms GPU p50 on the iPhone 18 Pro Max, over the phone's 7.5 ms budget,
about 100 frames per second on the iPhone 17), the native
backend also in the iOS Simulator (without ray tracing or MetalFX); builds of the Xcode generator compiled its
Objective-C++ without ARC until 2026-11-06, so the 0.8.0 iOS package crashes on the native backend (SDL GPU, that
package's default, works there). The stage 11 effects (0.9.0) ran on the Mac and on the iPhone 17 (2026-11-06: the device
tests on both backends, 0 failed; the cost of each effect, about 3 times the M4 Pro's; the demo's ray tracing
section defaults on phones chosen from them: half size with MetalFX spatial, no probe grid, 60 Hz), not on the
iPhone 18 Pro Max; the user played with that section at its phone defaults on the iPhone 17 (2026-11-06: "it's
fine"), nobody has looked at the other effects in motion. The default backend on Apple platforms is the native one
since 2026-11-07 (the user's decision; checked on the Mac, in the iOS Simulator and on the iPhone 17, not yet on
the iPhone 18 Pro Max or by eye on a phone), SDL GPU
elsewhere; the default stays shadow maps, every stage 11 effect off, until the user has looked at them on the
iPhone. The stage 12 area lights (the demo's
tunnel, whose lamps move and whose camera drives a loop since the same day) ran on the Mac and on the iPhone 17
(2026-11-06: costs and captures, still and in motion; the tunnel's phone defaults at 60 Hz; ten minutes of the ray
tracing section's and the tunnel's phone defaults, where the tunnel missed a few intervals after about 8 minutes),
not on the iPhone 18 Pro Max, and nobody has looked at them in motion or on a phone's screen. The demo's compact layout and touch controls were
looked at in the iOS Simulator and, in the ray tracing section, used by the user on the iPhone 17. The checks waiting for a person (on the iPhone, by ear, in motion) are the demo's check mode
(`docs/demo.md`, "Check mode on the iPhone"). TrailForge builds on the engine's
`quench_core` (`FixedStepClock`, JSON), `quench_physics` and shader build, renders its own passes through
`quench_gpu` (not through `quench_render`),
and runs on `quench_main` / `quench_app` (the engine's window, loop, flags, scenario runner and captures; pinned
`d708783`, not yet moved to a release) with its own input layer fed through `Game::event` and its own physics clock;
Ashfall is not being migrated for now (user decision, 2026-11-05) and still uses its own code;
`tools/check_games.sh` builds both games against this checkout and starts them; neither game uses the job system,
the asset system, the engine's audio or animation yet. No browser audio (stage 7 remaining
work)
yet; what comes next is listed in `docs/roadmap.md`, "What is next".**
`docs/migration.md` describes what exists in the two games; `docs/app.md`, `docs/render.md`, `docs/assets.md`,
`docs/audio.md`, `docs/animation.md`, `docs/demo.md`, `docs/performance.md` and `docs/platforms.md` describe what is
built, `docs/architecture.md` the design with the native backend and ray tracing as built; the rest of `docs/` is
design and plan beyond stage 12. Update this paragraph whenever that changes.

The stack: SDL3 (window, input), SDL GPU (Metal on Apple platforms) and Metal directly (the native backend, MetalFX)
with one HLSL shader source compiled at build time (glslang; DXC, built from source, for ray queries), Jolt Physics,
Dear ImGui (diagnostics), GLM, enkiTS (jobs), xxHash (asset IDs), meshoptimizer, bc7enc_rdo's BC7 decoder,
mikktspace and Basis Universal's transcoder (assets), miniaudio (sound decoding, Vorbis through its stb_vorbis, the reverb through its verblib, and device output),
the SADIE HRTF set (from Resonance Audio's repository, reduced to a table in `quench_audio`), ozz-animation (the animation runtime and the offline library
that builds skeletons and clips); the cooker adds fastgltf, stb_image, astc-encoder and Basis Universal's encoder at
build time. Pins, planned additions and
their licences: `docs/dependencies.md`.

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
startup). The first configure of every preset downloads the pinned dependencies; the first configure with shaders on
a machine also builds the pinned DXC from source for the ray query shaders, once per machine (about 2 minutes on the
M4 Pro, about 365 MB of scratch space while it builds, 27 MB kept in `~/Library/Caches/Quench`; do not delete that
cache to save space, the next configure builds it again; `docs/dependencies.md`, "Added in stage 9").

```sh
tools/check.sh quick              # a small change: format, incremental release build, ctest of the touched labels
tools/check.sh full               # shared foundations, the end of a stage; tools/check.sh release adds the package
cmake --preset debug && cmake --build --preset debug && ctest --preset debug -j8   # also release, asan, tsan
cmake --preset headless && cmake --build --preset headless && ctest --preset headless -j8   # no SDL, no GPU
tools/format.sh --check            # tools/format.sh without arguments formats in place
# iOS: the shaders and assets are compiled by a macOS quench_shaderc and quench_cook from the host-tools preset
cmake --preset host-tools && cmake --build --preset host-tools
cmake --preset ios-sim-release && cmake --build --preset ios-sim-release        # or ios-device-release (libraries)
# Tests in the iOS Simulator, run through `xcrun simctl spawn booted` (build and ctest need a booted Simulator;
# `simctl boot` starts it without the Simulator app's window)
xcrun simctl boot "iPhone 18 Pro Max"
cmake --preset ios-sim-tests && cmake --build --preset ios-sim-tests && ctest --preset ios-sim-tests -j4
# Web check: headless tests as WebAssembly under Node.js, single-threaded (needs host-tools and Homebrew's
# emscripten; the preset points EMSDK_PYTHON at Homebrew's python3, Xcode's is too old)
emcmake cmake --preset wasm-node && cmake --build --preset wasm-node && ctest --preset wasm-node -j8
# The sample: flags with --help; the benchmark (docs/performance.md) in Release with vsync, in a window on the screen
build/release/examples/hello/hello --onscreen --frame-size 1920x1080 \
    --script examples/hello/scenarios/benchmark.txt --audio-device null
# iPhone: an Xcode project with Automatic signing; install and launch with devicectl (docs/app.md, "iOS runs").
# The user's paid team (Xcode's accounts) with plain bundle ids; a free Personal Team: 3 apps, 7-day profiles
cmake --preset ios-xcode -DQUENCH_IOS_TEAM=<team id> -DQUENCH_IOS_BUNDLE_ID_SUFFIX=
xcodebuild -project build/ios-xcode/Quench.xcodeproj -scheme quench_device_tests -configuration Release \
    -destination id=<device udid> -allowProvisioningUpdates build                  # also -scheme quench_hello
# A new game from templates/game, built against this checkout and started once (offscreen)
python3 tools/new_game.py "My Game" ../my-game
# The engine demo (docs/demo.md), one section or the menu; its package for dist/ at the end of a stage
build/release/examples/demo/quench-demo --section shadows   # --gpu-backend metal: the native Metal backend
# Ray tracing and MetalFX options one by one on the native backend (docs/performance.md), on the screen
build/release/examples/demo/quench-demo --onscreen --frame-size 1920x1080 --gpu-backend metal --audio-device null \
    --script examples/demo/scenarios/raytracing_benchmark.txt
tools/package_demo.sh                    # --ios also builds a development-signed .ipa (ios-xcode, not installed)
# The asset cooker (docs/assets.md); games and the demo cook at build time through quench_cook_assets()
build/debug/quench_cook --help
```

Every test case is its own CTest test (`core.json_round_trip`, `physics.physics_sphere_settles`, ...) with its
module's label (`ctest -L animation`; labels: core, physics, assets, audio, animation, cook, gpu, app, render, hello,
demo; the variants on Vulkan through MoltenVK also have `vulkan`). A test executable runs one case by name (`build/debug/tests/quench_core_tests json_round_trip`), all cases
without arguments, and lists them with `--list`. A case that cannot run in a build (threads in the single-threaded
web build) calls `QUENCH_SKIP` and is reported as skipped, not passed.

**Automated runs show nothing on the screen** (user decision, 2026-11-06): app runs with `--script`, `--capture` or
`--quit-after` are offscreen on desktops (a hidden window, frames rendered offscreen, no Dock icon, also as a bundled
app, no focus), take no input from real devices and make no sound (`docs/app.md`, "Offscreen runs"); test
executables create hidden windows only. Never start a run that shows a window (`--onscreen`, the benchmarks,
`QUENCH_WINDOW_TESTS=1`) without telling the user first.

GPU tests (`quench_gpu_device_tests`) render small offscreen targets and check the pixels they read back. On the Mac
every case runs twice: `gpu_device.*` on the default driver (Metal, with the Metal API validation layer,
`MTL_DEBUG_LAYER=1`) and `gpu_vulkan.*` on Vulkan through MoltenVK (registered when Homebrew's `vulkan-loader` is
found; with `vulkan-validationlayers` installed, a validation error or warning in the output fails the case). With
the driver named in `QUENCH_TEST_GPU_DRIVER` (set by CTest) a missing device fails the case instead of
skipping it. On Apple platforms the GPU device and render cases run a third time on the native Metal backend
(`gpu_metal_native.*`, `render_metal_native.*`: `QUENCH_GPU_BACKEND=metal` with `MTL_DEBUG_LAYER=1`; also in
`ios-sim-tests`). The window cases (`window_present`, `window_swapchain_compositions`) need a window on the screen
for a swapchain: they are skipped unless `QUENCH_WINDOW_TESTS=1` is set (never in a default run or in
`tools/check.sh`), skipped in the iOS Simulator test executables (no app lifecycle there), and run in the iPhone test
app, which runs every case as `SDL_main`.

The apps' checks run in batches (`docs/app.md`, "Captures and golden images"): one offscreen process per GPU backend
runs every check of an app as a part of one scenario (`include`), and a CTest test per part checks the part's result
and compares its capture with `tests/golden/<backend>/<device>/<name>.png`. Hello: `hello_batch_<backend>` with
`hello_frame_*` (the frame at 1 s, `hello.png`) and `hello_input_*`; the demo: `demo_batch_<backend>` with
`demo_<part>_<backend>` per section, the layouts, the menu, the mix checks and the switch cycle (`docs/demo.md`,
"Checks"). Backends: `metal` (SDL GPU's Metal driver), `metal_native` (the native Metal backend, compared with the
same `metal` images except `demo_raytracing_native.png`, where ray tracing runs) and `vulkan` (through MoltenVK). The
section captures show the scene without the demo's windows; `demo_ui` and the compact layouts show the UI. Batches
and captures run in Release builds only (`QUENCH_TEST_CAPTURES`); Debug and the sanitizer builds run the unit and
headless tests and one demo smoke run. The renderer's own cases are `render.*` and `render_vulkan.*`.

All checks run locally; there is no CI (user decision, 2026-11-05). The gates are proportional to the change (user
decision, 2026-11-06); `tools/check.sh` runs them, offscreen:

- **A change inside a module, an example or a test (the default): `tools/check.sh quick`.** The format of the
 changed C and C++ files, an incremental build of `release` and `ctest -L` for the labels the changed files map to
 (`tools/check.sh quick demo render` names them; `--preset debug` for assertions), without the variants on Vulkan
 through MoltenVK (`gpu_vulkan.*`, `render_vulkan.*`, `hello_*_vulkan`, `demo_*_vulkan`: label `vulkan`, left out
 with `-LE vulkan`; user decision, 2026-11-06; `full` and `release` run them). Seconds, at most about a minute.
  Look at a capture when its check fails or its golden image changes, not at all of them every time. Add the one step
  that can break where the change can: `tools/check.sh step asan` or `step tsan` (threaded code), `step wasm` (a
  headless module), `step ios` (platform code), `step new-game` (what a game's build uses: `cmake/QuenchGame.cmake`,
  the template, the game skill).
- **Shared foundations (core, the job system and threads, the GPU layer, the app loop, the build and CMake) and the
  end of a stage: `tools/check.sh full`.** format; `debug`, `release`, `asan` and `tsan` (build and ctest each; the
  captures of every backend in `release`); `wasm-node`; `host-tools`, `ios-sim-release` and `ios-sim-tests`; the
  `ios-xcode` project generated into `build/ios-xcode-check` (`step xcode`: configure only, no build, no signing, no
  team; about 11 s, the first time about 4 minutes); `tools/new_game.py` into a temporary directory. `quick` says
  when a change touches a shared foundation, and when it touches other CMake files (then `step xcode`).
- **The end of a stage and a release: `tools/check.sh release`.** `full`, then `tools/package_demo.sh` (it reuses
  the release ctest's demo checks). The stage adds its section to the demo (`examples/demo`, with its part in the
  batches and golden captures for Metal and Vulkan through MoltenVK), and the package goes to `dist/` (user decision,
  2026-11-05): Release, `dist/QuenchDemo-<version>-<shorthash>-macos.zip` with the app, `LICENSE` and
  `THIRD_PARTY_LICENSES`; the script unzips the archive, starts the app once (offscreen) and saves a capture next to
  it (look at it). `--ios` also builds a development-signed `.ipa` into `dist/QuenchDemo-<version>-<shorthash>-ios.zip`
  (`ios-xcode`, signed by a paid team: `--team ID` or `QUENCH_IOS_TEAM`; it refuses a free Personal Team and installs
  nothing). `dist/` is not committed; say in the devlog which files it holds.
- **Code that can behave differently on the device:** also the device test app and the sample on the iPhone
  (`ios-xcode`, `devicectl`) after asking the user, and ask the user to check what is on the screen.
- **Rendering or frame-time-sensitive code:** the benchmark, compared with the numbers before the change (see
  "Verification"); it runs in a window on the screen (`--onscreen`).
- **The games are not built or tested in engine work** (user decision, 2026-11-05 21:34): neither
  `tools/check_games.sh` nor the games' own gates run unless the user asks. The engine is verified by its own tests,
  the samples and the demo. When asked: `tools/check_games.sh` builds every game against the local engine and checks
  only that each one starts, renders and quits with exit code 0 (`[--preset NAME] [trailforge] [ashfall]`; checkouts
  from `TRAILFORGE_DIR` and `ASHFALL_DIR`, default `../trailforge-public` and `../strategy`; each game builds in its
  own `build/quench-check-<preset>`); the games' full gates are TrailForge: `debug` + `release`, `ctest`,
  `tools/scenarios/full_check.txt`; Ashfall: `debug` + `release`, `ctest`, the capture check in its `AGENTS.md`;
  captures compared before and after with `compare_captures.py`. Problems inside a game are not fixed in engine
  work.

## Architecture rules

Details and rationale: `docs/architecture.md`.

- **Designed for any game, not copied from one.** The games are where code comes from, not the model for the engine.
  Code moved from a game is generalized: game concepts (vehicles, levels, combat, fixed material enums, game
  command-line flags) stay in the game, and the engine's names, layout and APIs are designed for any genre. A game
  keeps its behaviour when it switches to engine code (its gates and captures show it); the engine side does not have
  to keep the game's structure.
- **Names.** Namespace `quench`; CMake prefix `QUENCH_`; targets `quench_<module>`; shader entry guards
  `QUENCH_ENTRY_<name>` and `QUENCH_STAGE_VERTEX` / `QUENCH_STAGE_FRAGMENT` / `QUENCH_STAGE_COMPUTE`. Games keep
  their own namespaces and prefixes.
- **Headless core.** Simulation, physics, assets, animation, navigation and the audio mixer must not depend on
  windowing, the GPU or rendering, so they run in CTest without a window (and under Node.js for the web check).
- **Graphics goes through `quench::gpu`.** Backends: SDL GPU (portable, the first one, the default off Apple
  platforms, `--gpu-backend sdl`) and native Metal (since stage 9, `--gpu-backend metal`; the default on Apple
  platforms since 2026-11-07, chosen in `gpu::defaultBackend()` only, falling back to SDL GPU with one log line when
  it fails to start). The Metal backend exists for hardware ray tracing, GPU timestamps and
  MetalFX: SDL GPU does not offer them, and its maintainers decline to expose native object handles
  (libsdl-org/SDL#11147, #12094). SDL GPU types never appear in public engine headers, and games do not call SDL GPU
  directly. A new backend or a native feature is added only for a named need and verified on real hardware.
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
- Texture formats are chosen by device capability: BC7 and ASTC 4x4 cooked per platform, or Basis Universal files
  transcoded at load time to what `gpu::Caps` allow (the way to Android and the web).
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
- **Performance.** The sample's benchmark scenario (`examples/hello/scenarios/benchmark.txt` with `--onscreen`, a
  10 s region in real time, so the frame count follows the display) reports frame, CPU and GPU time (average and
  percentiles; GPU time per frame, per pass later). Budgets per device and how to compare runs: `docs/performance.md`. A change that makes the CPU median or 99th percentile
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
- The agent skill `.agents/skills/quench-game/` says how to build a game on the engine, and `tools/new_game.py`
  copies it into every new game: a change that affects how a game is built (public API, `quench_add_game()` and
  `cmake/QuenchGame.cmake`, the template, `tools/new_game.py`, common flags, scenario commands, asset cooking)
  updates the skill in the same commit. `tools/check.sh step new-game` checks that the paths, flags and names it uses
  still exist (`tools/check_skill.py`) and that a new game gets it.

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
- Do not commit build output, captures or local files (`build/`, `dist/`, `*.bmp`, `*.png` outside `tests/golden/`,
  `docs/images/` (a few small README screenshots) and asset folders, `imgui.ini`, `compile_commands.json`).
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
.agents/skills/        quench-game/: the agent skill on building a game on the engine (SKILL.md, reference.md),
                       copied into every new game by tools/new_game.py
README.md              short overview
LICENSE                MIT licence of the engine; THIRD_PARTY_LICENSES: licences of the libraries it ships
CMakeLists.txt         engine targets; CMakePresets.json: configure, build and test presets
cmake/                 Dependencies (runtime pins), ShaderToolchain (shader compiler pins), CookToolchain (asset
                       cooker pins), QuenchDxc and BuildDxc (DXC for ray query shaders, built from source into a
                       per-machine tool cache), QuenchBasisu (Basis Universal's sources, kept in that cache, and its
                       transcoder), QuenchToolCache (the cache's place), QuenchShaders (quench_add_shaders,
                       quench_add_ray_hits), QuenchCook (quench_cook_assets), EmbedShaders, QuenchTargets (warnings,
                       include directories), QuenchGame (quench_add_game)
                       and Info.ios.plist.in, patches/ (SdlIosSimulator: the SDL GPU patch for the iOS Simulator;
                       MikkTSpaceRotate: a shift by 32 in MikkTSpace masked)
include/quench/        public headers per module: core/, physics/, assets/, audio/, animation/, gpu/, app/, render/
src/                   sources and internal headers per module (src/core/Platform.h: QUENCH_PLATFORM_*;
                       src/core/Jobs.cpp: the job system; src/physics/JoltJobSystem: Jolt's jobs on it;
                       src/assets/: the asset manager, KTX2, Basis Universal (Basis.cpp; BasisTranscoder.cpp and
                       BasisZstd.c build the transcoder), cooked models, tangents, streaming; src/audio/: the mixer,
                       decoders, streams, the HRTF (Hrtf; HrtfTable.inc: the reduced SADIE set), the device and the
                       iOS audio session (AudioSessionApple.m);
                       src/animation/: skeletons, clips, graphs, the character update, IK, the animation system
                       (ozz-animation inside); src/gpu/Backend.h: the backend interface; src/gpu/sdlgpu/: the
                       SDL GPU backend; src/gpu/metal/: the native Metal backend (Objective-C++, ARC; Apple
                       platforms); src/gpu/shaders/: the backends' own shaders (blit); src/app/Main.cpp: quench_main;
                       src/render/: the scene, SceneAssets, the frame modules and their shaders, GPU skinning,
                       RayTracing: acceleration structures, hit data, the probe grid and the traced effects;
                       TemporalFilter: the temporal denoiser; PathTracer: the reference path tracer; ScreenSpace:
                       the screen-space fallbacks (GTAO, after XeGTAO; SSR, after Ashfall's); LtcTable.inc:
                       the area lights' fitted table)
shaders/quench/        the engine shader library (#include "quench/..."): BRDF (also anisotropic), shadows, sky, fog,
                       tone curves, surface shaders, the probe grid, area lights (AreaLights.hlsli: LTC);
                       SceneUniforms.h, the uniform structs shared by C++ and HLSL
examples/hello/        the sample game: lit mesh, ImGui, input and benchmark scenarios, icon
examples/demo/         the engine demo: a menu of sections (rendering features, HDR output, physics, input,
                       assets, audio, animation, ray tracing, the tunnel), one file each in sections/ behind Demo.h's Section
                       interface; the app around them (DemoGame.h: the layouts in Ui.cpp, the camera in Camera.cpp,
                       the tour, check and benchmark modes in Modes.cpp and Checks.cpp, the settings); per section a
                       scenario with a golden capture, scenarios for the menu, the modes, the settings and the
                       switch cycle; smoke and benchmark scenarios (raytracing_benchmark.txt: ray tracing,
                       denoising and MetalFX options and the tunnel; raytracing_phone*.txt, tunnel_phone*.txt: the
                       same on a phone); assets_src/ (procedural sources cooked at build time) and assets/ (files
                       used as they are)
templates/game/        starting point for a new game (copied by tools/new_game.py)
tools/shaderc/         quench_shaderc, the build-time shader compiler
tools/cook/            quench_cook, the build-time asset cooker (glTF, GLB, PNG, JPEG, KTX2 -> KTX2 (also Basis
                       Universal: CookBasis.cpp), .qmodel;
                       glTF, GLB -> .qskel skeletons, .qanim clip sets, also retargeted by a bone map; WAV, FLAC,
                       MP3 checked or converted to WAV)
tools/assets/          make_demo_assets.py: writes the demo's procedural source assets; make_demo_sounds.py: the
                       demo's procedural sounds; make_demo_character.py: the demo's animated mannequin as glTF;
                       make_demo_strider.py: its second character (other names and proportions) and bone map;
                       make_demo_blob.py: the blend shape creature; make_demo_tunnel.py: the tunnel's textures
                       (the last five run at build time)
tools/imagecompare/    quench_image_compare: capture against golden image
tools/icon/            make_icon.py: draws the icons of the sample and the demo
tools/make_ltc_table.py writes src/render/LtcTable.inc from selfshadow/ltc_code's fit (checks its SHA256)
tools/make_hrtf_table.py writes src/audio/HrtfTable.inc from Resonance Audio's SADIE set (checks its SHA256)
tools/new_game.py      creates a game from templates/game, with the quench-game skill
tools/check_skill.py   checks that the paths, flags and names the quench-game skill uses exist (step new-game)
tools/check.sh         the gates: quick (a small change), full, release, single steps
tools/check_games.sh   builds every game against this checkout, starts it, checks exit code 0 and a capture
tools/package_demo.sh  builds the demo into dist/ (macOS .app in a zip; --ios: a development-signed .ipa)
tools/format.sh        clang-format over the C and C++ sources (--check)
tests/                 harness/, core/ (job system included), physics/, assets/ (data/: CC0 Basis Universal test
                       files written by cook.cook_basis_test_data), audio/ (mixer, streams, decoders,
                       device; data/: CC0 test sounds), animation/, cook/ (and CookCli.cmake), gpu/ (shader tests;
                       GPU device tests and their shaders; RayTracingTests: acceleration structures, MetalFX), app/,
                       render/ (scene, scene assets, skinning and frame tests), header check, RunBatch.cmake
                       and CheckPart.cmake (the apps' batch runs and their parts); golden/ (reference captures)
docs/                  documentation (index docs/README.md): architecture, app shell, jobs, assets and the cook,
                       audio, animation, render module, engine demo, performance, platforms, dependencies,
                       migration, roadmap, releases/
docs/images/           the README's screenshots (a few small JPEG files from the demo, each under about 300 KB)
dist/                  demo packages from tools/package_demo.sh (not committed)
devlog/                daily development log (template in devlog/README.md)
.clang-format          code format; .editorconfig for editors
```

Modules and directories that do not exist yet are planned in `docs/architecture.md`.
