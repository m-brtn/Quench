# Platforms

What has run where, the GPU backend and shader format per platform, and the known constraints. Facts were checked on
2026-11-04 against SDL 3.4.18 (the pin both games use) and the SDL repository. All documentation: [index](README.md).

## Priorities

macOS and iOS first: the available hardware is a Mac with an Apple M4 Pro and two iPhones, an iPhone 18 Pro Max (A20
Pro) and, since 2026-11-06, an iPhone 17 (A19). Windows, Linux, Android and the web are later targets; the
portability rules in [AGENTS.md](../AGENTS.md) keep them reachable.

## Status

The engine (version 0.10.0) builds and passes its tests on macOS (arm64; also under the address, undefined-behaviour
and thread sanitizers). Its GPU tests render offscreen and check the pixels read back on Metal (Apple M4 Pro, with the
Metal API validation layer), on Vulkan through MoltenVK on the same Mac (with the Khronos validation layer; a test of
the SPIR-V path, not of a real Vulkan driver), in the iOS Simulator (iOS 27.0, iPhone 18 Pro Max; the test executables
run through `simctl spawn`) and on the user's iPhone 18 Pro Max (A20 Pro GPU, iOS 27, Metal with the API validation
layer; all cases in one signed app, `quench_device_tests`, launched through `devicectl`; last run 2026-11-06 with
the stage 10 code on SDL GPU and on the native Metal backend). The sample `hello` runs on the Mac (Metal, and Vulkan through MoltenVK, with capture checks against
golden images), in the iOS Simulator and on the iPhone at 120 Hz; on the iPhone the picture, touch input and rotation
were checked by the user (report of 2026-11-05). The engine demo runs on the Mac (with capture checks on both
backends), in the iOS Simulator and on the iPhone at 120 Hz, where every section's check passed in portrait and the
user looked at every section, HDR output on and off included (report of 2026-11-05; the seven sections of version
0.3.0; the Assets section of 0.4.0, the Audio section of 0.5.0, the Animation section and the new shadows in the
Materials section of 0.6.0, the Ray tracing section and the native backend of 0.7.0, the compact layout, touch
controls and modes of 0.8.0 not yet; the compact layout was looked at in the iOS Simulator). On 2026-11-06 the
0.8.0 demo ran its benchmark of every section on the iPhone on both backends at 120 Hz, and the ray tracing
benchmark on the native backend ([Performance](performance.md#measurements)); numbers only, nobody looked at the
screen. The headless modules
also pass as single-threaded WebAssembly under Node.js (Emscripten 6.0.13; the cases that need threads are skipped
there).
Nothing of the engine has run in a browser.
Audio (stage 7, see "Audio" below) has run only on the null backend and offline, on the Mac, in the iOS Simulator,
on the iPhone (the device test app, 2026-11-06) and under Node.js; nothing was listened to. Animation and GPU
skinning (stage 8, see "Animation" below) ran on the Mac, in the iOS Simulator, on the iPhone (the device test app
and the demo's benchmark, 2026-11-06) and (without rendering) under Node.js. The native Metal backend (stage 9, not
the default; see "Native Metal backend" below) ran on the Mac, in the iOS Simulator and on the iPhone (2026-11-06;
it crashes in builds of the Xcode generator before the ARC fix of that day, the 0.8.0 iOS package among them). Its
ray tracing and MetalFX (see "Hardware ray tracing" below) ran on the Mac and both iPhones; the stage 11 effects
(0.9.0: local light shadows, reflections, indirect light, the temporal denoiser, MetalFX denoising, the path
tracer) on the Mac and the iPhone 17 (2026-11-06: the device tests and the costs, numbers only; nobody looked at
the screen), not on the iPhone 18 Pro Max.

The iPhone 17 (A19 GPU, GPU family Apple10, iOS 27.0.1, 1206 x 2622 at 120 Hz) ran the stage 9 and 10 code
(`5aea212`, 2026-11-06 16:49-18:01: the device tests on both backends, every section's benchmark, the ray tracing
benchmark and ten minutes of ray tracing) and the stage 11 code (18:27-18:56: the device tests on both backends, the
costs of the stage 11 effects, the ray tracing section's phone defaults for a minute at 120 and at 60 Hz;
[Performance](performance.md#stage-11-on-the-iphone)). It has the same `gpu::Caps` as the iPhone 18 Pro Max (ray
tracing, MetalFX spatial, temporal and the denoised scaler); its GPU and CPU are slower (ray-traced shadows with
occlusion at the native size: about 100 frames per second cold against 120; CPU about 1.5 times), its recommended
working set is 5461 MB (8192 MB on the A20 Pro), and under sustained ray tracing it lowers its clocks within a
minute (about 1.6 times every GPU range from the second minute on; serious after a 65 s benchmark, 5 to 11 minutes
back to nominal).

| Platform | SDL GPU backend | Shader format | Run so far (engine) | Run so far (games) |
|---|---|---|---|---|
| macOS | Metal | metallib | tests, hello and the demo (also Vulkan through MoltenVK) | TrailForge and Ashfall |
| iOS | Metal | metallib (iOS SDK) | tests, hello, the render module and the demo in the Simulator and on the user's iPhone (signed by the user's paid team) | TrailForge in the iOS Simulator; on the user's iPhone (user reports 2026-11-02 and 2026-11-04; on the engine's app shell 2026-11-05) |
| Windows | D3D12 or Vulkan | DXIL or SPIR-V | nothing | TrailForge builds and passes headless tests in CI; never run on real hardware |
| Linux | Vulkan | SPIR-V | nothing | TrailForge builds in CI and starts on Mesa lavapipe under Xvfb; never run on real hardware |
| Android | Vulkan | SPIR-V | nothing | nothing |
| Web | WebGPU (not in SDL yet) | WGSL | headless tests under Node.js | nothing |

TrailForge also ran on Vulkan through MoltenVK on the Mac as a test of the SPIR-V path. Ashfall has only run on macOS;
its CMake files keep the SPIR-V and DXIL outputs from TrailForge, unbuilt elsewhere.

## macOS

- Deployment target 14.0 in both games; the metallibs are compiled for the same minimum version.
- GPU time is measured through SDL's internal Metal fence struct (`GpuTimingMetal.m` in the games,
  `src/gpu/sdlgpu/SdlGpuTimingMetal.m` in the engine), tied to the SDL 3.4.18 layout. The native Metal backend
  (stage 9, `--gpu-backend metal`, see "Native Metal backend" below) reads the command buffer's own GPU times and
  timestamps per pass; while SDL GPU stays the default, the hack stays for it.
- Present modes on Metal: `vsync` and `immediate`; `mailbox` is not supported and falls back to vsync. A window is
  paced to its display (60 Hz on the LG UltraFine 5K used here) even with `immediate`, and then the measured GPU time
  grows to about 15.5 ms (0.6 ms with vsync), probably a drawable wait counted inside the command buffer (not
  verified). Benchmarks therefore use vsync ([Performance](performance.md)). On 2026-11-06 the same Mac and display
  ran `immediate` unpaced: the hello benchmark 4286 frames in 10 s, GPU time 0.62 ms on average; the cause of the
  difference is not known. The demo's checks use `immediate` ([Engine demo](demo.md#checks)): unpaced it halves their
  time, paced it changes nothing.
- SDL GPU's Vulkan driver (seen through MoltenVK; the code is the same for every Vulkan device, and SDL's D3D12
  driver reads the same) shares one reference between a fence from `SDL_SubmitGPUCommandBufferAndAcquireFence` and
  its command buffer. A fence released after it signalled but before SDL's next cleanup pass goes back to SDL's pool
  while the command buffer still points at it; the next submission resets it before that pass, so the command buffer
  and the textures and buffers it used stay in flight, and SDL makes new ones for every cycled render target. The
  demo's assets section grew to 4.5 GB in 6 s this way (2026-11-06). The GPU layer waits on a signalled fence before
  it releases it (`SdlGpuDevice::release`), which runs that cleanup pass first: 676 MB, flat. SDL's Metal driver
  gives the caller a reference of its own and is not affected. Not reported upstream yet.
- Shader hot reload (2026-11-07, [App shell](app.md#shader-hot-reload)) runs the build's compilers through
  `SDL_CreateProcess` (`posix_spawnp` with non-blocking pipes in SDL 3.4.18): checked on the Mac on SDL GPU's Metal
  driver, Vulkan through MoltenVK and the native Metal backend. Not on iOS (no processes, and the app has no build
  tree) and not on Windows or Linux (no builds there yet; the code takes `\` as a separator too).

## iOS

- TrailForge presets: `host-tools` (host shader compiler), `ios-sim-release`, `ios-device-release`, `ios-xcode`; iOS
  26+; patches for SDL (Simulator Metal device) and ImGui (MSL shaders on iOS) in `cmake/patches/`.
- The user reported on 2026-11-04 that TrailForge plays well on the iPhone. BC7 support on that device is not
  confirmed explicitly: TrailForge silently drops BC7 level models and terrain textures when the GPU lacks BC7 (log
  lines "level models unavailable" and "BC7 textures are not supported by this GPU"). Check those log lines on the
  device before relying on BC7 there.
- ASTC is supported by every iPhone GPU that runs iOS 26.
- Engine presets: `host-tools`, `ios-sim-release`, `ios-sim-tests`, `ios-device-release` (Ninja, ad-hoc signed: not
  installable on a device) and `ios-xcode` (Xcode, Automatic signing; what runs on the iPhone). Commands:
  [App shell](app.md#ios-runs).
- iPhone 18 Pro Max (A20 Pro GPU), engine device tests 2026-11-05: `gpu::Caps` metallib, gpuTiming, depthClamp,
  depthBiasClamp, indirectDraw, bcTextures and astcTextures all true. `bcTextures` is SDL's answer to a format query
  for BC1 and BC7. On 2026-11-06 a BC7 texture was sampled on the device for the first time:
  `render.scene_assets_texture_states` passed there on both backends with the texture kept as `Bc7Srgb` (not decoded
  on the CPU) and read back in its color; the native backend reports the same caps plus `gpuPassTiming`.
- Phones run at 60 Hz unless Info.plist sets `CADisableMinimumFrameDurationOnPhone` (`quench_add_game()` does); with
  it hello runs at 120 Hz on the iPhone 18 Pro Max.
- A lower frame rate (`App::setMaxFrameRate`, `--max-frame-rate`): SDL's main callbacks on iOS are called by a
  display link at the display's rate, and the app lets the callbacks pass that come too early. On the iPhone 17 at
  60 Hz (2026-11-06): 1200 frames in each of three 20 s regions, frame p99 16.76 ms.
- The thermal state (`app::ThermalState`, `NSProcessInfo`'s `thermalState`): read every 0.5 s, logged on a change
  and in the perf lines. On the iPhone 17 (2026-11-06) it read serious after the stage 10 baseline's ten minutes of
  ray tracing and came back to nominal 5 to 11 minutes after a load; with the phone on its cable it also read fair
  before any demo had run.
- The app bundle is read-only and SDL's UIKit delegate keeps the process alive after `SDL_main` returns: the device
  test app calls `std::exit`, and file tests that write next to the executable skip there.
- The iOS Simulator's Metal device (engine GPU tests, 2026-11-05) reports GPU family Apple2 only, no BC textures
  (`Caps::bcTextures` false, ASTC true), no depth clip mode (`Caps::depthClamp` false) and no indirect draws
  (`Caps::indirectDraw` false: Metal validation aborts on them). SDL GPU needs its video subsystem even for offscreen
  work; test executables without `SDL_main` call `SDL_SetMainReady()` first.
- Stage 6 in the Simulator (2026-11-05): without BC textures, cooked BC7 textures are decoded to RGBA8 on the CPU at
  load (`render.scene_assets_texture_states` checks the format and the pixels there); the cooked demo assets are made
  by the `host-tools` `quench_cook` and bundled by the iOS presets. On the iPhone (2026-11-06) they were sampled as
  BC7 (see above; the demo logs "BC7 sampled, ASTC sampled" there).
- ASTC 4x4 (2026-11-07): iOS builds cook their textures as ASTC 4x4 (`quench_cook_assets(TEXTURE_FORMAT auto)`), so
  the Simulator samples them instead of decoding BC7 on the CPU. `render.scene_assets_texture_states` samples an
  ASTC texture on every backend that reports `astcTextures` (the Mac's three, the Simulator's two). The demo's
  Assets section with its 21 ASTC textures passed in the Simulator (17 checks, the capture looked at) and on the
  iPhone 17 on both backends (17 checks each, the two captures identical, one looked at: textured tiles and models,
  the broken and the missing model magenta). Not on the iPhone 18 Pro Max.
- Asset hot reload (2026-11-07): development builds read assets from `asset_overlay/` in the user directory
  first, and `tools/push_assets.sh` copies changed files there with `devicectl`
  ([Hot reload on a device](assets.md#hot-reload-on-a-device)). On the iPhone 17 a pushed texture was reloaded
  about a second later. Not tried in the Simulator. `devicectl device copy to --remove-existing-content true`
  deletes files outside its destination folder (it deleted the demo's user directory on the iPhone 17): the
  engine's scripts do not use it.

## HDR output

What the swapchain compositions do per platform and backend, as run on 2026-11-05 (how the app uses them:
[App shell](app.md#hdr-output)). In SDL 3.4.18's sources the Metal backend supports extended linear on macOS 11+ and
iOS and HDR10 (PQ) on macOS only; the Vulkan backend needs `VK_EXT_swapchain_colorspace`.

| Where | SDR, SDR linear, HDR extended linear, HDR10 | Display headroom | HDR checked by |
|---|---|---|---|
| Mac, Metal (M4 Pro, LG UltraFine 5K) | all four supported (`gpu_device.window_swapchain_compositions` creates each and draws frames) | 1.00 (an SDR display) | readback and capture peak: demo HDR section at `--hdr-headroom 4`, peak 3.95 times paper white |
| Mac, Vulkan through MoltenVK | all four supported (`gpu_vulkan.window_swapchain_compositions`, validation layer on) | 1.00 | the same demo check, peak 3.95 |
| iOS Simulator (iPhone 18 Pro Max, iOS 27.0) | extended linear supported (the demo with `--hdr on`: `Rgba16Float` swapchain); the others not queried (the window tests are skipped there) | 1.00 | demo capture peak 2.56 at headroom 4 (portrait framing) |
| iPhone 18 Pro Max (A20 Pro GPU, iOS 27) | extended linear supported (`gpu_device.window_swapchain_compositions` passed in the device test app; the demo with `--hdr on`: `Rgba16Float` swapchain) | read every frame ([App shell](app.md#hdr-output)): potential 8.00; current 1.20 at a start with no EDR content on screen, raised by iOS to 3.05 and then 3.16 within 1.7 s of HDR output, and steady at 3.16 for the next 19 s at an unchanged brightness; lowered from 2.90 to 1.20 about 17 s after EDR content left the screen (before the per-frame reading, SDL's value read once at startup was 1.20 or 8.00) | capture peak 1.20 at the display's 1.20, 3.02 at 8.00, 2.56 at `--hdr-headroom 4`; the user looked at the HDR section, on and off: "everything is fine" (2026-11-05) |

- This Mac's display has no HDR range, so `--hdr auto` stays SDR here and HDR was never seen on a Mac screen: values
  above paper white were checked only in readbacks (render tests) and in the logged capture peak. Looking at HDR on
  an HDR-capable Mac display is a user check; on the iPhone the user has looked (2026-11-05).
- Not run: the output update when a window moves to another display (`SDL_EVENT_WINDOW_DISPLAY_CHANGED`, needs a second
  display), a headroom change from the brightness slider (on the iPhone only the rise after HDR output started was
  seen), and the per-frame reading on a Mac display with HDR range. SDL alone does not refresh the headroom when only
  the brightness changes; the app's own reading on Apple platforms does not depend on it.
- HDR10 swapchains can be created on the Mac, but the renderer does not write PQ: such output gets SDR values with a
  warning ([Render module](render.md#tone-mapping-and-hdr-output)).

## Native Metal backend

The engine's own Metal backend of `quench::gpu` (stage 9, `--gpu-backend metal`, [Architecture](architecture.md#native-metal-backend));
SDL GPU stays the default. Built for macOS and iOS (device and Simulator); nothing of it exists elsewhere, and a
request for it there creates SDL GPU with a warning.

| Where | Run so far |
|---|---|
| Mac, M4 Pro | `gpu_metal_native.*` and `render_metal_native.*` with the Metal API validation layer; hello and every demo section's capture identical to the Metal golden images in Release (2026-11-06); the four swapchain compositions created and presented; `Caps::gpuPassTiming` true (timestamp period 1.0000 ns); `Caps::rayTracing`, `spatialUpscaling` and `temporalUpscaling` true; the ray tracing section against its own golden image; benchmarks in [Performance](performance.md#measurements) |
| iOS Simulator (iPhone 18 Pro Max) | `gpu_metal_native.*` and `render_metal_native.*` in `ios-sim-tests` (the two window cases skipped as on SDL GPU: no app lifecycle in a test executable; since the second half also the ray tracing and upscaling cases); `Caps::gpuPassTiming` false (the Simulator's GPU has no stage boundary sampling), no depth clamp, no indirect draws, as on SDL GPU there; `rayTracing`, `spatialUpscaling`, `temporalUpscaling` false |
| iPhone 18 Pro Max | `quench_device_tests` with the Metal API validation layer, 2026-11-06: 198 cases, 196 passed, 0 failed, 2 skipped (as on SDL GPU: the read-only bundle, the opt-in audio output), the window cases and the ray tracing and upscaling cases included; `Caps::gpuPassTiming`, `rayTracing`, `spatialUpscaling`, `temporalUpscaling` true; the demo's smoke run (15 of 15) and benchmarks ([Performance](performance.md#measurements)); not looked at on the screen yet. Builds of the Xcode generator before that day's fix crash at the first copy pass (see below) |
| iPhone 17 (A19) | `quench_device_tests` with the Metal API validation layer, 2026-11-06: the stage 10 code 198 cases, 196 passed, 0 failed, 2 skipped; the stage 11 code 215 cases, 213 passed, 0 failed, 2 skipped (on SDL GPU 193 passed, 22 skipped: the ray tracing and MetalFX cases); the same caps as the iPhone 18 Pro Max; the demo's benchmarks and ray tracing runs ([Performance](performance.md#stage-11-on-the-iphone)); not looked at on the screen |

- **ARC in Xcode builds** (found on the iPhone, fixed 2026-11-06). The backend's Objective-C++ keeps Metal objects as
  strong members of plain C++ structs and needs ARC. It was enabled only by `-fobjc-arc` under
  `$<COMPILE_LANGUAGE:OBJCXX>`, which the Xcode generator drops; Ninja builds (the Mac, the Simulator,
  `ios-device-release`) had it, the `ios-xcode` builds that go onto the iPhone did not. There the command buffer
  from `[m_queue commandBuffer]` was freed with its autorelease pool, and the first copy pass crashed with
  `EXC_BAD_ACCESS` (the device tests at `buffer_round_trip`, the demo in `Renderer::create`). The 0.8.0 iOS package
  (`tools/package_demo.sh --ios`) has this crash with `--gpu-backend metal`; on the default SDL GPU backend it works.
  Since the fix `quench_configure_target` sets `-fobjc-arc` and `XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC` on every
  engine target, and every engine Objective-C(++) file stops with `#error` when it is compiled without ARC.
- **The window cases' one failure on the iPhone 18 Pro Max** (2026-11-06, a portrait drawable in a landscape
  window) came from the user handling the phone during the run (the user's report, 2026-11-06); they passed on SDL
  GPU 5 s later and on the native backend 2 minutes later. Nothing open.

## Audio

The playback device is miniaudio's ([Audio](audio.md)). Its 0.11.25 source has backends for WASAPI, DirectSound and
WinMM (Windows), Core Audio (macOS, iOS), PulseAudio, ALSA, JACK, sndio and OSS (Linux, BSD), AAudio and OpenSL ES
(Android), Web Audio (Emscripten, with AudioWorklets behind `MA_ENABLE_AUDIO_WORKLETS`) and a null backend. The
engine compiles only Core Audio and null on Apple platforms, and no device I/O in headless and web builds.

| Where | Output | Run so far |
|---|---|---|
| Mac | Core Audio (miniaudio, frameworks linked at build time) | tests on the null backend and offline (`audio.*`, `audio_device.null_device`, the demo's audio check with `--audio-capture`); the default output not opened by any test (`audio_device.default_output` is opt-in), not listened to |
| iOS Simulator | Core Audio; the engine sets the `AVAudioSession` category and activates the session when a real output opens (the null backend leaves the session alone) | `ios-sim-tests`: the audio cases, `audio_device.null_device` and `audio_device.audio_session_category` (sets the playback and ambient categories, reads them back and reads the route); no test opened a real output, so activation, interruptions and route changes did not run; not listened to |
| iPhone | as in the Simulator | the device test app, 2026-11-06: the audio cases, `audio_device.null_device` and `audio_device.audio_session_category` (route: the speaker) passed on both GPU backends; the demo's audio section on the null backend; no real output opened, not listened to. Needs the user: what plays with the Ring/Silent switch on silent (the default `Ambient` category is silenced), interruptions (a call, Siri, an alarm), route changes (headphones, Bluetooth), background and foreground |
| Web | none (built with `MA_NO_DEVICE_IO`) | the mixer, decoders and analysis under Node.js (`wasm-node`, single-threaded; the thread stress case is skipped). Browser output needs a device backend (miniaudio's Web Audio one or SDL's audio) and starts only after a user gesture: not built |
| Windows, Linux, Android | miniaudio's defaults for the platform (not configured or built yet) | nothing |

## Animation

`quench_animation` is headless and portable C++ on ozz-animation, whose SIMD math is SSE and AVX only: on ARM (the
Mac, the iPhone) and WebAssembly it uses its scalar reference math ([Dependencies](dependencies.md#added-in-stage-8)).
GPU skinning is a compute pass through SDL GPU, which has compute on all its backends.

| Where | Run so far |
|---|---|
| Mac | `animation.*`, the cook cases, the render cases (Metal, and Vulkan through MoltenVK with the validation layer), the demo's Animation section and its captures on both; also under the sanitizers |
| iOS Simulator | `ios-sim-tests`: `animation.*` and the render cases with skinning (Metal in the Simulator) |
| iPhone | the device test app, 2026-11-06: `animation.*` and the render cases with skinning, on both GPU backends; the demo's Animation section (51 skinned characters) in the benchmark at 120 Hz, CPU p50 1.47 ms ([Performance](performance.md#measurements)). Needs the user: the section in motion; not run: the crowd of 256 |
| Web | `animation.*` under Node.js (`wasm-node`, single-threaded); no rendering |
| Windows, Linux, Android | nothing |

## Windows and Linux

- Shader formats: SPIR-V (Vulkan) on both, DXIL (D3D12) on Windows. Today DXIL is compiled only on Windows hosts,
  because only the Windows build of DXC is downloaded for it. Since stage 9 DXC is built from source on macOS for
  the ray query entries (SPIR-V; [Dependencies](dependencies.md#added-in-stage-9)), so all formats could be
  produced on a Mac; DXIL from the Mac is not set up.
- No hardware to run them; CI only builds and runs headless tests.

## Android

- SDL ships a Gradle project template (`android-project/` in the SDL sources).
- SDL GPU's Vulkan backend rejects a device that lacks any of these Vulkan 1.0 features: `independentBlend`,
  `imageCubeArray`, `depthClamp`, `shaderClipDistance`, `drawIndirectFirstInstance`, `sampleRateShading`,
  `samplerAnisotropy` (`VULKAN_INTERNAL_IsDeviceSuitable` in `src/gpu/vulkan/SDL_gpu_vulkan.c`). Older and budget GPUs
  may fail this check.
- Assets live inside the APK: `std::ifstream` cannot read them, SDL_IOStream can (hence `quench::fs`).
- BC formats are rare on Android GPUs; ASTC and ETC2 are the norm.

## Web (WebAssembly)

- SDL 3.4.18 and SDL `main` (checked 2026-11-04; latest release 3.4.20 of 2026-11-03) have only the Vulkan, D3D12 and
  Metal GPU backends.
- Pull request [libsdl-org/SDL#16020](https://github.com/libsdl-org/SDL/pull/16020) "GPU: Experimental WebGPU SDLGPU
  Backend" (open, last updated 2026-10-24) adds a WebGPU backend that takes WGSL shaders
  (`SDL_GPU_SHADERFORMAT_WGSL`). Its author reports it working on Linux, Windows and the web (Chrome, Firefox,
  Safari) and lists it as unfinished: not thread safe, sparse error handling, a WGSL parser that does not understand
  all valid WGSL, macOS, iOS and Android not done.
- WebGPU feature support quoted in that pull request: BC texture compression on about 86% of adapters, ASTC on about
  44%, `float32-filterable` about 91%. Textures for the web therefore need Basis Universal transcoding.
- Threads need cross-origin isolation (COOP / COEP headers on the host); otherwise the job system runs single
  threaded. Audio starts only after a user gesture (the engine's audio has no browser output yet: see "Audio").
  Memory and download size are limited (TrailForge has 272 MB of
  assets in Git LFS).
- Until the WebGPU backend is usable, web compatibility is checked for headless modules only: built with Emscripten
  and run as CTest under Node.js.

## Hardware ray tracing

Apple GPUs have hardware ray tracing from the M3 and the A17 Pro, so both devices have it. The engine uses it through
the native Metal backend only (stage 9: acceleration structures, ray queries in fragment shaders,
[Architecture](architecture.md#ray-tracing)); the renderer takes its fallbacks wherever `gpu::Caps::rayTracing` is
false. MetalFX (spatial and temporal upscaling, and since stage 11 the temporal denoised scaler) likewise. Stage 11
added local light shadows, reflections, indirect light, the temporal denoiser and the path tracer on the same
capability ([Render module](render.md#ray-traced-lighting)); they ran on the Mac and the iPhone 17.

| Where | `rayTracing` | MetalFX | Run so far |
|---|---|---|---|
| Mac, M4 Pro, native Metal backend | true | spatial, temporal and temporal denoised | `gpu_metal_native.*` (`ray_tracing_caps`, `ray_query_instances`, `ray_query_refit`, `ray_query_front_faces`, `ray_query_top_level_refit`, `ray_query_compaction`, `upscale_flat_color`, `upscale_denoised_flat_color`), `render_metal_native.renderer_ray_traced_*`, `renderer_upscaling`, `renderer_denoised_upscaling`, `renderer_path_tracer_furnace`, `renderer_indirect_light_*`, `renderer_temporal_ray_denoiser` and `renderer_ray_tracing_refit_and_compaction` under the Metal API validation layer; the demo's ray tracing section against its golden image; the benchmarks in [Performance](performance.md#measurements) |
| Mac, SDL GPU (Metal, Vulkan through MoltenVK) | false | none | the fallbacks: `demo_raytracing_metal` and `demo_raytracing_vulkan` against their golden image. MoltenVK has no Vulkan ray tracing extension (checked at its tag `v1.4.2` and on `main`, 2026-11-06), so Vulkan through it always falls back |
| iOS Simulator, native Metal backend | false (its Metal device does not report both `supportsRaytracing` and `supportsRaytracingFromRender`) | none (MetalFX is not in the Simulator's SDK; the engine does not link it there) | `ios-sim-tests`, 2026-11-06: `gpu_metal_native.ray_tracing_caps` reads 0, 0, 0 and creation fails as it should; the ray tracing and upscaling cases are skipped (263 tests, 22 skipped) |
| iPhone 18 Pro Max (A20 Pro), native Metal backend | true (`supportsRaytracing` and `supportsRaytracingFromRender`; GPU family Apple11, Metal 4) | spatial and temporal (the device also supports the temporal denoised scaler, which the engine uses since stage 11 (not run there yet), and frame interpolation, which it does not use) | 2026-11-06, the stage 10 code: the device test app on the native backend (`ray_tracing_caps`, `ray_query_instances`, `ray_query_refit`, `upscale_flat_color`, the renderer's ray-traced and upscaling cases) under the Metal API validation layer; the demo's ray tracing section and its benchmark at 1320 x 2868 and 120 Hz: RT shadows with occlusion 8.0 ms GPU p50, over the phone's 7.5 ms budget ([Performance](performance.md#measurements)); not looked at on the screen yet |
| iPhone 17 (A19), native Metal backend | true (GPU family Apple10, Metal 4) | spatial, temporal and temporal denoised | 2026-11-06: the stage 10 code's device tests and ray tracing benchmark (RT shadows with occlusion about 100 frames per second cold at 1206 x 2622, 10.2 ms busy), and the stage 11 code's device tests (215 cases, 0 failed) and every stage 11 effect at the native size and scaled ([Performance](performance.md#stage-11-on-the-iphone)): everything traced 34.4 ms, 13.4 ms at 0.5 with MetalFX spatial; MetalFX temporal 3.6 ms and spatial 1.1 ms at 0.5, MetalFX denoising 15.0 ms at 0.67; the demo's phone defaults 117 frames per second cold, 60 Hz held for a minute; not looked at on the screen |

On the web there is no ray tracing API; WebGPU has none so far.
