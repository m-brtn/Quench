# Architecture

Design of the engine. Stages 1 to 3 exist (CMake, `quench_core`, `quench_physics`, the shader build, headless tests;
`quench_gpu` over SDL GPU, see "The layer as built"; `quench_app` and `quench_main`, see [App shell](app.md); the
sample, the game template; the current layout is in [AGENTS.md](../AGENTS.md#layout)), stage 5: `quench_render`
(see "Rendering" and [Render module](render.md)) and the engine demo ([Engine demo](demo.md)), and stage 6 apart from
shader hot reload: the job system, `quench_assets` and `quench_cook` (see "Threads and jobs", "Assets" and
[Jobs, assets and the cook](assets.md)), and stage 7: `quench_audio` and `quench_audio_device` (see "Audio" and
[Audio](audio.md)), and stage 9: the native Metal backend, GPU timestamps per pass, acceleration structures,
ray-traced sun shadows and ambient occlusion, MetalFX upscaling (see "Native Metal backend" and "Ray tracing"), and
stage 11: ray-traced local light shadows, reflections and indirect light, a probe grid, the engine's temporal
denoiser, MetalFX's denoising upscaler and a reference path tracer (see "Ray tracing"), and stage 12: area lights
and anisotropic GGX (see "Rendering");
everything else here
describes the intended structure and the reasons for it. The binding rules
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
              sdlgpu/, metal/ (Apple platforms), shaders/ (the backends' own shaders)
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
| `quench_gpu` | GPU layer and backends (SDL GPU; native Metal on Apple platforms) | core, gpu_shaders, SDL3; GLM (private); Metal, QuartzCore on Apple platforms | no |
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

- **SDL GPU** (first, portable, the default): Metal on Apple platforms, Vulkan and D3D12 elsewhere; WebGPU only if SDL
  ships it.
- **Metal** (native, since stage 9; Apple platforms): for what SDL GPU does not offer. Built and at parity with SDL
  GPU's Metal driver (see "Native Metal backend"), with GPU timestamps per pass, acceleration structures and ray
  queries, and MetalFX.

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
`src/gpu/sdlgpu/`. Since stage 9 the public classes forward to a backend interface, `detail::DeviceImpl`
(`src/gpu/Backend.h`, one virtual call per operation, opaque handles), with two implementations: SDL GPU and the
native Metal backend (`src/gpu/metal/`, below). The front end (`src/gpu/Device.cpp`, `CommandBuffer.cpp`) keeps the
checks that do not depend on the backend: the asserts below, live resource counts, pass state.

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
- **`gpu::Caps`**: backend, driver, device name, shader format, and the differences found so far: `gpuTiming` (Metal
  only: on SDL GPU `SdlGpuTimingMetal.m` reads SDL's internal fence, as TrailForge does; the native backend reads the
  command buffer's GPU times), `gpuPassTiming` (native Metal backend only, see "GPU timestamps per pass"),
  `rayTracing`, `spatialUpscaling`, `temporalUpscaling` and `denoisedUpscaling` (native Metal backend only, see
  "Ray tracing"),
  `depthClamp` (not in the iOS Simulator, where
  the SDL patch skips the depth clip mode), `depthBiasClamp` (not on SDL's Vulkan backend, which does not enable the
  device feature), `indirectDraw` (not in the iOS Simulator: its Metal device is GPU family Apple2), `bcTextures`,
  `astcTextures`; per format `supportsFormat` and `supportsSampleCount`.
- **SDL video**: SDL GPU needs SDL's video subsystem for every driver. Its device holds a reference to it, so it is
  created and destroyed on the main thread. The native Metal device needs video only for windows, which the app
  creates (offscreen tests run without it).
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

### Native Metal backend

Stage 9, first half (2026-11-06): `src/gpu/metal/` (`MetalDevice.mm`: device, resources, swapchain, fences, timestamps;
`MetalCommands.mm`: command recording; Objective-C++ with ARC), built on Apple platforms only. It implements the whole
`quench::gpu` surface that the SDL GPU backend does, from the same compiled shaders, and its behaviour follows SDL
3.4.18's Metal backend (`SDL_gpu_metal.m`) wherever the engine could observe a difference.

- **Selection**: `DeviceDesc::backend` (`gpu::Backend::Default`, `SdlGpu`, `Metal`); `Default` reads the
  `QUENCH_GPU_BACKEND` environment variable (`sdl` or `metal`), else SDL GPU. In the app: `AppDesc::gpuBackend` and the
  `--gpu-backend sdl|metal` flag ([App shell](app.md#command-line-flags)). Off Apple platforms a request for Metal logs a warning
  and creates SDL GPU. `Caps::backend` says which one runs; the device's log line names it.
- **Default stays SDL GPU** (decided without the user, 2026-11-06, as the stage plan says: "until parity is shown on
  every platform"). Parity is shown on the Mac (below) and in the iOS Simulator's tests, not on the iPhone, which was
  not available. The switch is a one-line change in `chooseBackend` (`src/gpu/Device.cpp`) after a device run.
- **Shaders and bindings**: the same `quench_shaderc` metallib (or MSL source) as SDL GPU, so the slots are the ones
  SPIRV-Cross assigns for SDL GPU's register spaces. Per stage: uniform block `b[n]` in `buffer(n)`; sampled texture
  and sampler `t[n]`, `s[n]` in `texture(n)`, `sampler(n)`; storage textures in `texture(numSamplers + k)`; storage
  buffers in `buffer(numUniformBuffers + k)`; vertex buffer slot `i` in `buffer(14 + i)`. Compute: read-only storage
  buffers in `buffer(numUniformBuffers + k)`, read-write ones in `buffer(numUniformBuffers + numReadOnly + k)`,
  uniforms in `buffer(n)`. Uniforms go into 32 KB shared chunks at 256-byte offsets; a slot the pipeline declares but
  nothing pushed reads a zeroed buffer (SDL binds a pooled buffer with whatever it held). Bindings are applied lazily at the draw or dispatch, and reset at
  the end of every pass.
- **Resources**: textures and GPU buffers in private storage, transfer buffers in shared storage (write-combined for
  uploads) with one backing per cycle that the GPU still uses. `cycle` on a GPU buffer or texture is a no-op: Metal's
  hazard tracking orders the writes after earlier reads (SDL keeps several copies instead; the pixels are the same,
  the overlap of frames may differ). Releasing a resource drops the reference at once; command buffers retain what
  they use. Depth24Stencil8 only on macOS.
- **Passes**: render, compute (serial dispatch) and copy passes on their own encoders; at the start of a render pass
  the viewport and scissor cover the smallest target, blend constants are 1 and the stencil reference 0; no depth
  clip mode in the iOS Simulator (`Caps::depthClamp` false, as with the SDL patch). `CommandBuffer::blit` is a render
  pass with the engine's blit shader (`src/gpu/shaders/blit.hlsl`), for 2D and 2D array sources (3D and cube sources
  log an error; nothing in the engine blits them). Mipmaps through the blit encoder.
- **Swapchain**: `SDL_Metal_CreateView` and its `CAMetalLayer`; at most two frames in flight per window; `Sdr` is
  BGRA8, `SdrLinear` BGRA8 sRGB, `HdrExtendedLinear` RGBA16F in extended linear sRGB with EDR, `Hdr10` RGB10A2 in
  BT.2100 PQ (macOS only, as SDL). Vsync everywhere, immediate on macOS (`displaySyncEnabled`), no mailbox. The
  drawable is presented before the commit, as in SDL.
- **Differences kept**: cube arrays are created with `layers / 6` cubes (SDL GPU passes the layer count to
  `arrayLength`, six times the memory); debug labels and groups only with `DeviceDesc::debug`, as in SDL.
- **Not covered**: anything SDL GPU itself lacks (storage textures in compute, multiple command queues). The native
  features for which the backend exists are timestamps (below), acceleration structures with ray queries and MetalFX
  (see "Ray tracing").
- **SDL GPU's Vulkan mipmaps**: SDL 3.4.18's Vulkan driver halves both sides per mip level without stopping at 1, so
  the last levels of a non-square texture were blits into an empty region (`VUID-vkCmdBlitImage-dstOffsets-12519`
  with the demo's 128 x 64 fence texture). The SDL GPU backend blits those textures' levels itself, each side at
  least 1 (`SdlGpuDevice::generateMipmaps`; `gpu_vulkan.generate_mipmaps` covers an 8 x 2 texture).
- **Verified** (2026-11-06, Mac, M4 Pro, `MTL_DEBUG_LAYER=1`): every GPU device and render case
  (`gpu_metal_native.*`, `render_metal_native.*`), hello and all eleven demo captures against the Metal golden
  images, identical in Release (mean difference 0, no pixel different) and identical to SDL GPU's captures of the same
  Debug build; the audio section's offline mix byte-identical. ctest keeps `hello_metal_native`,
  `demo_animation_metal_native` and `demo_hdr_on_metal_native` (wall time); the other sections were run by hand. iOS
  Simulator: see [Platforms](platforms.md).

### GPU timestamps per pass

`Caps::gpuPassTiming` (native Metal backend where the device samples timestamps at stage boundaries, which is how
Apple GPUs sample them; SDL GPU reports false). `CommandBuffer::beginTiming(name)` / `endTiming()` around passes,
outside passes, nesting allowed; after `submitWithFence`, `Device::gpuTimings(fence)` returns per range its GPU busy
time, its start and end within the command buffer and its nesting depth (`gpu::GpuTiming`). The backend attaches a
counter sample buffer (512 samples per command buffer, pooled) to every encoder that starts inside a range: start and
end of the vertex and fragment stages of a render pass, start and end of a compute or blit pass. A range's busy time
is the union of those stage intervals: on Apple GPUs the vertex stage of a pass runs while the previous pass still
shades fragments, so a range from first to last timestamp would count the wait, and the sum of the ranges may exceed
the frame. The timestamp period is measured once against the CPU clock (1.0000 ns on the M4 Pro). The renderer times
`upload`, `accel`, `prepass`, `raytrace` (`rays`, `denoise`, `probes`, `reflections`, `indirect`), `path`, `shadows`,
`hdr`, `taa`, `upscale`, `bloom` and `tonemap` inside the app's `game` range; the app adds `ui` and `blit`
([Performance](performance.md#gpu-time-per-pass)). MetalFX encodes passes of its own: in a range, a blit encoder
before and one after them copy a texel (of the input, then of the output, so Metal orders them around MetalFX's
work) and take the range's start and end timestamps at their own start (after the temporal scaler an end-of-encoder
sample was not written; an empty encoder is dropped with its samples).

Off unless read (stage 9, second half): `Device::setPassTiming(bool)`; a command buffer acquired while it is off
records no ranges. Timing costs CPU time in every timed command buffer, measured 0.08 to 0.14 ms per frame in the demo
(sample attachments, resolving, the app's bookkeeping). The app turns it on while something reads the ranges:
`App::requestPassTiming()` in a frame (the demo's frame panel), a scenario's perf region, or `--gpu-pass-timing`
([App shell](app.md#command-line-flags)). Without timing, the native backend's CPU time in the demo is that of SDL GPU
within the noise (see [Performance](performance.md#measurements)).

## Ray tracing

Stage 9, second half (2026-11-06). Built on the native Metal backend and the retained scene, and keeping the rules:
effects are slots with fallbacks chosen by `gpu::Caps`, games do not change when a backend gains ray tracing, and no
ray tracing type is backend-specific in a public header. The design written before it (inline ray queries, one bottom
level per mesh, a top level per frame, refits for skinned instances, DXC for the SPIR-V step) held; the differences
are noted below.

- **Capability**: `Caps::rayTracing` (named `rayQuery` in the design): acceleration structures and ray queries in
  fragment and compute shaders, `supportsRaytracing` and `supportsRaytracingFromRender` on Metal. False on SDL GPU
  (no ray tracing; also Metal through SDL GPU), on Vulkan through MoltenVK (no ray tracing extension, see
  [Roadmap](roadmap.md#stage-9-native-metal-backend-and-ray-tracing)), and in the iOS Simulator where its device
  lacks them. No ray tracing pipelines (hit groups, shader tables): inline ray queries cover shadows, ambient
  occlusion, reflections and indirect light that read material data from buffers (stage 11 kept it), and they map
  to Metal's `intersection_query`,
  Vulkan's `VK_KHR_ray_query` and DXR 1.1's `RayQuery` alike.
- **`quench::gpu` surface**: `AccelerationStructure` (move-only), created from `AccelerationStructureDesc` (level
  `Bottom` with vertex and triangle counts and `allowRefit`, or `Top` with an instance capacity; the backend sizes
  it, so there is no `accelerationStructureSizes` query and no scratch buffer in the API: the Metal backend keeps one
  private scratch buffer that grows). Building is a pass, `CommandBuffer::beginAccelerationStructurePass()` with
  `build(bottom, RayGeometry)` (float3 positions at an offset and stride in a vertex buffer, as the engine's vertex
  format has them, and 32-bit indices), `refit(bottom, RayGeometry)` and `build(top, span<RayInstance>)` (3 x 4
  transform, 24-bit id, 8-bit mask, `Opaque` or `NonOpaque`; the backend writes Metal's instance descriptors into a
  pooled shared buffer, so the CPU writes no transfer buffer). Within a pass the Metal backend splits the encoder
  before a top-level build, so the bottom levels built or refitted before it are complete. Shaders get the top level
  with `RenderPass::bindAccelerationStructure(stage, slot, top)` or `ComputePass::bindAccelerationStructure`; the
  backend marks the top level's bottom levels resident (`useResources`). Stage 11 added `refit(top, instances)` (a
  top level created with `allowRefit`; the backend builds instead when the bottom levels differ from the last
  build), compaction (`queryCompactedSize(bottom)` in a pass, `Device::compactedSize(bottom)` once the command
  buffer has completed, a bottom level created with `AccelerationStructureDesc::compactedSize` and
  `copyAndCompact(source, destination)`) and `AccelerationStructure::bytes()`.
- **Bottom levels**: one per `render::Scene` mesh that casts shadows, built the first frame it is used and kept with
  the mesh, compacted the frame after (stage 11; the copy replaces it before the top level is built). Skinned
  instances: one bottom level each over the skinned vertex buffer of stage 8, refitted every frame
  after the skinning pass and built again after 60 refits (no bounding-box check: a fixed count, staggered between
  instances), not compacted. Measured in the animation section with a crowd of 256 (259 characters) and ray-traced
  shadows: the `accel` range (the top level included) 1.57 ms of GPU time per frame with refits, 5.12 ms with a
  temporary change that built every frame ([Performance](performance.md#measurements)).
- **Top level**: from the shadow-casting instances (skinned ones within `shadowDistance`), with masks for opaque and
  translucent, and the instance id indexing a table (`RayInstanceData`) of every instance's vertex and index offsets
  and material data. Refitted while the same bottom levels come in the same order, built again otherwise and after
  30 refits (stage 11; stage 9 rebuilt it every frame). The renderer owns it; games see only the effect settings.
- **Hit shading** (stage 11): inline ray queries, not hit groups: a fragment pass per effect traces, and on a hit
  reads the triangle's vertices from a copy of the meshes' vertex and index data, the material's values and its
  textures from two 2D texture arrays (base color, metallic-roughness and emissive copied once at 256 x 256), and
  shades it with the standard surface's BRDF (`src/render/shaders/RayHit.hlsli`). One shading for every surface keeps
  the passes few and the shaders known when the pipelines are made; a surface shader's own shading is not run for
  rays (the cost of hit groups or callable shaders, which inline queries do not have, against the demo's one custom
  surface: decided without the user, 2026-11-06).
- **Shaders**: HLSL `RaytracingAccelerationStructure` and `RayQuery`, entries marked `:rayquery` in
  `quench_add_shaders`, compiled by DXC (see "Shaders") and then SPIRV-Cross to MSL 3.0 as before; the acceleration
  structure takes a buffer slot after the storage buffers (`ShaderResources::accelerationStructures`). The renderer
  creates their pipelines only where `Caps::rayTracing` holds, so the other backends never load them.
- **Fallback slots** in `render::RenderSettings` and `render::ActiveEffects`: `SunShadows::ShadowMaps | RayTraced`
  (shadow maps are the fallback and stay the default until the user has looked at the traced ones on the iPhone),
  `AmbientOcclusion::None | RayTraced`, and since stage 11 `localLightShadows` (unshadowed without rays; since
  stage 12 also the area lights' soft shadows),
  `Reflections::Sky | RayTraced`, `IndirectLight::Sky | Probes | RayTraced` (the sky's ambient values without rays),
  `RayDenoiser::Spatial | Temporal` and `pathTracer`. Without the capability the renderer takes the fallback and logs
  why, as for TAA and depth clamp ([Render module](render.md#fallbacks)). The screen-space fallbacks the design
  names (SSR, GTAO, Ashfall's bounce light) are not built: a device without ray tracing gets the sky's values.
- **Denoising** (stage 11): the engine's own filters (a spatial one and, the default, a temporal one with
  reprojection, a neighbourhood clamp, anti-lag and a-trous passes; [Render module](render.md#denoising)) run
  everywhere ray tracing does; MetalFX's temporal denoised scaler replaces them and TAA where `denoisedUpscaling`
  holds and the game picks `Upscaling::TemporalDenoised` (user decision, 2026-11-06: both, the engine's for older
  OS versions, devices without MetalFX's, the TAA and FXAA paths and future backends).
- **MetalFX** (`Caps::spatialUpscaling`, `temporalUpscaling`, `denoisedUpscaling`; `Upscaler` from `UpscalerDesc`,
  `CommandBuffer::upscale` with an `UpscaleDesc`; the output texture needs `TextureUsage::ShaderWrite`): the
  renderer's `Upscaling::Spatial | Temporal | TemporalDenoised`, with bilinear scaling in tone mapping as the fallback
  ([Render module](render.md#upscaling)).
- **Path tracer** (stage 11): a progressive reference in the same fragment-pass form, reusing the hit shading and
  the acceleration structures; not a game feature ([Render module](render.md#path-tracer)).
- **Verification**: the golden-capture rule: the demo's ray tracing section has its own golden for the native
  backend, and the fallback's goldens for Metal through SDL GPU and Vulkan through MoltenVK; readback tests compare
  ray-traced shadows with shadow maps, the probe grid and the traced indirect light with the sky's light in a
  furnace, the path tracer with a uniform sky, the denoisers with many-ray references. The stage 11 effects were
  measured on the iPhone 17 ([Performance](performance.md)), not yet on the iPhone 18 Pro Max.

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
sun shafts), particles, terrain, quality presets, GPU timing per pass (added in stage 9 where the backend has it:
see "GPU timestamps per pass").

### Area lights and anisotropy (stage 12)

Area lights (rect, disk, tube) are scene lights like the others, with a size and a radiance, and the scene draws
their shapes itself, so what is seen and what lights cannot disagree ([Render module](render.md#area-lights)). The
light itself is analytic in the raster: linearly transformed cosines (Heitz et al. 2016) give the diffuse and
specular integral over the shape with no noise, on every backend; ray tracing adds only the visibility (soft shadows
in the `localLightShadows` slot, unshadowed without it), and the path tracer samples the shapes with MIS as its
reference. The fitted LTC tables are third-party data (`selfshadow/ltc_code`, BSD-2-Clause) baked into a source file
by a script, not fitted at build time. Anisotropic GGX is a material parameter (strength and rotation) along the
MikkTSpace tangent, the same lobe in the raster, the ray hits and the path tracer; the sky's reflection and the LTC
take it approximately (a bent normal, the mean roughness). Both kept the isotropic, area-free paths as they were
(branches on the material and on the frame's area light count), so no existing golden image changed.

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
- Ray queries: done (below).

Ray queries (stage 9): entries marked `<entry>:<stage>:rayquery` in `quench_add_shaders` are compiled by DXC
(`cmake/QuenchDxc.cmake`: v1.9.2609, `01b62ad`, built from its pinned source archives once per machine into a tool
cache; on Windows hosts the pinned release archive; `QUENCH_RAY_QUERY_DXC` names another one) with
`-spirv -fspv-target-env=vulkan1.2 -HV 2021 -O3` and shader model 6.5; `quench_shaderc --dxc` runs it, validates the
SPIR-V (Vulkan 1.2) and goes on as for glslang's output: reflection, SPIRV-Cross to MSL 3.0 (as every entry),
metallib. DXIL comes from DXC on Windows hosts as before (`ps_6_5`). Without a DXC (the build failed, or
`QUENCH_RAY_QUERY_SHADERS=OFF`) the entries are left out and the renderer falls back. glslang stays for every other
entry (decided without the user, 2026-11-06): its output for the existing shaders is what every golden image was
made with, DXC's HLSL rules differ in places that would need every shader and capture checked again (matrix
packing, implicit conversions, its own legalization), and glslang is a library inside `quench_shaderc` where DXC is a
27 MB executable and library built from source. Build cost of the pinned DXC on the M4 Pro: 135 s wall time (1146 s
user), about 365 MB of scratch space at its peak, deleted afterwards, 27 MB kept (`dxc` and `libdxcompiler`), once per machine.

The test before it (2026-11-06, a compute kernel with one `RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH>`,
  `TraceRayInline`, `Proceed`, `CommittedStatus`, `CommittedRayT` and a `RaytracingAccelerationStructure` at
  `t0, space0`):
  - **glslang's HLSL front end: no.** Homebrew's glslangValidator 16.6.0 stops at the first line
    (`'declaration' : Expected`, "HLSL parsing failed"); the pinned glslang (`vulkan-sdk-1.4.357.0`) has no
    `RaytracingAccelerationStructure`, `RayQuery` or `TraceRayInline` anywhere in `glslang/HLSL/`. The current
    `quench_shaderc` cannot compile ray tracing shaders.
  - **DXC: yes.** DXC 1.9 (`1.9.0.5512`, `01b62ad4`, the macOS build in the LunarG Vulkan SDK 1.4.363.1, installed
    for the test and removed) with `-T cs_6_5 -spirv -fspv-target-env=vulkan1.2` emits SPIR-V with `RayQueryKHR` and
    `SPV_KHR_ray_query` that `spirv-val` accepts; without `-spirv` it emits a DXIL container (`DXBC`, with
    `dx.op.allocateRayQuery` and `rayQuery_TraceRayInline`; not run, no D3D12 device). DXC's licence is NCSA (as
    for the Windows build already pinned). DXC publishes no macOS binary (its release has Windows and Linux
    archives); a macOS build has to come from the Vulkan SDK or be built from source.
  - **SPIRV-Cross to MSL: yes**, with the pinned SPIRV-Cross (`6c09849`): `spirv_msl.cpp` line 17165 emits
    `raytracing::acceleration_structure<raytracing::instancing>` and line 17172
    `raytracing::intersection_query<raytracing::instancing, raytracing::triangle_data>`; ray flags become an
    `intersection_params` helper. The acceleration structure is listed in
    `ShaderResources::acceleration_structures` and takes a buffer slot (`MSLResourceBinding::msl_buffer`); with MSL
    3.0 the output compiles with `xcrun metal` for macOS 14 and iOS 26. The metallib ran on the M4 Pro
    (`supportsRaytracing` 1) under `MTL_DEBUG_LAYER=1`: one triangle in a primitive acceleration structure, one
    instance in an instance acceleration structure, bound with `setAccelerationStructure:atBufferIndex:` plus
    `useResource:usage:` for the primitive structure; the committed distance was 1.0 as expected.
  - Metal APIs the output needs: MSL 2.3+ `<metal_raytracing>` (`intersection_query`, `acceleration_structure`,
    `intersection_params`), the `MTLAccelerationStructure` objects and their command encoder; Apple's Metal Shader
    Converter (DXIL -> metallib) is not needed, and its licence is not one of the permitted ones.
  - Consequence for the second half: ray tracing shaders need DXC (or another HLSL compiler with ray queries) for the
    SPIR-V step on the Mac; `quench_shaderc` keeps glslang for everything else, maps acceleration structures to a
    buffer slot after the storage buffers and requires SPIR-V 1.4 / Vulkan 1.2 for those entries.

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
