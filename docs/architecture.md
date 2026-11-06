# Architecture

Design of the engine. Stages 1 to 3 exist (CMake, `quench_core`, `quench_physics`, the shader build, headless tests;
`quench_gpu` over SDL GPU, see "The layer as built"; `quench_app` and `quench_main`, see [App shell](app.md); the
sample, the game template; the current layout is in [AGENTS.md](../AGENTS.md#layout)), stage 5: `quench_render`
(see "Rendering" and [Render module](render.md)) and the engine demo ([Engine demo](demo.md)), and stage 6 apart from
shader hot reload: the job system, `quench_assets` and `quench_cook` (see "Threads and jobs", "Assets" and
[Jobs, assets and the cook](assets.md)), and stage 7: `quench_audio` and `quench_audio_device` (see "Audio" and
[Audio](audio.md)); everything else here describes the intended structure and the reasons for it. The binding rules
are in [AGENTS.md](../AGENTS.md). All documentation: [index](README.md).

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
  animation/  ozz runtime, state machines, blending, events, IK, attachments
  nav/        Recast navmesh building, Detour queries and crowds
  audio/      mixer, sounds, streams, 3D sound, WAV writer, analysis (headless); the playback device and the
              iOS audio session (miniaudio)
  gpu/        the GPU layer (resources, pipelines, command recording, captures, timing) and its backends:
              sdlgpu/, metal/ (planned)
  render/     retained scene, standard frame, render modules (shadows, post, screen-space effects, sky, particles,
              terrain, ray tracing)
  app/        SDL main callbacks, window, input actions, ImGui, game interface, common flags, scenario runner
shaders/quench/       engine shader library (.hlsli), included by game shaders
tools/shaderc/        quench_shaderc: HLSL -> SPIR-V -> MSL (later WGSL), build-time host tool
tools/cook/           quench_cook: asset pipeline (glTF, PNG, JPEG, KTX2 -> cooked formats)
tools/imagecompare/   quench_image_compare: capture against golden image (mean difference, share of pixels over a
                      threshold, difference image)
tools/new_game.py     creates a game from templates/game
tools/check_games.sh  builds every game against the local engine and checks that it starts and quits cleanly
tools/package_demo.sh builds the engine demo in Release and packs it into dist/ (macOS zip, optionally iOS)
examples/hello/       minimal sample game: lit mesh, ImGui, --quit-after, --capture, input and benchmark scenarios
examples/demo/        the engine demo: a menu of sections, one per engine feature, each with a scripted check
templates/game/       starting point for a new game (CMake, game class, shader, scenario test, AGENTS.md, devlog)
tests/                CTest, headless, one test name per argument; header self-containment test
tests/golden/         reference captures per backend and device (small PNGs of procedural content)
docs/releases/        release notes per version
```

## Modules and dependencies

| Target | Contents | Depends on | Headless |
|---|---|---|---|
| `quench_core` | clock, Json, logging, `quench::fs` interface, job system | enkiTS | yes |
| `quench_physics` | Jolt wrapper, character controller, terrain tiles | core, Jolt, GLM | yes |
| `quench_assets` | IDs, manager, cache, cooked formats (KTX2, `.qmodel`), BC7 decoding, streaming grid | core, GLM, xxHash, meshoptimizer, bc7decomp | yes |
| `quench_animation` | skeletons and clip sets (the `"skeleton"`, `"animation"`, `"animgraph"` asset types), sampling, blending, state machines, events, root motion, IK, foot placement | core, assets, GLM, ozz-animation (`quench_ozz`; `quench_ozz_offline` for the build functions) | yes |
| `quench_nav` | navmesh and crowds | core, Recast & Detour | yes |
| `quench_audio` | mixer, buses, sounds (the `"sound"` asset type), streams, generators, 3D sound, WAV writer, signal analysis | core, assets, GLM, miniaudio (decoders) | yes |
| `quench_audio_device` | playback device, iOS audio session | audio, miniaudio (device API), Core Audio, AVFoundation on iOS | no |
| `quench_gpu_shaders` | the table type of embedded shaders (`ShaderBlob`, used by `quench_add_shaders`) | none | yes |
| `quench_gpu` | GPU layer and backends | core, gpu_shaders, SDL3 | no |
| `quench_render` | retained scene, standard frame, render modules, GPU skinning, `SceneAssets` (cooked textures and models in a scene) | core, assets, gpu, gpu_shaders, GLM | no |
| `quench_app` | window, lifecycle, input, ImGui, game interface, flags, scenarios, captures, SDL backends of `quench::fs` and the log, the app's job system and asset manager | core, assets, audio, audio_device, gpu, GLM, ImGui, SDL3 | no |
| `quench_main` | `main` through the SDL main callbacks; linked by `quench_add_game()` | app | no |
| `quench_cook` (host tool) | glTF / GLB, PNG, JPEG, KTX2 to cooked files (also skins, skeletons and clips), sounds checked or converted to WAV; `quench_cook_assets()` | assets, audio, animation, fastgltf, stb_image, bc7enc, meshoptimizer | yes (not in games) |

The `QUENCH_HEADLESS` option (the `headless` preset, forced on for Emscripten) builds only the headless targets and
does not fetch SDL.

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

### The layer as built (stage 2)

Headers in `include/quench/gpu/` (`Device.h`, `CommandBuffer.h`, `Resources.h`, `Types.h`), the SDL GPU backend in
`src/gpu/sdlgpu/`. There is one backend, so there is no virtual dispatch yet; the native Metal backend will bring it.

- **Close to the SDL GPU model**: device, command buffers, render and copy passes, transfer buffers, uniforms pushed
  per stage and slot. The games' renderers are written against SDL GPU, so porting them is mechanical. No SDL type
  appears in the public headers except a forward-declared `SDL_Window` for presentation.
- **Resources**: `Texture`, `Buffer`, `Sampler`, `TransferBuffer`, `GraphicsPipeline` and `Fence` own their backend
  object, are move-only and release it on destruction; SDL defers the destruction until the GPU is done with the
  object. `TextureRef`, `BufferRef` and `SamplerRef` are non-owning views for bindings, pass targets and the swapchain
  texture. The device counts live resources and asserts on destruction that none outlive it.
- **Pipelines** are created from `ShaderBlob`s. The device picks the shader format once (metallib, MSL source, DXIL,
  SPIR-V, in this order among the formats the shader build embeds and the driver accepts) and creates and releases
  the shader objects itself.
- **Errors**: misuse is a `QUENCH_ASSERT` (pipeline and pass must agree on formats and sample count, one pass open at
  a time, uniform blocks are multiples of 16 bytes, mipmap generation needs Sampled and ColorTarget usage, indirect
  draws need `Caps::indirectDraw`); SDL checks some of these only in its own debug mode. Failures the caller can
  handle return `Result` errors: creation, an unsupported format, usage or sample count, submission.
- **Reversed depth by default**: `DepthState::compare` is `GreaterOrEqual` and `DepthTarget::clearDepth` is 0.
- **`gpu::Caps`**: driver, device name, shader format, and the differences found so far: `gpuTiming` (Metal only:
  `SdlGpuTimingMetal.m` reads SDL's internal fence, as TrailForge does), `depthClamp` (not in the iOS Simulator, where
  the SDL patch skips the depth clip mode), `depthBiasClamp` (not on SDL's Vulkan backend, which does not enable the
  device feature), `indirectDraw` (not in the iOS Simulator: its Metal device is GPU family Apple2), `bcTextures`,
  `astcTextures`; per format `supportsFormat` and `supportsSampleCount`.
- **SDL video**: SDL GPU needs SDL's video subsystem for every driver. The device holds a reference to it, so it is
  created and destroyed on the main thread.
- **Transfers**: `Device::upload` submits without waiting; `Device::download` blocks and is meant for captures, tests
  and tools.
- **Swapchain composition** (stage 5): `gpu::SwapchainComposition` (`Sdr`, `SdrLinear`, `HdrExtendedLinear`,
  `Hdr10`), queried per window (`supportsComposition`) and set with `setComposition`, SDR by default; the swapchain
  format follows it. `gpu::DisplayOutput` (composition, headroom, paper white) is what a frame's values mean; the app
  fills it, the renderer reads it.
- **Compute** (stage 8, for GPU skinning): `ComputePipeline` (`Device::createComputePipeline` from a compute
  `ShaderBlob`, with its thread group size), `CommandBuffer::beginComputePass` with read-write storage buffers bound
  for the pass, and in the pass a pipeline, read-only storage buffers, uniforms and `dispatch`;
  `BufferUsage::ComputeStorageRead` and `ComputeStorageWrite`. No storage textures in compute yet (no need).

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
  engine falls back by `gpu::Caps`. Temporal accumulation (TrailForge TAA, Ashfall's AO history) is reused for
  denoising.
- **Quality presets** chosen from `gpu::Caps` (TrailForge already has Low, Medium, High, Ultra in `RenderSettings.h`).
  Mobile and web need lower tiers than the Mac.

### As built (stage 5)

`quench_render` ([Render module](render.md)) has the retained scene (`render::Scene`: meshes, RGBA8 textures,
materials, instances, lights, the environment, all by generation-checked handles; no ECS) and the standard frame
(`render::Renderer`): sun shadow cascades, the HDR pass with reversed depth, the sky (atmosphere model or gradient),
height fog, motion vectors with TAA or FXAA, bloom and tone mapping (ACES or AgX) for SDR or for HDR output (extended
linear up to the display's headroom; the app shell picks the output, draws the UI at paper white and keeps captures
SDR: [App shell](app.md#hdr-output)). Games add passes at five stages
(`Shadow`, `Opaque`, `Transparent`, `PostHdr`, `Overlay`) and their own material shading as surface shaders that the
engine lights. Fallbacks are chosen from the device's format support and `gpu::Caps` (TAA to FXAA, no shadows
without a sampleable depth array, no depth clamp). The code is TrailForge's renderer and shaders (`b97883e`),
generalized; not done in this stage: Ashfall's screen-space effects (ambient occlusion, bounce light, reflections,
sun shafts), particles, terrain, quality presets, GPU timing per pass.

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
  library (`shaders/`, so `#include "quench/Surface.hlsli"`). `quench_shaderc` writes a depfile, so editing an included file
  recompiles the shaders that use it (verified with Ninja). DXC gets the same directories with `-I`.
- Cross builds (iOS) use a macOS `quench_shaderc` given by `QUENCH_HOST_SHADERC` (the `host-tools` preset builds it).
- Storage textures and combined image samplers are refused with an error for now; no shader uses them yet.
- Compute shaders (stage 8): `quench_add_shaders(... ENTRIES name:compute)` and `quench_shaderc --stage compute`
  (read-only resources in space0, read-write storage buffers in space1, uniforms in space2, as SDL GPU wants them;
  `QUENCH_RW_STORAGE(n)` in `quench/Bindings.hlsli`; the thread group size goes into the table), DXC `cs_6_0`.

Done in stage 5: the engine shader library in `shaders/quench/` (lighting, shadows, sky, fog, tone curves, the surface
shader interface) and uniform structs defined once in headers read by both C++ and HLSL (`quench/Shared.h`,
`quench/SceneUniforms.h`; 16-byte members only). See [Render module](render.md#shader-library-and-shared-uniforms).

Planned changes:

- WGSL output for the web (from SPIR-V through Tint or naga) once SDL has a WebGPU backend.
- Ray queries need Shader Model 6.5. Whether glslang's HLSL front end can compile them is unverified; the likely path
  is DXC (it emits SPIR-V and DXIL). For Metal, either SPIRV-Cross MSL output (ray query support unverified) or
  Apple's Metal Shader Converter (DXIL -> metallib). Test on a small example before relying on either.

## Main loop and game interface

The engine owns `main` and uses SDL main callbacks (`SDL_AppInit`, `SDL_AppIterate`, `SDL_AppEvent`, `SDL_AppQuit`).
Browsers cannot run a blocking loop, and iOS and Android send the app to the background and can destroy the render
surface at any time. TrailForge ran a blocking `while (m_running)` loop in `Application::run` until its move to the
app shell (2026-11-05, TrailForge `b97883e`); Ashfall's `Application::run` still does.

A game implements `quench::app::Game` (`include/quench/app/App.h`: `init`, `fixedStep`, `update`, `ui`, `render`,
`event`, `shutdown`, and three hooks for scenarios) and defines `quench::app::createGame()`, which fills `AppDesc` and
returns the game. In `render` a game draws its `render::Scene` with `render::Renderer::render` into the frame's
target, or records its own passes into the frame's command buffer and target (as `examples/hello` does); the engine
draws ImGui on top and presents. Details: [App shell](app.md).

The fixed-step clock keeps TrailForge's behaviour: accumulator, frames longer than 0.25 s clamped, at most 12 steps
per frame, interpolation between the last two ticks. Scripted and capture runs replace it with one tick per frame, so
they are deterministic, except inside a scenario's `perf_begin` .. `perf_end` region, which runs in real time.

## Files

All runtime file access goes through `quench::fs`: read-only assets (app bundle on Apple platforms, APK on Android,
virtual file system on the web) and a writable user directory (`SDL_GetPrefPath`; IndexedDB-backed on the web).
TrailForge has 18 and Ashfall 5 uses of `std::ifstream`, `std::ofstream` or `fopen` that move to it when their code
moves to the engine.

## Threads and jobs

One job system for the engine, `quench::JobSystem` on enkiTS (`include/quench/core/Jobs.h`): asset reading and
decoding, the cook's BC7 encoding, Jolt's jobs (through `JoltJobSystem`, a `JPH::JobSystemWithBarrier`, so Jolt does
not start a second pool) and game work. Jobs have dependencies and three priorities; `parallelFor` splits a range.
`wait` runs queued jobs on the waiting thread, so waiting inside a job does not deadlock. Worker threads call
`raiseWorkerThreadPriority()` from enkiTS's thread start callback (user-interactive QoS on Apple platforms), as
TrailForge does for Jolt's workers. With zero workers (builds without threads, the web without cross-origin isolation)
jobs run on the creating thread inside `wait`, `waitAll` and `runFor`; the asset manager calls `runFor` with a time
slice per frame, so loading still makes progress without blocking. The app owns one job system (`App::jobs()`);
`PhysicsWorld(JobSystem*)` runs Jolt's jobs on it (with `nullptr`, on the thread that calls `step()`).
`AnimationSystem::update` splits its characters over `parallelFor` and waits (the frame thread works too); the ground
query a game gives it for foot placement is called from those jobs, so it must be safe to call concurrently (Jolt's
queries are, while the world does not step). Details: [Jobs, assets and the cook](assets.md).

The one exception is the audio device's callback thread (stage 7). The operating system's audio API owns it (Core
Audio through miniaudio) and calls it on its own schedule, every few milliseconds, with a real-time deadline, so it
cannot be a job. It only mixes (`AudioSystem::render`): no allocation, freeing, locks, logging, file access or waits.
The game thread talks to it through single-producer single-consumer queues of plain commands and events and reads
its statistics from atomics; decoding and stream refills run on the job system. Its priority is set by the system,
not by `raiseWorkerThreadPriority()`. Offline mixing (tests, `--audio-capture`, `--no-audio`) runs the same code on
the main thread instead. Details: [Audio](audio.md#threads).

## Assets

As built in stage 6 ([Jobs, assets and the cook](assets.md)):

- **IDs**: `AssetId`, XXH64 of the asset path as `quench::fs` takes it, identical on every platform and in the cook.
- **Manager**: `AssetManager` for every asset type (textures and models in `quench_render`, sounds in
  `quench_audio`, skeletons, clip sets and graphs in `quench_animation`): reference-counted `AssetRef`s, states loading,
  ready and failed, a placeholder until ready and after a failure (one log line, never a crash), CPU and GPU bytes
  per type with budgets, and LRU eviction of unreferenced assets over budget. A type is registered once per
  manager, by the system that owns it (`SceneAssets`, `AudioSystem`, `AnimationSystem`): so one `SceneAssets` (one
  scene) per manager; a second scene needs a second manager on the same job system.
- **Asynchronous loading**: files are read through `quench::fs` and decoded on jobs; the main-thread part (the GPU
  upload) runs in `AssetManager::update()` under a byte budget per frame, so a frame never waits for a load.
- **Cooked formats**: games ship cooked files: KTX2 textures (BC7 or RGBA8 with mips; BC7 is decoded to RGBA8 on a
  device that cannot sample it; ASTC or Basis Universal transcoding for Android and the web is later work) and
  `.qmodel` models (meshoptimizer-ordered and compressed vertex and index streams, LOD ranges, materials naming their
  textures). The importers (fastgltf, stb_image, bc7enc) live only in `quench_cook`, a host tool; games call
  `quench_cook_assets()` and `quench_add_game(... COOKED_ASSETS ...)`.
- **Hot reload** (on the Mac): the manager polls the modification time of loaded files and swaps a changed file in
  under the same ID and handles; rebuilding the game's assets target cooks the changed sources and copies the changed
  files. Shaders are not reloaded yet (stage 6 remaining work: SDL GPU on Metal accepts MSL source compiled at run
  time). On the iPhone hot reload needs an asset server over the network (later).
- **Streaming by distance**: `StreamingGrid`, cells on a grid around any number of focus points with a load and a
  larger keep radius, nearest first; generalized from TrailForge's `TerrainTiles` (physics terrain tiles). What a cell
  holds is the game's (typically `AssetRef`s loaded with the distance as priority).

## Audio

As built in stage 7 ([Audio](audio.md)): `quench_audio` is a headless mixer (32-bit float stereo, 48 kHz by
default) that does not open a device. Something pulls the mix through `AudioSystem::render`: the device of
`quench_audio_device` (miniaudio's playback API; Core Audio on Apple platforms) or an offline loop on the main thread
(`mixOffline`: tests, `--audio-capture`, `--no-audio`, a missing device). Sounds are an asset type of the
`AssetManager`, decoded on jobs; music is streamed through `quench::fs` readers and decoded on jobs ahead of the
mix. The app owns one `AudioSystem` (`App::audio()`) and the device, and pauses the device in the background. The
browser's audio output (Web Audio) is not built: the mixer is checked under Node.js only.

## Animation

As built in stage 8 ([Animation](animation.md)). ozz-animation provides sampling, blending with per-joint weights,
additive layers, two-bone and aim IK, local-to-model matrices (for attachments) and, in its offline library, the
skeleton and clip builders and the key reduction. ozz stays out of the public headers (poses and matrices are GLM).
The engine adds:

- `quench_animation`: skeletons, clip sets and graphs as asset types; an `AnimationSystem` that updates its
  characters in parallel on the job system (with zero workers on the calling thread), owned by the game;
- data-driven state machines in JSON (layers that override on a joint mask or add, states with a clip or a 1D blend
  tree, transitions with conditions, triggers, exit times and cross-fades);
- events as named markers on a clip's timeline (from the cook's clip settings), delivered in order per update, also
  across loops and for updates longer than a loop;
- root motion taken from a named joint and reported per update;
- foot placement (two-bone IK on the ground a game's ray query finds, from job threads; the demo casts into Jolt) and
  look-at (aim IK down a chain);
- GPU skinning in `quench_render`, in a compute pass, not in the vertex shader (decided without the user,
  2026-11-06): the standard frame draws a mesh up to five times (shadow cascades, the main pass, motion vectors), the
  surface and shadow shaders stay as they are, and stage 9's ray tracing can refit acceleration structures from the
  posed vertices. It needed compute in `quench::gpu` and the shader build (above);
- cooking of glTF skins, skeletons and animations in `quench_cook`.

Not done: retargeting between skeletons (not in ozz: bone name mapping and rest pose correction), 2D blend spaces,
animation LOD.

## Coordinate system and units

- Right-handed, **Y up**; meters, kilograms, seconds.
- Jolt and GLM matrices are column-major; NDC depth 0..1 (`GLM_FORCE_DEPTH_ZERO_TO_ONE`).
- The scene is rendered with reversed depth (near 1, far 0); shadow maps keep standard depth.
