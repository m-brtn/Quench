# Roadmap

Stages from the empty repository to ray tracing. Nothing below is implemented unless marked done. The first iteration
agreed with the user covers stages 1 to 3. All documentation: [index](README.md).

## Stage 0: rules and documentation (done 2026-11-05)

`AGENTS.md`, `docs/`, `devlog/`. No code.

## Stage 1: skeleton and shared infrastructure (done 2026-11-05)

- Done: CMake project, presets (`debug`, `release`, `asan`, `tsan`, `host-tools`, `ios-sim-release`,
  `ios-device-release`), pinned dependencies, `THIRD_PARTY_LICENSES`, `-Wall -Wextra` on engine targets with
  `-Werror` when the engine is built on its own.
- Done: code rules in code: `quench::log`, `quench::Result<T>`, `QUENCH_ASSERT`, the header self-containment check,
  `tools/format.sh --check`; exceptions stay enabled ([AGENTS.md](../AGENTS.md#errors-and-assertions)).
- Done: shader toolchain: `quench_shaderc` (from TrailForge, entry guards `QUENCH_ENTRY_<name>`), `#include` with a
  depfile, resource counts in the blob table, `quench_add_shaders()` for engine and game shader sets.
- Done: `quench_core`: `FixedStepClock`, JSON, `quench::fs` (standard library backend), worker thread priority.
- Done: `quench_physics`: `PhysicsWorld` with both games' additions (Apple QoS for Jolt's workers, `tempAllocator()`).
- Done: CTest with headless tests, one CTest test per case.
- Done: the `wasm-node` preset: the headless targets and their tests built with Emscripten and run under Node.js,
  single-threaded (`PhysicsWorld` uses Jolt's `JobSystemSingleThreaded` there).
- Moved: a separate `headless` preset is only useful once non-headless targets exist; it came with stage 2.

## Stage 2: GPU layer (done 2026-11-05; the iPhone run came with stage 3)

- Done: `quench::gpu` over SDL GPU: device, buffers, textures, samplers, pipelines, command recording, uploads,
  downloads (render target captures), mipmap generation, blits, fences, presentation to a window, `gpu::Caps`.
- Done: GPU timing behind the layer (`SdlGpuTimingMetal.m`, isolated until the native Metal backend replaces it).
- Done: `quench_app` with the SDL backends of `quench::fs` (SDL_IOStream) and of the log (SDL_Log); the `headless`
  preset (`QUENCH_HEADLESS`: no SDL).
- Done: GPU tests on the Mac (Metal), in the iOS Simulator (the `ios-sim-tests` preset) and on Vulkan through MoltenVK
  on the Mac, with the Metal API validation layer and the Khronos validation layer.
- Moved to stage 3: the run on the iPhone. A test executable needs a signed app bundle to run on the device, and the
  iOS bundle CMake comes with stage 3; the sample app is checked there.

## Stage 3: app shell (done 2026-11-05, version 0.1.0)

- Done: SDL main callbacks (`quench_main`), window and lifecycle, the game interface, common flags (`--quit-after`,
  `--capture`, `--capture-at`, `--script`, `--window`, `--frame-size`, `--gpu-driver`, `--gpu-debug`,
  `--present-mode`), deterministic scripted runs, scenario runner with scripted input and `perf_begin` / `perf_end`,
  ImGui on `quench::gpu`, input actions (keyboard, mouse, gamepad, touch), iOS bundle and app icon CMake
  (`quench_add_game()`, Ninja and Xcode). Reference: [App shell](app.md).
- Done: the GPU tests and the sample on the iPhone (moved from stage 2): `quench_device_tests` and hello, built with
  the `ios-xcode` preset and run through `devicectl`; what is on the screen was checked by the user.
- Done: `examples/hello`: a lit mesh with ImGui, capture checks against `tests/golden/` (Metal, and Vulkan through
  MoltenVK), an input scenario and a benchmark scenario; first budgets in [Performance](performance.md).
- Done: `templates/game` and `tools/new_game.py`, which creates a game from it, pins the engine and checks that it
  builds and starts.
- Done: version 0.1.0, release notes in [docs/releases/v0.1.0.md](releases/v0.1.0.md).

## Stage 4: games on the engine (done for TrailForge 2026-11-05, version 0.2.0; Ashfall not now)

TrailForge first, then Ashfall: replace their copies of stages 1 to 3 with the engine, following
[Migration](migration.md). The games are not built or tested in engine work unless the user asks (user decision,
2026-11-05); then `tools/check_games.sh` (build against the local engine, start, quit cleanly) and each game's gates.

- Done: `tools/check_games.sh`.
- Done (TrailForge): `PhysicsWorld`, `FixedStepClock`, JSON and the shader build from the engine, pinned with
  FetchContent (2026-11-05; progress table in [Migration](migration.md)).
- Done (TrailForge): the renderer on `quench::gpu`, ImGui through the engine's `ImGuiRenderer`, first with
  TrailForge's own window and loop; pinned to engine `3dfb9f4` (TrailForge `658fd77`, 2026-11-05).
- Done (TrailForge): the app shell (`quench_main`, `quench_app`: SDL main callbacks, the engine's flags, scenario
  runner, captures, perf regions), run on the iPhone; pinned to engine `d708783` (TrailForge `b97883e`, 2026-11-05).
  TrailForge's input stays a game layer fed through `Game::event`, and its physics keeps its own clock inside
  `Game::update`.
- Not now (user decisions, 2026-11-05): TrailForge's physics through `Game::fixedStep` (game work), Ashfall, and
  the input layer generalized from both games.
- Done: version 0.2.0, release notes in [docs/releases/v0.2.0.md](releases/v0.2.0.md). TrailForge stays pinned to
  `d708783` until it moves to `v0.2.0`.

## Stage 5: render modules and the retained scene (done 2026-11-05, version 0.3.0; games not switched)

User decisions for this stage (2026-11-05): engine only (TrailForge and Ashfall are not switched to the modules; their
code may be moved), verified with the engine's tests and the engine demo; no version change. Released afterwards as
0.3.0 at the user's request (2026-11-05, 21:22).

- Done: `quench_render` ([Render module](render.md)): the retained scene (meshes, RGBA8 textures, materials,
  instances, lights, environment, by generation-checked handles); material registration replaces the fixed `MatId`
  enums.
- Done: the standard frame: sun shadow cascades (standard depth), the HDR pass with reversed depth, sky and atmosphere
  (TrailForge's daylight palette), height fog, motion vectors with TAA (TrailForge's) or FXAA, bloom, tone mapping
  (ACES, AgX); game passes at five stages and game surface shaders lit by the engine; fallbacks by device support.
- Done (added by the user to this stage, 2026-11-05): HDR display output with the tone mapping: the swapchain
  composition in `quench::gpu`, `--hdr auto|on|off` and the app API, an HDR tone curve up to the display's headroom,
  the UI at paper white, SDR captures. Checked on the Mac (Metal; Vulkan through MoltenVK) and in the iOS Simulator
  through readbacks and capture peaks only: this Mac's display has no HDR range. On the iPhone: the device test app,
  the demo's checks, the display headroom read every frame (current and potential), and the user's look. Also fixed
  after a user report: the sun disc was darker than its glow at a low sun ([Render module](render.md#sky-and-sun)).
- Done: scenario points in percent of the window or named by the game (`Game::scenarioPoint`), so the demo's checks
  pass at any window size (the portrait iPhone included).
- Done: the engine shader library (`shaders/quench/`) with the shared functions the frame needs (hashes, noise, BRDF
  terms, shadows, sky, fog, tone curves, the fullscreen triangle) and uniform structs shared by C++ and HLSL. The other
  shared functions of the games move when a module needs them.
- Done: render tests (CPU cases and offscreen renders, on Metal and on Vulkan through MoltenVK), the engine demo with
  one section per rendering feature (stage 10), its golden captures and `tools/package_demo.sh`.
- Changed: "every module moved with capture comparisons in the game it comes from" became the engine's own captures
  (the games are not switched). Switching TrailForge's renderer to `quench_render` is later work, when the user asks.
- Done later: cut-out shadows of alpha-tested materials and translucent, tinted shadows of alpha-blended ones
  (0.6.0, after the user's report from the iPhone); motion vectors of skinned meshes (stage 8).
- Not done, remaining: Ashfall's screen-space occlusion, bounce light, reflections and sun shafts (optional modules);
  normal maps; motion vectors for other vertex-deformed surfaces (custom vertex shaders); quality
  presets; GPU timing per pass; HDR seen on a Mac HDR display (this Mac's display has no HDR range; on the iPhone the
  user looked at the demo's HDR section, on and off, on 2026-11-05: "everything is fine"); PQ encoding for `Hdr10`
  output; auto exposure; scripted gamepad input in the iOS Simulator (the virtual gamepad does not attach there, so
  the gamepad lines of the demo's and hello's input scenarios fail in the Simulator only).
- Done: version 0.3.0, release notes in [docs/releases/v0.3.0.md](releases/v0.3.0.md).

## Stage 6: assets (done 2026-11-05 except shader hot reload, version 0.4.0; games not switched)

User decisions for this stage (2026-11-05): engine only (the games are not switched, built or tested), verified with
the engine's tests and the demo's new "Assets" section. Described in [Jobs, assets and the cook](assets.md).

- Done: the job system on enkiTS (`quench::JobSystem` in `quench_core`): one pool, tasks with dependencies,
  parallel-for, a wait that runs other jobs, a mode without workers (the single-threaded web build), worker priority
  through `raiseWorkerThreadPriority()`. Jolt's jobs run on it through a `JPH::JobSystemWithBarrier`
  (`PhysicsWorld(JobSystem*)`); the demo's physics capture is byte-identical with Jolt's old pool, with one thread and
  with the job system.
- Done: `quench_assets`: asset IDs (XXH64 of the path) and reference-counted handles (`AssetRef<T>`), a manager with
  a cache, placeholders while loading and on failure (an error and a log line, never a crash), memory budgets per type
  with eviction of unreferenced assets, reads and decodes on jobs, a per-frame GPU upload budget, and a time budget
  per frame for the jobs the frame thread runs when there are no workers. `quench_render`'s `SceneAssets` puts
  textures and models into the retained scene.
- Done: `quench_cook` (host tool, `quench_cook_assets()` and `quench_add_game(... COOKED_ASSETS ...)` in CMake): glTF
  and GLB through fastgltf, PNG and JPEG through stb_image, KTX2 in; cooked models (meshoptimizer: optimized,
  quantized, LODs, encoded streams) and KTX2 textures (BC7 where possible, else RGBA8; mipmaps filtered in linear
  light) out; byte-identical output for any number of workers; a depfile per asset. Generalized from TrailForge's
  `tools/texc` (`TexCore.h`) and `src/core/Ktx2.cpp`. TrailForge's `texc` / `modelc` and Ashfall's Python converters
  stay in the games until they switch.
- Done: asset hot reload on the Mac (with `--hot-reload` or `AssetManager::setHotReload`, a changed file in the asset
  directory reloads in place), checked by replacing files while the demo ran and by rebuilding `quench_demo_assets`.
- Done: streaming by distance (`StreamingGrid`, generalized from TrailForge's `TerrainTiles`): cells load and unload
  around a focus point with hysteresis, nearest first.
- Not done, remaining: shader hot reload; ASTC and Basis Universal for Android and the web; normal and emissive
  textures in the standard surface; an asset server for hot reload on the iPhone; the demo's "Assets" section on the
  iPhone (not run there: running on the device needs the user's approval). Shader hot reload stays open as P1 (see
  the table below); the release went ahead without it (decided without the user, 2026-11-05, as recommended).
- Done: version 0.4.0, release notes in [docs/releases/v0.4.0.md](releases/v0.4.0.md).

## Stage 7: audio (done 2026-11-06 except the checks on the iPhone and by ear, version 0.5.0; games not switched)

Requested as P0 by the user (2026-11-05), after stage 6 so that sound files load through the asset system and decode
on the job system. Written for the engine; TrailForge's miniaudio device and offline capture were read, its mixer is
game-specific and was not moved (both games keep their own audio until the user asks to switch them). Built while
the user was away: the decisions are in the devlog of 2026-11-06 ("decided without the user"). Described in
[Audio](audio.md).

- Done: `quench_audio`, a headless mixer: 32-bit float stereo at 48 kHz, voices resampled per voice, a bus tree
  with volume, mute, pause and meters, sounds as an asset type (decoded on jobs), streamed music with crossfades,
  generators and a PCM stream the game feeds, click-free parameter changes, 3D sound (one listener, four
  attenuation models, constant-power panning, Doppler) and a voice limit with priorities and stealing. Tested in
  CTest (also under the sanitizers), in the iOS Simulator and under Node.js.
- Done: device output through miniaudio (`quench_audio_device`, pinned at TrailForge's commit), with the audio
  callback thread as the one documented exception to the job-system rule; the iOS audio session (category,
  activation, interruptions, route changes; of these only setting the category ran, in the iOS Simulator), suspend
  and resume with the app lifecycle.
- Done: `App::audio()`, `--no-audio`, `--audio-device NAME`, `--audio-capture FILE.wav` (offline mixing on the
  frame clock, deterministic in scripted runs), the scenario command `audio_measure` (RMS per channel, peak, the
  strongest frequency) and the `audio_*` values; `quench_cook sound` and `quench_cook_assets(... SOUNDS ...)`.
- Done: the demo's "Audio" section with its scripted check (39 checks on the measured mix) and golden captures for
  Metal and Vulkan through MoltenVK.
- Not done, remaining: listening (the Mac's output, headphones, the iPhone), the iPhone run of the stage 7 code
  (silent switch, interruptions, route changes, background and foreground), browser output (Web Audio), HRTF,
  occlusion, reverb and filters, virtual voices, Vorbis or Opus, gapless MP3 (encoder delay and padding are not
  trimmed). The release went ahead before the checks by ear and on the iPhone (decided without the user,
  2026-11-06, as recommended); they are listed as not verified in the release notes.
- Done: version 0.5.0, release notes in [docs/releases/v0.5.0.md](releases/v0.5.0.md).

## Stage 8: animation (done 2026-11-06 except retargeting and the checks in motion and on the iPhone, version 0.6.0)

Requested as P0 (skinning, state machines, blending, events) and P1 (partial and additive blending, IK and foot
placement, retargeting, attachments) by the user (2026-11-05). Written for the engine: neither game has skeletal
animation to move. Built while the user was away: the decisions are in the devlog of 2026-11-06 ("decided without
the user"). Described in [Animation](animation.md).

- Done: `quench_animation` on ozz-animation 0.17.0, headless (tested in CTest, under the sanitizers, in the iOS
  Simulator and under Node.js): skeletons, clip sets and graphs as asset types (the rest pose and a log line while
  loading and after a failure; budgets), sampling and 1D blend trees with phase sync, JSON state machines (layers,
  conditions, triggers, exit times, cross-fades), masked and additive layers, events (in order, across loops, with
  large steps), root motion, two-bone and aim IK, foot placement on a game's ground query, look-at, bone attachments
  (`jointWorld`), the update of all characters in parallel on the job system (also with zero workers).
- Done: cooking in `quench_cook` (`skeleton`, `animation`, skins in `.qmodel` version 2; clip settings with loop,
  additive, events and root motion; deterministic) and `quench_cook_assets(... SKELETONS ... ANIMATIONS ...)`.
- Done: compute in `quench::gpu` and `quench_shaderc`; GPU skinning in `quench_render`'s retained scene (a joint
  palette per instance, a compute pass, motion vectors from the previous pose, culling bounds from the palette,
  `RenderStats` with the skinning GPU time), through `SceneAssets` for cooked models.
- Done: the demo's "Animation" section (speed blend, jump and wave, additive lean, look-at, a held torch, footstep
  sounds, foot placement on a ramp and a step, a crowd of up to 512 with its update and skinning times) with its
  scripted check and golden captures for Metal and Vulkan through MoltenVK; a benchmark region with 256 characters.
- Not done, remaining: retargeting (clips bind to the skeleton they were cooked for), 2D blend spaces, animation
  LOD, dual quaternion skinning, blend shapes; the check in motion by the user (foot sliding, blend pops, IK) and on
  the iPhone. The release went ahead before those checks (decided without the user, 2026-11-06, as recommended);
  the release notes list them as not verified.
- Done: version 0.6.0 (with the cut-out and translucent shadows), release notes in
  [docs/releases/v0.6.0.md](releases/v0.6.0.md).

## Stage 9: native Metal backend and ray tracing (done 2026-11-06 except reflections and indirect light, 0.7.0)

- Metal backend of `quench::gpu` (pixel comparison against the SDL GPU backend on the same Mac), GPU timestamps.
- Acceleration structures from the retained scene; ray-traced shadows first, then reflections, ambient occlusion and
  indirect light, each as a fallback slot. MetalFX upscaling.
- Shader path for ray queries decided by a test first ([Architecture](architecture.md#shaders)).

Where ray tracing runs (discussed with the user, 2026-11-05):

- At first only on the Metal backend: the Mac (M4 Pro) and the iPhone (A20 Pro) are the hardware it can be verified
  on. SDL GPU has no ray tracing and does not expose native handles (libsdl-org/SDL#11147, #12094).
- Every ray-traced effect is a slot with a fallback chosen by `gpu::Caps`, never by platform: shadow maps for shadows,
  screen-space reflections, screen-space or no ambient occlusion and indirect light. The SDL GPU backends (Vulkan
  through MoltenVK today; D3D12, Vulkan on Windows, Linux and Android, the web later) run the same game with the
  fallbacks.
- Later, only for a named need and verified on real hardware: a native D3D12 backend (DXR) on Windows, or a native
  Vulkan backend with the ray tracing extensions on Windows, Linux and the Android devices that have them. WebGPU has
  no ray tracing. Acceleration structures come from the retained scene and the shaders are HLSL (compiled to MSL,
  SPIR-V and DXIL), so games do not change when a backend gains ray tracing.

TODO for stage 9:

- [x] Check in MoltenVK's source whether it exposes any Vulkan ray tracing extension (`VK_KHR_acceleration_structure`,
  `VK_KHR_ray_query`, `VK_KHR_ray_tracing_pipeline`). Checked 2026-11-06: no. MoltenVK's extension list,
  `MoltenVK/MoltenVK/Layers/MVKExtensions.def`, has none of the three, neither at tag `v1.4.2` (commit `db66022`,
  the version Homebrew installs here; 207 lines) nor on `main` at `a814250` (2026-10-03). Only prerequisites are
  there: `KHR_buffer_device_address` (line 47) and `KHR_deferred_host_operations` (line 52) at `v1.4.2`. The
  installed `libMoltenVK.dylib` contains no `VK_KHR_ray_*` or `VK_KHR_acceleration_structure` string either. Vulkan
  through MoltenVK therefore always takes the fallbacks.
- [x] Test the ray query shader path before writing ray tracing shaders (2026-11-06; details in
  [Architecture](architecture.md#shaders)). glslang's HLSL front end cannot: it fails on
  `RaytracingAccelerationStructure`. DXC can (SPIR-V and DXIL), and the pinned SPIRV-Cross turns DXC's SPIR-V into
  MSL that runs: a kernel with one `RayQuery` hit a triangle at t = 1.0 on the M4 Pro under the Metal API validation
  layer. Not kept as a CTest: the build has no DXC on macOS yet.
- [x] Design the `quench::gpu` surface for acceleration structures (one bottom level per registered mesh, a top level
  per frame from the scene's instances, refit or rebuild) in [Architecture](architecture.md#ray-tracing), with
  the fallback slots (2026-11-06; built the same day, below).
- [x] Native Metal backend of `quench::gpu` (`src/gpu/metal/`, selected with `--gpu-backend metal`; SDL GPU stays the
  default) and GPU timestamps per pass (`Caps::gpuPassTiming`), 2026-11-06 ([Architecture](architecture.md#graphics-layer)).
- [x] DXC for the shader entries with ray queries (2026-11-06): v1.9.2609 built from its pinned source archives
  once per machine (macOS has no DXC release binary; not the LunarG SDK); glslang stays for everything else
  ([Architecture](architecture.md#shaders)).
- [x] Acceleration structures in `quench::gpu` on the native Metal backend (`Caps::rayTracing`): a bottom level per
  mesh, a top level per frame, refits for skinned instances (2026-11-06).
- [x] Ray-traced sun shadows (fallback: shadow maps) with cut-out holes, translucent tints and posed skinned casters,
  and ray-traced ambient occlusion (fallback: none) as `quench_render` slots (2026-11-06,
  [Render module](render.md#ray-traced-shadows-and-ambient-occlusion)).
- [x] MetalFX upscaling, spatial and temporal, on the native Metal backend; the temporal one replaces TAA's resolve;
  fallback bilinear with TAA or FXAA (2026-11-06, [Render module](render.md#upscaling)).
- [x] Per-pass GPU timing off unless something reads it (`Device::setPassTiming`, `App::requestPassTiming`,
  `--gpu-pass-timing`); the native backend's CPU time without it checked against SDL GPU (2026-11-06,
  [Performance](performance.md#measurements)).
- [x] Demo section 11, "Ray tracing", with checks and golden images for the native backend and the fallbacks
  (2026-11-06, [Engine demo](demo.md)).
- [ ] Measure ray-traced shadows against shadow maps on the iPhone at 120 Hz and pick the default per device (the
  default stays shadow maps and SDL GPU until then).
- [ ] Run the native backend on the iPhone (the device test app with `QUENCH_GPU_BACKEND=metal`, the demo with EDR
  and at 120 Hz, timestamps on the A20 Pro, `Caps::rayTracing` and MetalFX on the device), then make it the default
  on Apple platforms.
- [ ] Look at the ray tracing section in motion (noise and ghosting with 1 and 2 shadow rays, with MetalFX
  temporal): by the user, on the Mac and the iPhone.
- [x] Ray-traced reflections (fallback: what exists, the sky reflection) and indirect light: done in stage 11 (below).
- [x] Bottom-level compaction and top-level refits: done in stage 11 (below).
- [x] Version 0.7.0, release notes in [docs/releases/v0.7.0.md](releases/v0.7.0.md). Released before the iPhone
  run and before anyone watched the section in motion (decided without the user, 2026-11-06, as recommended); the
  release notes list both under "What was not verified".

## Stage 10: engine demo (done 2026-11-06 except the iPhone run, version 0.8.0)

Requested by the user (2026-11-05). Started in stage 5 (user decision, 2026-11-05; it replaces the planned
`examples/scene`): every stage adds its section, with a scripted check and golden captures, and ends with a packaged
build of the demo in `dist/` (`tools/package_demo.sh`); stage 10 finishes and polishes it. Sections so far: materials
and lights, shadows, sky and fog, post-processing, HDR output, physics, input, assets, audio, animation, ray tracing
([Engine demo](demo.md)).

- One app built with the engine (`examples/demo`, "Quench Demo") for the Mac and iOS: a menu of sections, each a
  small interactive scene of primitives or procedural content (CC0 assets only where a feature needs real content,
  such as an animated character) showing one area of the engine with ImGui controls: rendering (scene, materials,
  lights, shadows, sky, fog, post-processing, anti-aliasing), physics, input (keyboard, mouse, gamepad, touch),
  audio (3D sound, buses, music), assets (asynchronous loading, hot reload, streaming), animation (blending, state
  machines, IK), ray tracing with its fallbacks, and the frame, CPU and GPU times.
- Every section has a scripted check and a golden capture, so the demo is also a test of the engine.

Finished on 2026-11-06 and released as 0.8.0 (Mac and iOS Simulator only, the iPhone was away):

- [x] A coverage audit in [Engine demo](demo.md#coverage), with the gaps that mattered filled: the job pool's
  activity and a job test, hot reload and streaming made visible (a reload note, the streaming rings), memory and
  GPU resources, section load and switch times, a frame-time graph, and the backend with every fallback and its
  reason (`Renderer::fallbacks()`).
- [x] One panel layout for every section (title, description, controls, stats, frame and fallbacks, reset buttons)
  and the same camera on mouse and keyboard, gamepad and touch (one finger orbits, two pan and pinch); the menu by
  keyboard and gamepad.
- [x] Phones: sizes by display density, the safe area, a compact layout (a bar and one sheet at a time, docked at
  the bottom in portrait and on the right in landscape), touch-sized controls. Looked at in the iOS Simulator
  (iPhone 18 Pro Max, portrait and landscape); not on the iPhone.
- [x] Modes: a check mode over the devlog's checks waiting for the user (OK, problem or skip with a note; results as
  JSON in the user directory), a tour and a benchmark of every section into one JSON report; flags in
  [Engine demo](demo.md#flags).
- [x] An About panel (version, build, commit, backend, device, the licences, the controls), settings as versioned JSON
  with a fallback to the defaults for a corrupt file (`--no-settings`; scripted runs ignore them), sections made when
  first shown and switched without growth (checked by `demo_cycle`), a relaunch with the other GPU backend on the Mac.
- [x] Each section in its own file behind one interface (`examples/demo/sections/`); the scenes render the same as
  before (every capture with the windows hidden identical to the stage 9 demo's, Metal and Vulkan through MoltenVK).
- [x] Engine additions the demo needed, for any game: perf regions a game starts (`App::beginPerf` / `endPerf`),
  the memory footprint and live GPU resources as scenario values, a relaunch with other arguments on desktops,
  `--orientation` and `--user-dir`, `Renderer::fallbacks()`, `gpu::backendAvailable()`,
  `quench_cook_assets(... FILES ...)`; Mac trackpad touches are no longer fingers.
- [ ] The iPhone: the check mode, the layouts and the touch controls on the device (by the user).
- [x] Version 0.8.0, release notes in [docs/releases/v0.8.0.md](releases/v0.8.0.md). Released before the iPhone run
  (decided without the user, 2026-11-06, as recommended); the release notes list it under "What was not verified".

## Stage 11: full ray tracing (done 2026-11-06 except the checks by eye and the iPhone 18 Pro Max, version 0.9.0)

Requested by the user on 2026-11-06. Order: foundations, the reference path tracer and the temporal denoiser come
before reflections and indirect light (1-sample effects need temporal accumulation, and the path tracer is their
numeric reference). New effects stay off by default until measured on the user's iPhone. Fallbacks are chosen by
`gpu::Caps`, never by platform. User decisions (2026-11-06): plan the iPhone around render scale 0.67 or 0.5 with
MetalFX; MikkTSpace tangents in the vertices; the engine's own temporal denoiser plus the MetalFX temporal denoised
scaler.

- [x] iPhone baseline (the user, 2026-11-06): the native backend and the stage 9 ray tracing section on the device;
  ray-traced shadows and occlusion at the native 1320x2868 cost 8.02 ms GPU p50 (over the 7.5 ms budget); MetalFX
  temporal within 120 Hz at scale 0.67 and 0.5; the MetalFX temporal denoised scaler is supported on the A20 Pro
  ([Performance](performance.md#measurements)).
- [x] Normal and emissive maps in the standard surface (slots 2 and 3) with MikkTSpace tangents in the vertex
  (`.qmodel` version 3, a 64-byte vertex; the cook generates tangents where a glTF has none; `computeTangents`
  for procedural meshes); `SceneAssets` maps both textures (2026-11-06).
- [x] G-buffer prepass when ray tracing needs it (normal + roughness, base color + metallic, from a surface's
  `surface_gbuffer_fs` or the standard surface); motion vectors in the prepass for the temporal denoiser and MetalFX
  (2026-11-06, [Render module](render.md#ray-traced-lighting)).
- [x] Hit material access for every instance (`RayHit.hlsli`): the triangle, the material's values and its base
  color, metallic-roughness and emissive textures in 256 x 256 texture arrays (no normal maps in hits).
- [x] Counter-clockwise front faces through mirroring transforms in the Metal backend, with a test
  (`ray_query_front_faces`).
- [x] Top-level refit when only transforms change, bottom-level compaction, `rayMemoryBytes`, `rayCompactions`,
  `rayTopRefit` (2026-11-06).
- [x] Split the `accel` timing into `blas` / `tlas` (after stage 12, 2026-11-06): ranges inside `accel`, the bottom
  levels' builds, refits and compactions, and the top level's build or refit.
- [x] Reference path tracer: progressive, next event estimation toward the sun and one local light, furnace test,
  restarts on any change including a new skinned pose ([Render module](render.md#path-tracer)).
- [x] Temporal ray denoiser (SVGF-like without the variance estimate, with anti-lag; `RenderSettings::rayDenoiser`,
  the default) on every traced signal ([Render module](render.md#denoising)).
- [x] Ray-traced shadows for up to 4 local lights (`localLightShadows`, `LightDesc::sourceRadius`); fallback
  unshadowed (spot and point shadow maps later, open).
- [x] Ray-traced reflections with hit lighting and a roughness cut-off; sky fallback (no SSR this stage).
- [x] Indirect light: DDGI-like probes (16 x 8 x 16, 64 rays each per frame), then per-pixel rays; sky ambient
  fallback; furnace test against the sky's light.
- [x] MetalFX temporal denoised scaler (`Caps::denoisedUpscaling`, `UpscalerType::TemporalDenoised`,
  `Upscaling::TemporalDenoised`); exposure passed to the MetalFX temporal scalers.
- [x] Demo: the ray tracing scene grew (mirror, roughness strip, colored corner, emissive panel, shadowed lamps), the
  Graphics panel's "Ray-traced lighting" options, scenarios and golden captures (native, the fallbacks on Metal and
  on Vulkan through MoltenVK), benchmark regions per effect, five check mode items (29 in all).
- [x] Per-pass GPU costs at 1080p on the M4 Pro in `docs/performance.md`.
- [x] The same on an iPhone, the device test app on both backends with the stage 11 code, and the phone defaults
  chosen from them (2026-11-06, on the iPhone 17 (A19); [Performance](performance.md#stage-11-on-the-iphone)): the
  device tests 215 cases on each backend, 0 failed; every effect costs 2.8 to 3.1 times the M4 Pro's 1080p frame at
  the phone's 1.5 times the pixels, everything 34.4 ms at the native size; the ray tracing section's defaults on
  phones and tablets that trace (`App::mobile()`): sun and lamp shadows, occlusion and reflections without the probe
  grid, render scale 0.5 (not 0.67) with MetalFX spatial (temporal costs three times as much there), at 60 Hz
  (held for a minute at the nominal thermal state; at the display's rate 117 frames per second cold, fair after
  34 s). The iPhone 18 Pro Max has not run the stage 11 code.
- [x] The system's thermal state (`app::ThermalState`: frame stats, perf lines, the scenario value `thermal_state`,
  the demo's Performance panel, About and `benchmark.json`) and a frame rate limit (`App::setMaxFrameRate`,
  `--max-frame-rate`, the scenario command `max_frame_rate`), 2026-11-06. The engine does not react to the thermal
  state by itself.
- [x] Xcode builds copy re-cooked assets into the app (found on the iPhone: the demo's mannequin stayed in the old
  model format): the merged `assets/` folder is touched when a file in it changes, and `<game>_assets` is built
  first ([App shell](app.md#ios-runs)).
- [ ] The phone defaults on the iPhone 18 Pro Max, a ten-minute run at 60 Hz, and whether that phone should start
  at 120 Hz.
- [ ] Look at the effects in motion (noise, lag of the temporal denoiser, MetalFX denoising, reflections and
  probe light while the sun turns): by the user, on the Mac and the iPhone (the demo's check mode).
- [x] Full test tier, `tools/package_demo.sh` (macOS), release notes [docs/releases/v0.9.0.md](releases/v0.9.0.md);
  after the iPhone 17 runs again with `--ios` (2026-11-06 19:07, the macOS zip and the development-signed `.ipa`).

## Stage 12: area lights and anisotropic GGX (done 2026-11-06 except the checks by eye, no version yet)

Requested by the user on 2026-11-06 (19:20): a futuristic tunnel with lamps of different colors and shapes (strips
in the floor, squares in the ceiling, panels, tubes, spots), metal and wet materials, rubber, embossed plastic and
brushed aluminium. User decisions: real area lights (not emissive geometry with point lights) and an anisotropic
BRDF; own procedural content only. No release (the version stays 0.9.0); "Changes for games" in
[docs/releases/next.md](releases/next.md).

- [x] Area lights in `quench_render`: `LightType::Rect`, `Disk` and `Tube` (`LightDesc::size`, `tangent`,
  `twoSided`), at most 8 of a frame's 16 lights; their shapes drawn in the scene with the light's radiance on their
  emitting faces ([Render module](render.md#area-lights)). Raster shading by linearly transformed cosines (LTC):
  the fitted GGX table and the polygon, disk and line integrals of selfshadow/ltc_code (BSD-2-Clause,
  [Dependencies](dependencies.md)), diffuse and specular, against a CPU integral -2.3 % (rect), -1.2 % (disk),
  -4.7 % (tube, no end caps) on all three backends.
- [x] Soft ray-traced shadows of area lights (one ray per pixel per light toward a point of the shape, through the
  temporal denoiser), in the same 4 channels as the lamp shadows; ray hits shade area lights by the same LTC.
- [x] The path tracer samples area lights by a point of their shape and by the BRDF's rays that hit the shape,
  weighted by multiple importance sampling; within 2.4 % of the integral after 1024 samples.
- [x] Fallback without ray tracing (`gpu::Caps`): the LTC shading without shadows; reflections of the sky.
- [x] Anisotropic GGX (`MaterialDesc::anisotropy`, `anisotropyRotation`, along the vertex tangent) in the raster,
  the ray hits and the path tracer, with the anisotropic visible-normal sampling in the path tracer; a highlight of
  6 x 6 pixels stretches to 58 x 6 at anisotropy 0.8 ([Render module](render.md#anisotropic-ggx)).
- [x] `RenderStats::areaLights`, the scenario value `area_lights`.
- [x] Demo section 12, "Tunnel" ([Engine demo](demo.md#sections)): eight area lights and two spots, metal panels,
  wet asphalt with a puddle, rubber, embossed plastic and brushed aluminium from textures written at build time
  (`tools/assets/make_demo_tunnel.py`); its options in its panel and the Graphics panel; scenarios and golden
  captures (native with ray tracing, the fallback on Metal and on Vulkan through MoltenVK; the path tracer without a
  golden image); benchmark regions; three check mode items (32 in all).
- [x] Costs on the M4 Pro and the iPhone 17, the phone defaults chosen from them (60 Hz on every phone; with ray
  tracing two lamp shadows, no traced reflections, half size with MetalFX spatial; held 40 s at nominal), and the
  cost of the new shader paths in frames that do not use them (5 to 8 % of the M4 Pro's unpaced frame rate;
  [Performance](performance.md#the-tunnel)).
- [x] The tunnel in motion (requested by the user after stage 12, 2026-11-06): the lamps move (the violet tube on
  ceiling rails, the orange tube in its bay, the amber spot's cone turning, the white spot circling the sphere, the
  portal breathing) with their fixtures (instances refitted in the top level), the camera drives a loop through the
  tunnel by default (a drag hands it to the free camera), everything stands still while the path tracer runs;
  scripts set the clock, so captures stay deterministic. `RenderStats::localShadowChanges`. The motion costs the M4
  Pro about 0.05 ms of GPU and CPU time; the iPhone 17 held 60 Hz in motion at the nominal thermal state on both
  backends, after phones without ray tracing went to three quarters of the size
  ([Performance](performance.md#the-tunnel-in-motion)). A check mode item (33 in all).
- [x] Surface shader variants without the area light and anisotropy paths, for frames and materials that do not
  use them (`surface_basic_fs`, `SurfaceDesc::basicFragmentShader`; after stage 12, 2026-11-06): such a frame is
  back to before stage 12 within 0.6% ([Performance](performance.md#the-tunnel)).
- [ ] Look at the tunnel in motion and on the iPhone's screen (the lamps' shading and shadows, the brushed
  aluminium, the puddle): by the user (the demo's check mode).
- [ ] The tunnel on the iPhone 18 Pro Max.

## What is next

The plan the user left (stages 6 to 10) has been worked through with version 0.8.0, stage 11 with 0.9.0, and stage
12 (area lights, anisotropy, the tunnel) after it, not yet released. What stays open, in the order it is useful (nothing of it is scheduled; the user decides):

1. **The checks that need a person**, mostly on the iPhone: the demo's check mode on both GPU backends (33 items:
   the cut-out and tinted shadows, the cascades, the sky and fog, anti-aliasing, HDR on an HDR display, physics, the
   input devices, loading and streaming, the audio by ear, animation in motion, ray tracing noise, MetalFX ghosting
   and the fallbacks, the stage 11 reflections, bounced light, lamp shadows, MetalFX denoising and the path tracer,
   the tunnel's lamps, brushed aluminium and puddle, the tunnel in motion, the frame rate and the layout on the phone;
   [Engine demo](demo.md#check-mode-on-the-iphone)), on the iPhone 18
   Pro Max the device test app on both backends with the 0.9.0 code and the stage 11 ray tracing runs (done on the
   iPhone 17), Core Audio output on the Mac, the audio session on the iPhone (silent switch, interruptions, routes,
   background).
2. **Defaults from those runs:** the ray tracing section's phone defaults are chosen (stage 11, on the iPhone 17);
   open: ray-traced shadows or shadow maps in the other sections, and the native Metal backend as the default on
   Apple platforms (it ran the device tests and the benchmarks on both iPhones on 2026-11-06; the user has not looked
   at it there yet). Recommended, not built: a game-level reaction to the thermal state (opt-in, for example 60 Hz
   or a lower render scale from serious on, with hysteresis and the player's choice first); the engine reports the
   state and limits the frame rate on request, and does not change settings on its own. The probe grid on phones:
   a fraction of the 2048 probes traced per frame instead of all of them (on the A19 they take about 3 ms at any
   render scale).
3. **Remaining work of the stages:** shader hot reload, ASTC or Basis Universal textures and asset hot reload on a
   device (stage 6); browser audio, HRTF, reverb and Vorbis (stage 7); retargeting, 2D blend spaces, animation LOD,
   dual quaternion skinning and blend shapes (stage 8); normal maps
   and custom surfaces in ray hits, shadow maps for spot and point lights and screen-space fallbacks (SSR, GTAO) for
   devices without ray tracing, a denser or cascaded probe grid (stage 11); shadows of area lights without ray
   tracing, normal maps in ray hits (stage 12).
4. **Games on the engine** (stage 4): moving a game's pin to a release, more of a game's code onto the engine's
   modules, the second game's migration; only when the user asks, with each game's own gates.
5. **Known open questions:** per-frame call counts and profiles need a profiler the user has allowed on the Mac. An
   Xcode build of a game still copies a change merged by that build into the app only with the next build (Xcode
   decides when the build starts; probably what left the licence files out of one stage 10 build, not reproduced
   then); building `<game>_assets` first avoids it ([App shell](app.md#ios-runs)). The Xcode build also cooks every
   asset and copies the whole folder in every build (about a second for the demo).
6. Other modules when a game needs them and the later platforms when the user asks (below).

## Other modules (when a game needs them)

Navigation (Recast & Detour: navmesh building, paths, crowds), Tracy profiling, Flecs as an optional ECS module.

## Later platforms

Not scheduled; started only when the user asks. Windows and Linux (SDL GPU D3D12 / Vulkan; need hardware or CI),
Android (Vulkan, ASTC / ETC2 textures through Basis Universal, APK assets), web (needs SDL GPU's WebGPU backend, WGSL
shaders, single-thread jobs, Basis Universal textures). See [Platforms](platforms.md).

## Requested features

Priorities from the user's request of 2026-11-05.

| Feature | Priority | Stage |
|---|---|---|
| Asynchronous loading | P0 | 6 (done) |
| Asset manager and cache | P0 | 6 (done) |
| Asset pipeline (glTF / GLB, PNG, JPEG, KTX2 -> cooked formats) | P0 | 6 (done) |
| Mipmaps and compression | P1 | 2 (GPU mipmaps), 6 (cooked, BC7; done) |
| Hot reload | P1 | 6 (assets done, shaders remaining) |
| Streaming by distance | P1 | 6 (done) |
| Audio: mixer, buses, music, 3D sound, device output (added 2026-11-05) | P0 | 7 (done; not yet run on the iPhone or listened to) |
| Skeletal animation and GPU skinning (ozz) | P0 | 8 (done; not yet looked at in motion or run on the iPhone) |
| Animation state machine | P0 | 8 (done) |
| Blending | P0 | 8 (done) |
| Animation events | P0 | 8 (done) |
| Partial and additive blending | P1 | 8 (done) |
| IK and foot placement | P1 | 8 (done) |
| Retargeting | P1 | 8 (not done) |
| Bone attachments | P1 | 8 (done) |
| Engine demo: interactive sections for every feature (added 2026-11-05) | after the stages | 10 (done; started in 5; not yet run on the iPhone in its finished form) |
| Full ray tracing: reflections, indirect light, local light shadows, denoising, a path tracer (added 2026-11-06) | after the stages | 11 (done; run on the iPhone 17, not yet looked at in motion) |
| Area lights, anisotropic GGX, a demo tunnel with lamps of every shape and metal, wet, rubber and plastic materials (added 2026-11-06) | after stage 11 | 12 (done; run on the iPhone 17, not yet looked at in motion or on the phone's screen) |

The request also asks to verify Metal, Vulkan and browser compatibility separately. Metal: Mac and iPhone. Vulkan:
MoltenVK on the Mac only (a test of the SPIR-V path, not of real Vulkan drivers). Browser: headless modules under
Node.js until SDL GPU has a WebGPU backend.
