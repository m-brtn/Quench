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
- Moved: a separate `headless` preset is only useful once non-headless targets exist; it comes with stage 2.

## Stage 2: GPU layer

- `quench::gpu` over SDL GPU: device, buffers, textures, samplers, pipelines, command recording, uploads, mipmap
  generation, render target captures, `GpuCaps`.
- GPU timing behind the layer (the `GpuTimingMetal.m` hack stays isolated until the native Metal backend replaces it).
- Verified on the Mac (Metal), in the iOS Simulator and on the iPhone; Vulkan through MoltenVK on the Mac.

## Stage 3: app shell

- SDL main callbacks, window, the game interface, common flags (`--quit-after`, `--capture`, `--capture-at`),
  scenario runner, ImGui, input actions (keyboard, mouse, gamepad, touch), iOS bundle and app icon CMake
  (`quench_add_game()`).
- `examples/hello`: a lit mesh with a capture check against `tests/golden/`, and a benchmark scenario; first budgets
  in `docs/performance.md`. `templates/game` and a script that creates a new game from it.
- Version 0.1.0 and the first release notes.

## Stage 4: games on the engine

TrailForge first, then Ashfall: replace their copies of stages 1 to 3 with the engine, following
[Migration](migration.md). `tools/check_games.sh` (build against the local engine, start, quit cleanly) after each
change; each game's full gates at the end of the work.

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

## Stage 7: animation

- ozz runtime, GPU skinning, data-driven state machines, blending, animation events.
- Then partial and additive blending, IK and foot placement, bone attachments, retargeting.

## Stage 8: native Metal backend and ray tracing

- Metal backend of `quench::gpu` (pixel comparison against the SDL GPU backend on the same Mac), GPU timestamps.
- Acceleration structures from the retained scene; ray-traced shadows first, then reflections, ambient occlusion and
  indirect light, each as a fallback slot. MetalFX upscaling.
- Shader path for ray queries decided by a test first ([Architecture](architecture.md#shaders)).

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
| Skeletal animation and GPU skinning (ozz) | P0 | 7 |
| Animation state machine | P0 | 7 |
| Blending | P0 | 7 |
| Animation events | P0 | 7 |
| Partial and additive blending | P1 | 7 |
| IK and foot placement | P1 | 7 |
| Retargeting | P1 | 7 |
| Bone attachments | P1 | 7 |

The request also asks to verify Metal, Vulkan and browser compatibility separately. Metal: Mac and iPhone. Vulkan:
MoltenVK on the Mac only (a test of the SPIR-V path, not of real Vulkan drivers). Browser: headless modules under
Node.js until SDL GPU has a WebGPU backend.
