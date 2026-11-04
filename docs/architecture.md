# Architecture

Design of the engine. Stage 1 exists (CMake, `quench_core`, `quench_physics`, the shader build, headless tests; the
current layout is in [AGENTS.md](../AGENTS.md#layout)); everything else here describes the intended structure and the
reasons for it. The binding rules are in [AGENTS.md](../AGENTS.md). All documentation: [index](README.md).

## Planned layout

```
CMakeLists.txt, CMakePresets.json
cmake/            Dependencies.cmake (pinned FetchContent), ShaderToolchain.cmake, QuenchShaders.cmake
                  (quench_add_shaders), EmbedShaders.cmake, QuenchGame.cmake (quench_add_game: bundles, icon,
                  packages), patches/ (SDL and ImGui patches carried over from TrailForge)
include/quench/<module>/   public headers (the only headers games include)
src/<module>/              sources and internal headers, per module:
  core/       FixedStepClock, Json, hashing, logging (quench::log), Result, QUENCH_ASSERT, fs interface, job system
  physics/    PhysicsWorld (Jolt), character controller, ray queries, streamed terrain tiles
  assets/     asset IDs, asset manager and cache, cooked format readers, streaming
  anim/       ozz runtime, state machines, blending, events, IK, attachments
  nav/        Recast navmesh building, Detour queries and crowds
  audio/      mixer (no device), device and offline capture (miniaudio)
  gpu/        the GPU layer (resources, pipelines, command recording, captures, timing) and its backends:
              sdlgpu/, metal/ (planned)
  render/     retained scene, standard frame, render modules (shadows, post, screen-space effects, sky, particles,
              terrain, ray tracing)
  app/        SDL main callbacks, window, input actions, ImGui, game interface, common flags, scenario runner
shaders/quench/       engine shader library (.hlsli), included by game shaders
tools/shaderc/        quench_shaderc: HLSL -> SPIR-V -> MSL (later WGSL), build-time host tool
tools/cook/           quench_cook: asset pipeline (glTF, PNG, JPEG, KTX2 -> cooked formats)
tools/render/         compare_captures.py (pixel statistics between two capture sets)
tools/check_games.sh  builds every game against the local engine and checks that it starts and quits cleanly
examples/hello/       minimal sample game: lit mesh, ImGui, --quit-after, --capture, benchmark scenario
templates/game/       starting point for a new game (CMake, game class, shader, test, AGENTS.md, devlog)
tests/                CTest, headless, one test name per argument; header self-containment test
tests/golden/         reference captures per backend and device (small PNGs of procedural content)
docs/releases/        release notes per version
```

## Modules and dependencies

| Target | Contents | Depends on | Headless |
|---|---|---|---|
| `quench_core` | clock, Json, hashing, logging, `quench::fs` interface, job system | enkiTS | yes |
| `quench_physics` | Jolt wrapper, character controller, terrain tiles | core, Jolt, GLM | yes |
| `quench_assets` | IDs, manager, cache, cooked formats, streaming | core, libktx, meshoptimizer | yes |
| `quench_anim` | animation runtime and state machines | core, ozz | yes |
| `quench_nav` | navmesh and crowds | core, Recast & Detour | yes |
| `quench_audio` | mixer; device in a separate target | core, miniaudio | mixer yes |
| `quench_gpu` | GPU layer and backends | core, SDL3 | no |
| `quench_render` | scene, frame, render modules | gpu, assets, anim | no |
| `quench_app` | main callbacks, window, input, UI, game interface | render, audio, SDL3, ImGui | no |

Headless targets build and test without SDL video or a GPU. This is what lets the games keep their headless test
presets and what makes the Node.js web check possible.

`quench::fs` has two backends behind one interface (`quench::fs::Backend`), chosen at run time: the C++ standard
library one in `quench_core` is the default (headless host tests and tools), and the app module installs an
SDL_IOStream one at startup (bundle, APK, browser file system). Headless targets therefore need no SDL. Paths are
checked in one place before any backend sees them: relative, '/' separators, no '.', '..' or empty segments, no '\'
or ':'. User files are written next to the target and renamed over it, so an interrupted write leaves the previous file
intact (no fsync: a power loss right after the rename is not covered).

## Graphics layer

All graphics goes through `quench::gpu`: engine-owned handles for buffers, textures, samplers and pipelines,
descriptor structs, and command recording. Backends:

- **SDL GPU** (first, portable): Metal on Apple platforms, Vulkan and D3D12 elsewhere; WebGPU only if SDL ships it.
- **Metal** (planned, native): for what SDL GPU does not offer.

Why a native backend at all: SDL GPU deliberately targets a roughly ten-year-old common feature set. In reply to a
request for a ray tracing API, its maintainers stated that features missing from the API were omitted intentionally
and that pull requests are considered only if they implement the feature for every supported backend, consoles
included (libsdl-org/SDL#11147, closed). They also do not want to expose native object handles
(libsdl-org/SDL#12094). TrailForge already works around the missing GPU timestamps by reading SDL's
internal Metal fence struct (`src/render/GpuTimingMetal.m`, tied to the layout in SDL 3.4.18), which any SDL update can
break. Hardware ray tracing, MetalFX and timestamps therefore need a backend of our own.

Why not more native backends: in SDL 3.4.18 the backend sources alone are 4696 lines (`SDL_gpu_metal.m`), 13124
(`SDL_gpu_vulkan.c`) and 9732 (`SDL_gpu_d3d12.c`), plus 3632 shared (`SDL_gpu.c`), and only Metal can be verified on
the available hardware. Further backends wait for a named need.

Verification bonus: on the Mac, SDL GPU also runs on Metal, so the same frame can be rendered through both backends on
one machine and compared pixel by pixel.

## Rendering

- **Standard frame.** Shadows, main HDR pass, sky, post-processing chain (bloom, tonemap, anti-aliasing), assembled
  from modules a game enables. Games add passes and material shading through engine APIs instead of writing a whole
  renderer (today TrailForge's renderer is 3301 lines and Ashfall's 1158).
- **Retained scene.** Meshes, instances, materials and lights are registered with the engine and updated, not
  resubmitted every frame. Ray tracing needs all geometry to build acceleration structures, and hit shaders need
  material data for surfaces that are not on screen. TrailForge is mostly retained already (`setTerrain`, `setTrees`,
  `setModels`); Ashfall submits a list of `DrawItem`s per frame.
- **Effect slots with fallbacks.** Shadows (shadow maps or ray traced), reflections (SSR or ray traced), ambient
  occlusion (GTAO or ray traced), indirect light (Ashfall's screen-space bounce or ray traced). The game picks; the
  engine falls back by `GpuCaps`. Temporal accumulation (TrailForge TAA, Ashfall's AO history) is reused for
  denoising.
- **Quality presets** chosen from `GpuCaps` (TrailForge already has Low, Medium, High, Ultra in `RenderSettings.h`).
  Mobile and web need lower tiers than the Mac.

## Shaders

The toolchain carried over from TrailForge: glslang (HLSL front end) -> SPIR-V -> SPIRV-Tools legalization ->
SPIRV-Cross MSL -> `xcrun metal` -> metallib; DXC -> DXIL on Windows hosts; all blobs embedded in the binary. Every
entry point is guarded by a macro and follows the SDL GPU register conventions (vertex: resources space0, uniforms
space1; fragment: resources space2, uniforms space3).

Done in stage 1 (`quench_add_shaders()` in `cmake/QuenchShaders.cmake`):

- One table per shader set, `quench::shaders::<name>` (`include/quench/gpu/ShaderBlob.h`), so the engine and a game
  can each have several sets. A row holds the entry name, the stage and the resource counts SDL GPU needs to create
  the shader (samplers, storage textures, storage buffers, uniform buffers), read from the compiled SPIR-V by
  `quench_shaderc`; the games pass those counts by hand today.
- `#include "x"` resolves next to the including file, then in the set's `INCLUDE_DIRS`, then in the engine shader
  library (`shaders/`, empty until stage 5). `quench_shaderc` writes a depfile, so editing an included file
  recompiles the shaders that use it (verified with Ninja). DXC gets the same directories with `-I`.
- Cross builds (iOS) use a macOS `quench_shaderc` given by `QUENCH_HOST_SHADERC` (the `host-tools` preset builds it).
- Storage textures and combined image samplers are refused with an error for now; no shader uses them yet.

Planned changes:

- The engine shader library in `shaders/quench/`.
- Uniform structs in shared headers read by both C++ and HLSL.
- WGSL output for the web (from SPIR-V through Tint or naga) once SDL has a WebGPU backend.
- Ray queries need Shader Model 6.5. Whether glslang's HLSL front end can compile them is unverified; the likely path
  is DXC (it emits SPIR-V and DXIL). For Metal, either SPIRV-Cross MSL output (ray query support unverified) or
  Apple's Metal Shader Converter (DXIL -> metallib). Test on a small example before relying on either.

## Main loop and game interface

The engine owns `main` and uses SDL main callbacks (`SDL_AppInit`, `SDL_AppIterate`, `SDL_AppEvent`, `SDL_AppQuit`).
Browsers cannot run a blocking loop, and iOS and Android send the app to the background and can destroy the render
surface at any time. Today TrailForge runs a blocking `while (m_running)` loop in `Application::run`.

A game implements an interface along these lines (sketch, not final):

```cpp
namespace quench {
class Game {
public:
    virtual ~Game() = default;
    virtual bool init(Engine& engine) = 0;
    virtual void fixedStep(double dt) = 0;        // simulation tick (1/120 s by default)
    virtual void update(double frameDt) = 0;      // per frame, before rendering
    virtual void render(FrameContext& frame) = 0; // fill the retained scene, add game passes
    virtual void event(const Event& e) {}
};
}
```

The fixed-step clock keeps TrailForge's behaviour: accumulator, frames longer than 0.25 s clamped, at most 12 steps
per frame, interpolation between the last two ticks.

## Files

All runtime file access goes through `quench::fs`: read-only assets (app bundle on Apple platforms, APK on Android,
virtual file system on the web) and a writable user directory (`SDL_GetPrefPath`; IndexedDB-backed on the web).
TrailForge has 18 and Ashfall 5 uses of `std::ifstream`, `std::ofstream` or `fopen` that move to it when their code
moves to the engine.

## Threads and jobs

One job system (enkiTS) for the engine: asset decoding, animation, streaming and Jolt (through a
`JPH::JobSystemWithBarrier` implementation, so Jolt does not start a second pool). It must also run with a single
thread. Worker threads get user-interactive QoS on Apple platforms through enkiTS's thread start callback, as
TrailForge does for Jolt's workers today.

## Assets

- **IDs**: a 64-bit hash (xxHash) of the logical asset path, identical on every platform.
- **Manager**: reference-counted handles; states loading, ready, failed; a placeholder until ready (TrailForge already
  uses 1x1 placeholders); memory accounting per type (CPU and GPU bytes) with budgets; LRU eviction for streamed
  assets.
- **Asynchronous loading**: decoding and transcoding on job threads; GPU uploads on the render thread under a byte
  budget per frame, so a frame never waits for a load.
- **Cooked formats**: games ship only cooked assets: meshes optimized and quantized with meshoptimizer (LODs, compressed
  buffers), textures as KTX2 (BC7 for the Mac and the iPhone; Basis Universal for Android and the web), skeletons and
  animations as ozz archives, materials as data. The importers (fastgltf, stb_image, the KTX encoder) live in
  `quench_cook` and in development builds, not in release builds.
- **Hot reload** (development builds on the Mac first): a changed source file is cooked again and the manager swaps the
  resource under the same ID. Shaders are recompiled at run time to MSL source, which SDL GPU on Metal accepts
  (TrailForge already supports the "MSL source (runtime compile)" path). On the iPhone hot reload needs an asset server
  over the network (later).
- **Streaming by distance**: world cells with a priority by distance, hysteresis and a memory budget; a
  generalisation of TrailForge's streamed physics terrain tiles (`TerrainTiles`) to render resources and physics
  bodies.

## Animation

ozz-animation provides sampling, blending with per-joint weights, additive layers, two-bone and aim IK, user channel
tracks with a triggering job (for events), local-to-model matrices (for attachments) and motion extraction. The engine
adds:

- data-driven state machines (states, transitions with conditions and durations, blend spaces by speed and direction);
- animation events from ozz user channel tracks, delivered as an event stream like Ashfall's `GameEvent`;
- GPU skinning: a joint matrix palette in a storage buffer read by the vertex shader (SDL GPU supports vertex storage
  buffers; TrailForge's trees and level models already use them);
- foot placement from ozz two-bone IK and Jolt ray casts;
- retargeting between compatible skeletons (not in ozz: bone name mapping and rest pose correction).

## Coordinate system and units

- Right-handed, **Y up**; meters, kilograms, seconds.
- Jolt and GLM matrices are column-major; NDC depth 0..1 (`GLM_FORCE_DEPTH_ZERO_TO_ONE`).
- The scene is rendered with reversed depth (near 1, far 0); shadow maps keep standard depth.
