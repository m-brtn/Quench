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
[Migration](migration.md). `tools/check_games.sh` (build against the local engine, start, quit cleanly) after each
change; each game's full gates only when the user asks (user decision, 2026-11-05).

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

## Stage 5: render modules and the retained scene

- Retained scene: mesh, instance, material and light registration; material registration replaces the fixed `MatId`
  enums.
- Standard frame modules: shadow maps, sky and atmosphere, fog, bloom, tonemap, anti-aliasing (TrailForge TAA), and
  Ashfall's screen-space occlusion, bounce, reflections and sun shafts as optional modules.
- Engine shader library extracted from the 10 identical and 16 diverged shared shader functions.
- Every module moved with capture comparisons in the game it comes from.

## Stage 6: assets

- Job system on enkiTS (Jolt's jobs included, single-thread mode, Apple QoS).
- Asset IDs, manager, cache, placeholders, memory budgets; asynchronous loading with a per-frame GPU upload budget.
- `quench_cook`: glTF / GLB (fastgltf), PNG, JPEG, KTX2 -> cooked meshes (meshoptimizer) and KTX2 textures (BC7 now,
  Basis Universal later) with mipmaps. Replaces TrailForge's `texc` / `modelc` and Ashfall's Python converters.
- Hot reload on the Mac (assets and shaders); streaming by distance generalized from `TerrainTiles`.

## Stage 7: audio

Requested as P0 by the user (2026-11-05), after stage 6 so that sound files load through the asset system and decode
on the job system. Generalized from the games' own mixers (both games keep theirs until the user asks to switch them).

- `quench_audio`: a headless mixer (voices, buses with volume, sample rate conversion, streamed music, sources whose
  samples the game generates), tested in CTest and under Node.js; 3D sound (listener, distance attenuation, panning,
  Doppler).
- Device output through miniaudio; the iOS audio session (category, interruptions, route changes).
- Capture of the mix to a WAV file for scripted checks (`--audio-capture`), and a no-audio mode.

## Stage 8: animation

- ozz runtime, GPU skinning, data-driven state machines, blending, animation events.
- Then partial and additive blending, IK and foot placement, bone attachments, retargeting.

## Stage 9: native Metal backend and ray tracing

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

- [ ] Check in MoltenVK's source whether it exposes any Vulkan ray tracing extension (`VK_KHR_acceleration_structure`,
  `VK_KHR_ray_query`, `VK_KHR_ray_tracing_pipeline`). Said "no" from memory on 2026-11-05; not verified.
- [ ] Test the ray query shader path before writing ray tracing shaders: HLSL through glslang's HLSL front end to
  SPIR-V, then SPIRV-Cross to MSL; DXC if glslang cannot. Also the DXIL output.
- [ ] Design the `quench::gpu` surface for acceleration structures (one bottom level per registered mesh, a top level
  per frame from the scene's instances, refit or rebuild) in [Architecture](architecture.md), with the fallback slots.
- [ ] Measure ray-traced shadows against shadow maps on the iPhone at 120 Hz and pick the default per device.
- [ ] MetalFX upscaling (spatial and temporal) as an option, and how it combines with the TAA of stage 5.

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
| Asynchronous loading | P0 | 6 |
| Asset manager and cache | P0 | 6 |
| Asset pipeline (glTF / GLB, PNG, JPEG, KTX2 -> cooked formats) | P0 | 6 |
| Mipmaps and compression | P1 | 2 (GPU mipmaps), 6 (cooked) |
| Hot reload | P1 | 6 |
| Streaming by distance | P1 | 6 |
| Audio: mixer, buses, music, 3D sound, device output (added 2026-11-05) | P0 | 7 |
| Skeletal animation and GPU skinning (ozz) | P0 | 8 |
| Animation state machine | P0 | 8 |
| Blending | P0 | 8 |
| Animation events | P0 | 8 |
| Partial and additive blending | P1 | 8 |
| IK and foot placement | P1 | 8 |
| Retargeting | P1 | 8 |
| Bone attachments | P1 | 8 |

The request also asks to verify Metal, Vulkan and browser compatibility separately. Metal: Mac and iPhone. Vulkan:
MoltenVK on the Mac only (a test of the SPIR-V path, not of real Vulkan drivers). Browser: headless modules under
Node.js until SDL GPU has a WebGPU backend.
