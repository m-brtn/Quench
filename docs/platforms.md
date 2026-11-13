# Platforms

What has run where, the GPU backend and shader format per platform, and the known constraints. Facts were checked on
2026-11-04 against SDL 3.4.18 (the pin both games use) and the SDL repository. All documentation: [index](README.md).

## Priorities

macOS and iOS first: the available hardware is a Mac with an Apple M4 Pro and two iPhones, an iPhone 18 Pro Max (A20
Pro) and, since 2026-11-06, an iPhone 17 (A19). Windows, Linux, Android and the web are later targets; the
portability rules in [AGENTS.md](../AGENTS.md) keep them reachable.

## Status

The engine (version 0.16.0) builds and passes its tests on macOS (arm64; also under the address, undefined-behaviour
and thread sanitizers); [the release notes](releases/v0.13.1.md) give the counts of passed and skipped cases (those
of 0.13.1: 0.13.2, a patch of the renderer, ran the render and demo tests only, [its notes](releases/v0.13.2.md);
0.14.0, the weather's look and sound, ran no gate, [its notes](releases/v0.14.0.md); after its tag the golden
captures of the demo's Weather section were renewed, and the Release tests of the Mac's three backends, 818 with 69
skipped, and the web check in headless Chrome on the Mac pass again; 0.15.0, phones in a browser, ran the full
gate and the web check before its version number changed, [its notes](releases/v0.15.0.md); 0.16.0, the user files
of a browser build kept in IndexedDB, ran the release gate, [its notes](releases/v0.16.0.md)).
Its GPU tests render offscreen and check the
pixels read back on Metal (Apple M4 Pro, with the Metal API validation layer), on Vulkan through MoltenVK on the same
Mac (with the Khronos validation layer; a test of
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
Since 2026-11-10 the sample and the demo also build for a browser (the `web` preset, experimental: a WebGPU backend
of the engine's own, see "Web (WebAssembly)" below) and ran in Chrome 157 on the Mac, headless: the demo's batch of
scripted checks 757 of 757 with its 41 captures within the golden image thresholds of the Mac's Metal ones. No other
browser, no phone, and nobody has played it there.
Audio's tests (stage 7, see "Audio" below) have run only on the null backend and offline, on the Mac, in the iOS
Simulator, on the iPhone (the device test app, 2026-11-06) and under Node.js; the user listened to the demo's
weather sounds on the Mac and the iPhone 17 (2026-11-08: fine), to nothing else. Navigation and the weather ran on
the iPhone 17 only by hand, in the demo (the user, 2026-11-08; see "Navigation" and "Weather" below). Animation and GPU
skinning (stage 8, see "Animation" below) ran on the Mac, in the iOS Simulator, on the iPhone (the device test app
and the demo's benchmark, 2026-11-06) and (without rendering) under Node.js. The native Metal backend (stage 9, the
default on Apple platforms since 2026-11-07; see "Native Metal backend" below) ran on the Mac, in the iOS Simulator
and on the iPhone (2026-11-06, as an option; as the default on the Mac, in the iOS Simulator and on the iPhone 17
(2026-11-07), not yet on the iPhone 18 Pro Max;
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
back to nominal). On 2026-11-07 (iOS 27.2 by then) it ran the code of `c42b2c5` with the native backend as the
default: the device tests, hello, the demo's smoke run, its benchmark of every section and its ray tracing and
tunnel phone scenarios, with the same numbers as before ([Performance](performance.md#the-native-default-on-the-screen-and-on-the-iphone-17)).

| Platform | SDL GPU backend | Shader format | Run so far (engine) | Run so far (games) |
|---|---|---|---|---|
| macOS | Metal | metallib | tests, hello and the demo (also Vulkan through MoltenVK) | TrailForge and Ashfall |
| iOS | Metal | metallib (iOS SDK) | tests, hello, the render module and the demo in the Simulator and on the user's iPhone (signed by the user's paid team) | TrailForge in the iOS Simulator; on the user's iPhone (user reports 2026-11-02 and 2026-11-04; on the engine's app shell 2026-11-05) |
| Windows | D3D12 or Vulkan | DXIL or SPIR-V | builds with MSVC on GitHub's runner; tests and the demo's checks on D3D12 over WARP (software); never on real hardware | TrailForge builds and passes headless tests in CI; never run on real hardware |
| Linux | Vulkan | SPIR-V | nothing | TrailForge builds in CI and starts on Mesa lavapipe under Xvfb; never run on real hardware |
| Android | Vulkan | SPIR-V | nothing | nothing |
| Web | none: the engine's WebGPU backend (SDL GPU has none) | WGSL | headless tests under Node.js; hello and the demo with their scripted checks in Chrome on the Mac (headless, WebGPU over Metal), experimental | nothing |

TrailForge also ran on Vulkan through MoltenVK on the Mac as a test of the SPIR-V path. Ashfall has only run on macOS;
its CMake files keep the SPIR-V and DXIL outputs from TrailForge, unbuilt elsewhere.

## macOS

- Deployment target 14.0 in both games; the metallibs are compiled for the same minimum version.
- GPU time is measured through SDL's internal Metal fence struct (`GpuTimingMetal.m` in the games,
  `src/gpu/sdlgpu/SdlGpuTimingMetal.m` in the engine), tied to the SDL 3.4.18 layout. The native Metal backend
  (stage 9, `--gpu-backend metal`, see "Native Metal backend" below) reads the command buffer's own GPU times and
  timestamps per pass; SDL GPU (`--gpu-backend sdl`, no longer the default here since 2026-11-07) keeps the hack.
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
- 3D textures on Vulkan through MoltenVK (2026-11-07, the clouds' noise): MoltenVK's portability subset has no 2D
  views of 3D images, so a 3D texture cannot be a color target there, and SDL GPU created every 3D texture as if it
  could (the engine's SDL patch `SdlVulkan3d.cmake`, [Dependencies](dependencies.md)). The engine draws 3D content
  into the layers of a 2D array and copies them into a sampled 3D texture (`gpu_vulkan.texture_3d_from_layers`), on
  every backend.
- Shader hot reload (2026-11-07, [App shell](app.md#shader-hot-reload)) runs the build's compilers through
  `SDL_CreateProcess` (`posix_spawnp` with non-blocking pipes in SDL 3.4.18): checked on the Mac on SDL GPU's Metal
  driver, Vulkan through MoltenVK and the native Metal backend. Not on iOS (no processes, and the app has no build
  tree) and not on Windows (it builds there, but hot reload has not been run) or Linux (no builds there yet; the
  code takes `\` as a separator too).

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
  before any demo had run. A game's opt-in reaction to it (`App::thermalGovernor()`, since 2026-11-07) has run only
  on the Mac with a fake state (`--thermal-state`, the scenario command `thermal_state`), not on a hot phone; where
  the state is unknown (every platform but Apple's) the reaction stays at nominal and limits nothing.
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
- Basis Universal textures (2026-11-07; [Assets](assets.md#basis-universal)): transcoded at load time to what
  `gpu::Caps` allow. On the Mac SDL GPU's Metal driver and Vulkan through MoltenVK report BC7 and ASTC but no ETC2
  (SDL GPU 3.4.18 has no ETC2 formats; MoltenVK exposes ASTC on Apple silicon), so both codecs become ASTC 4x4; the
  native Metal backend reports ETC2 too (Apple7 GPUs and later on macOS, every GPU on iOS), so ETC1S becomes ETC2
  RGB8 there (`render.scene_assets_basis_textures` and the demo's `assets_basis` part on all three). The same in
  the iOS Simulator (`ios-sim-tests`: the `assets.basis_*` cases and `render.scene_assets_basis_textures` on SDL GPU,
  ASTC 4x4 for both codecs, and on the native backend, ETC1S to ETC2 RGB8; the first transcode of a process, with
  the transcoder's tables, about 115 ms there against about 20 ms on the Mac). Not run on an iPhone yet.
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
the default on macOS, iOS and the iOS Simulator since 2026-11-07 (the user's decision; SDL GPU with
`--gpu-backend sdl` or `QUENCH_GPU_BACKEND=sdl`, and for `--gpu-driver vulkan`). A native device that fails to start
falls back to SDL GPU with one warning. Built for macOS and iOS (device and Simulator); nothing of it exists
elsewhere, and a request for it there creates SDL GPU with a warning. As the default (no flag) it has run on the Mac
(hello and the demo offscreen, hello's benchmark on the screen against SDL GPU: the same GPU time), in the iOS
Simulator (hello and the demo's smoke scenario, 17 of 17, with captures) and on the iPhone 17 (the device tests,
hello, the demo's smoke run and phone scenarios; all 2026-11-07,
[Performance](performance.md#the-native-default-on-the-screen-and-on-the-iphone-17)). The Mac's tests and batches
name their backend; the iPhone 18 Pro Max has run it only when chosen. On the iPhone 17 the user looked at it as
the default in the ray tracing section of an interim build (2026-11-07 17:47, launched without a backend flag:
"the grain is fine now"); on 2026-11-08 the user looked at the Weather and Navigation sections of the demo build of
`b91aff0` there (the report names no backend).

| Where | Run so far |
|---|---|
| Mac, M4 Pro | `gpu_metal_native.*` and `render_metal_native.*` with the Metal API validation layer; hello and every demo section's capture identical to the Metal golden images in Release (2026-11-06); the four swapchain compositions created and presented; `Caps::gpuPassTiming` true (timestamp period 1.0000 ns); `Caps::rayTracing`, `spatialUpscaling` and `temporalUpscaling` true; the ray tracing section against its own golden image; benchmarks in [Performance](performance.md#measurements) |
| iOS Simulator (iPhone 18 Pro Max) | `gpu_metal_native.*` and `render_metal_native.*` in `ios-sim-tests` (the two window cases skipped as on SDL GPU: no app lifecycle in a test executable; since the second half also the ray tracing and upscaling cases); `Caps::gpuPassTiming` false (the Simulator's GPU has no stage boundary sampling), no depth clamp, no indirect draws, as on SDL GPU there; `rayTracing`, `spatialUpscaling`, `temporalUpscaling` false |
| iPhone 18 Pro Max | `quench_device_tests` with the Metal API validation layer, 2026-11-06: 198 cases, 196 passed, 0 failed, 2 skipped (as on SDL GPU: the read-only bundle, the opt-in audio output), the window cases and the ray tracing and upscaling cases included; `Caps::gpuPassTiming`, `rayTracing`, `spatialUpscaling`, `temporalUpscaling` true; the demo's smoke run (15 of 15) and benchmarks ([Performance](performance.md#measurements)); not looked at on the screen yet. Builds of the Xcode generator before that day's fix crash at the first copy pass (see below) |
| iPhone 17 (A19) | `quench_device_tests` with the Metal API validation layer, 2026-11-06: the stage 10 code 198 cases, 196 passed, 0 failed, 2 skipped; the stage 11 code 215 cases, 213 passed, 0 failed, 2 skipped (on SDL GPU 193 passed, 22 skipped: the ray tracing and MetalFX cases); the same caps as the iPhone 18 Pro Max; the demo's benchmarks and ray tracing runs ([Performance](performance.md#stage-11-on-the-iphone)); as the default (2026-11-07, iOS 27.2, `c42b2c5`, `MTL_DEBUG_LAYER=1 QUENCH_TEST_GPU_DRIVER=metal` without `QUENCH_GPU_BACKEND`): 262 cases, 258 passed, 0 failed, 4 skipped, every device native (SDL GPU by the variable: 232 passed, 0 failed, 30 skipped); hello and the demo's smoke run on it without a flag; not looked at on the screen |

- **ARC in Xcode builds** (found on the iPhone, fixed 2026-11-06). The backend's Objective-C++ keeps Metal objects as
  strong members of plain C++ structs and needs ARC. It was enabled only by `-fobjc-arc` under
  `$<COMPILE_LANGUAGE:OBJCXX>`, which the Xcode generator drops; Ninja builds (the Mac, the Simulator,
  `ios-device-release`) had it, the `ios-xcode` builds that go onto the iPhone did not. There the command buffer
  from `[m_queue commandBuffer]` was freed with its autorelease pool, and the first copy pass crashed with
  `EXC_BAD_ACCESS` (the device tests at `buffer_round_trip`, the demo in `Renderer::create`). The 0.8.0 iOS package
  (`tools/package_demo.sh --ios`) has this crash with `--gpu-backend metal`; on SDL GPU (its default) it works.
  Since the fix `quench_configure_target` sets `-fobjc-arc` and `XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC` on every
  engine target, and every engine Objective-C(++) file stops with `#error` when it is compiled without ARC.
- **The window cases' one failure on the iPhone 18 Pro Max** (2026-11-06, a portrait drawable in a landscape
  window) came from the user handling the phone during the run (the user's report, 2026-11-06); they passed on SDL
  GPU 5 s later and on the native backend 2 minutes later. Nothing open.

## Audio

The playback device is miniaudio's ([Audio](audio.md)). Its 0.11.25 source has backends for WASAPI, DirectSound and
WinMM (Windows), Core Audio (macOS, iOS), PulseAudio, ALSA, JACK, sndio and OSS (Linux, BSD), AAudio and OpenSL ES
(Android), Web Audio (Emscripten, with AudioWorklets behind `MA_ENABLE_AUDIO_WORKLETS`) and a null backend. The
engine compiles only Core Audio and null on Apple platforms, no device I/O in headless builds (`wasm-node` among
them) and only Web Audio in a browser build (the `web` preset).

| Where | Output | Run so far |
|---|---|---|
| Mac | Core Audio (miniaudio, frameworks linked at build time) | tests on the null backend and offline (`audio.*`, `audio_device.null_device`, the demo's audio check with `--audio-capture`); the default output not opened by any test (`audio_device.default_output` is opt-in); the demo's weather sounds (rain, wind, thunder) listened to by the user (2026-11-08: fine), nothing else |
| iOS Simulator | Core Audio; the engine sets the `AVAudioSession` category and activates the session when a real output opens (the null backend leaves the session alone) | `ios-sim-tests`: the audio cases, `audio_device.null_device` and `audio_device.audio_session_category` (sets the playback and ambient categories, reads them back and reads the route); no test opened a real output, so activation, interruptions and route changes did not run; not listened to |
| iPhone | as in the Simulator | the device test app, 2026-11-06: the audio cases, `audio_device.null_device` and `audio_device.audio_session_category` (route: the speaker) passed on both GPU backends; the demo's audio section on the null backend; no test opened a real output. On the iPhone 17 the user listened to the demo's weather sounds (rain, wind, thunder; 2026-11-08: fine), nothing else. Needs the user: what plays with the Ring/Silent switch on silent (the default `Ambient` category is silenced), interruptions (a call, Siri, an alarm), route changes (headphones, Bluetooth), background and foreground |
| Web | under Node.js none (`MA_NO_DEVICE_IO`); in a browser (the `web` preset) miniaudio's Web Audio backend through a `ScriptProcessorNode`, mixing on the main thread | the mixer, decoders and analysis under Node.js (`wasm-node`, single-threaded; the thread stress case is skipped). In a browser the output opens (Chrome on the Mac, 2026-11-10: 48 kHz, periods of 512 frames) and starts only after a user gesture; the scripted checks mix offline. Nobody has listened to it; no null device there (`--audio-device null` is not available, `--no-audio` is) |
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

## Navigation

`quench_navigation` is headless and portable C++ on Recast & Detour, with no platform code; its tile builds run on
the job system, and inside `NavMesh::update()` where there are no worker threads ([Navigation](navigation.md)).

| Where | Run so far |
|---|---|
| Mac | `navigation.*` and the navmesh cook cases in `headless`, `debug`, `release`, `asan` and `tsan`; the demo's Navigation section and its capture on SDL GPU's Metal driver, the native Metal backend and Vulkan through MoltenVK (offscreen) |
| iOS Simulator | `ios-sim-tests`: `navigation.*` |
| iPhone 17 | the demo's Navigation section by hand (the user, 2026-11-08, the demo build of `b91aff0`): looked at in motion, "yes, everything is fine"; the tests not run there, nothing measured |
| iPhone 18 Pro Max | nothing |
| Web | `navigation.*` under Node.js (`wasm-node`, single-threaded: the builds run inside `update()`); no rendering |
| Windows, Linux, Android | nothing |

`quench_weather` is headless and portable C++ (the field is a header shared with the shaders, the wind's gusts and
lightning included); the clouds, the rain, the snow, the plants' sway (`quench/Wind.hlsli`) and the lightning's light
are `quench_render`'s, with the 2D layer as the clouds' fallback by format support, `WeatherAudio` (the wind and the
thunder included) is `quench_audio`'s and `WindDrag` `quench_physics`' ([Weather](weather.md#platforms)).

| Where | Run so far |
|---|---|
| Mac | `weather.*` in `headless`, `debug`, `release`, `asan` and `tsan`; `render.renderer_clouds_shadow_the_ground` and the GPU cases `texture_3d_from_layers` and `cube_faces`, `render.renderer_rain_wets_the_open_ground`, `renderer_rain_shafts_from_afar`, `renderer_snow_covers_the_open_ground`, `renderer_snow_shafts_from_afar`, `renderer_wind_sways_plants` and `renderer_lightning_lights_the_storm` on SDL GPU's Metal driver, the native Metal backend and Vulkan through MoltenVK; the demo's Weather section, its `weather`, `rain`, `snow` and `wind` parts and their fourteen captures and the `physics_wind` part on all three (offscreen); `audio.weather_*` (the mix, the wind, the thunder and the default sounds, by measurement); `physics.physics_wind_drag`; the costs on the native backend (offscreen) |
| iOS Simulator | `ios-sim-tests`: `weather.*`, `physics.physics_wind_drag` and the GPU and render cases on both backends |
| iPhone 17 | the demo's Weather section by hand (the user, 2026-11-08, the demo build of `b91aff0`): the clouds, the rain, the snow and the wind looked at in motion, the rain, the wind and the thunder listened to, "yes, everything is fine"; the tests and the phone defaults' cost not run there |
| iPhone 18 Pro Max | nothing |
| Web | `weather.*` and `physics.physics_wind_drag` under Node.js (`wasm-node`); no rendering |
| Windows, Linux, Android | nothing |

`quench_particles` is headless and portable C++ (the motion is a header shared with the shaders); the particles'
step by compute, the CPU fallback and the draws are `quench_render`'s, chosen by `gpu::Caps::computeShaders`
([Particles](particles.md#platforms)).

| Where | Run so far |
|---|---|
| Mac | `particles.*` in `headless`, `debug`, `release`, `asan` and `tsan`; `render.renderer_particles_*`, `renderer_trails_*` and `renderer_particle_slots_are_limited` on SDL GPU's Metal driver, the native Metal backend and Vulkan through MoltenVK; the demo's Particles section, its `particles` part and its two captures on all three (offscreen); the costs on the native backend (offscreen) |
| iOS Simulator | `ios-sim-tests`: `particles.*` and the render cases on both backends |
| iPhone 17, iPhone 18 Pro Max | nothing |
| Web | `particles.*` under Node.js (`wasm-node`); no rendering |
| Windows, Linux, Android | nothing |

## Windows and Linux

- Shader formats: SPIR-V (Vulkan) on both, DXIL (D3D12) on Windows. Today DXIL is compiled only on Windows hosts,
  because only the Windows build of DXC is downloaded for it. Since stage 9 DXC is built from source on macOS for
  the ray query entries (SPIR-V; [Dependencies](dependencies.md#added-in-stage-9)), so all formats could be
  produced on a Mac; DXIL from the Mac is not set up.
- Windows (since 2026-11-09, the user's request): `.github/workflows/windows.yml` builds the engine, the sample, the
  demo and the tests with MSVC 14.51 (`/W4 /WX`, Ninja, the static runtime; the `windows-release` preset) on
  GitHub's `windows-2025` runner, on a push to the branch `windows-build` or by hand, in about an hour. It is a
  build, not a gate: the engine's gates stay local. On that runner (2026-11-09, commit `c8fb061`): the headless
  tests 228 of 228; the GPU, app and demo tests 180 of 180 with 33 skipped (ray query, MetalFX, window cases); the
  demo's weather, rain, snow, wind, particles, navigation and mix checks, 211 of 211; the packaged demo's smoke run
  22 of 22. Everything graphical ran on SDL GPU's Direct3D 12 driver over the runner's only adapter, Microsoft
  Basic Render Driver (WARP, a software rasterizer), with the Direct3D 12 debug layer (no errors; warnings about
  clear values and comparison samplers). Captures on WARP against the Mac's Metal ones: within the golden image
  thresholds, except the panel width in `demo_ui` (the adapter's longer name) and the rain's splashes and ripples
  (0.1 to 0.45% of the pixels: some land elsewhere). The demo, packaged, is the run's artifact. Not yet run on
  real Windows hardware, on Vulkan on Windows, with ray tracing or with sound.
- What Windows found (2026-11-09): SDL GPU's shader blit, which its Direct3D 12 driver uses for blits and mipmaps,
  pushes uniforms of its own into fragment slot 0 (the SDL GPU backend pushes the engine's again after it); SDL
  GPU took a mip level's size as `size >> level`, 0 for the last levels of a non-square texture (the patch
  `SdlLevelSize.cmake`, [Dependencies](dependencies.md)); `std::this_thread::sleep_for(1 ms)` takes about 15 ms;
  a file still open cannot be removed; MSVC evaluates a call's arguments in another order than Clang (the demo drew
  several random numbers in one call and built other scenes).
- The plan (the user's decisions of 2026-11-09): the build on SDL GPU first (Direct3D 12 by default, Vulkan
  through `--gpu-driver vulkan`, the ray tracing effects on their fallbacks); then a native Vulkan backend with
  ray queries, developed on the Mac through MoltenVK and checked on the user's PC (an NVIDIA RTX GPU); then a
  native Direct3D 12 backend with DXR. The default backend on Windows is chosen later from measurements on that PC;
  until then (stage 15 part 4 of the [roadmap](roadmap.md), added 2026-11-10) the native backends are chosen with
  `--gpu-backend`, and a plain launch stays on SDL GPU without ray tracing.
- Linux: no builds yet.

## Android

- SDL ships a Gradle project template (`android-project/` in the SDL sources).
- SDL GPU's Vulkan backend rejects a device that lacks any of these Vulkan 1.0 features: `independentBlend`,
  `imageCubeArray`, `depthClamp`, `shaderClipDistance`, `drawIndirectFirstInstance`, `sampleRateShading`,
  `samplerAnisotropy` (`VULKAN_INTERNAL_IsDeviceSuitable` in `src/gpu/vulkan/SDL_gpu_vulkan.c`). Older and budget GPUs
  may fail this check.
- Assets live inside the APK: `std::ifstream` cannot read them, SDL_IOStream can (hence `quench::fs`).
- BC formats are rare on Android GPUs; ASTC and ETC2 are the norm. Basis Universal files serve them from one set
  (built 2026-11-07); ETC2 needs a GPU backend that offers it (SDL GPU 3.4.18 does not), and ASTC is optional on
  Vulkan (`textureCompressionASTC_LDR`): a device with neither would get RGBA8.

## Web (WebAssembly)

Experimental since 2026-11-10 (the user's request; the `web` preset): the engine, the sample and the demo as
WebAssembly on the browser's WebGPU. Run so far only in Chromium on the Mac (WebGPU over Metal, adapter
`apple metal-3`): Cursor's built-in browser, by hand, and Google Chrome 157 headless, the scripted checks.

- **The GPU backend.** SDL 3.4.18 and SDL `main` have only the Vulkan, D3D12 and Metal backends in SDL GPU. Pull
  request [libsdl-org/SDL#16020](https://github.com/libsdl-org/SDL/pull/16020) ("GPU: Experimental WebGPU SDLGPU
  Backend", open) adds one; it was tried first (its head `5823af8`, on SDL's development branch: `hello` ran on
  it; its bind group layouts, built from the bound resources at draw time, had no comparison samplers or depth
  textures, which the shadow maps need) and
  dropped. The engine has its own WebGPU backend instead (`src/gpu/webgpu`, `gpu::Backend::WebGpu`, the only
  backend of a web build) on Emscripten's `emdawnwebgpu` port (`<webgpu/webgpu.h>`), so SDL stays on its pin; SDL
  still gives the window (the page's canvas), input and the main callbacks.
- **Shaders.** WGSL only: `quench_shaderc --wgsl-spirv` writes each entry's SPIR-V again with the backend's
  bindings, and Tint (Dawn's shader compiler, built from source into the tool cache like DXC;
  [Dependencies](dependencies.md)) translates it at build time. Ray query entries are left out.
- **Capabilities.** No ray tracing (WebGPU has none), no GPU timing, no HDR output, vsync only; compute shaders,
  indirect draws, depth clamp where the adapter has `depth-clip-control`. BC, ETC2 and ASTC textures by the
  adapter's features (Basis Universal files are transcoded to what it has; BC7 in Chrome on the Mac). The renderer
  takes its fallbacks as on any device without ray tracing.
- **Threads.** None: the job system runs with zero workers (a build with threads needs cross-origin isolation, COOP
  and COEP headers on the host; not built). The program is linked with Asyncify: the backend waits for the browser
  when the device starts and when a download or a fence is waited for.
- **What a wait costs.** A wait hands the frame back to the browser, which then expires the canvas's texture: a
  frame that waits after it acquired the swapchain texture loses it (a WebGPU error, one black frame). Downloads
  are therefore mapped when their command buffer is submitted and read between frames; a `map()` after the
  submission's fence does not wait (the rain's cover readback works this way). `Device::download`, `waitIdle` and
  `Fence::wait` inside a frame on the screen still do; captures are taken in offscreen runs.
- **Files.** Assets are preloaded into the virtual file system (`<name>.data`, fetched before `main`). The default
  user directory is kept in the browser's IndexedDB since 2026-11-13 (IDBFS, one database per app named
  `/libsdl/<organization>/<name>`; loaded before the game starts, at most 5 s, stored after each write; in memory
  when IndexedDB is missing, blocked or slow; [App shell](app.md#web-runs)). Checked in headless
  Chrome on the Mac only (`step web`: a second load of the demo reads the settings the first wrote; a run with
  `--user-dir` leaves no database). Not tried: Safari, Firefox, private windows, a phone's browser, quota.
- **Sizes** (Release, 2026-11-10): `hello` 3.3 MB of WebAssembly; the demo 9.8 MB and 9.0 MB of assets. No size or
  load time work was done (no Closure pass, no compression).
- **Known differences and gaps.** Depth textures and, without `float32-filterable`, 32-bit float textures are
  sampled with nearest filtering (WebGPU has no linear filter for them outside comparison samplers); storage
  textures, 3D blit sources, blits into depth textures and line fill mode are not supported (logged once); buffer
  uploads are padded to 4 bytes. The scripted checks did not show a difference from these.
- **Checked** (2026-11-10): `tools/check.sh step web` runs `tools/web_check.mjs`, which serves the build, starts
  headless Chrome and runs the page offscreen ([App shell](app.md#web-runs)): `hello`'s check scenario 21 of 21;
  the demo's `batch.txt` 757 of 757 with no WebGPU error, its 41 captures compared with the Mac's Metal golden
  images at the usual thresholds (the largest: `demo_ui`, 0.066% of the pixels, where the panel names the
  backend); then `hello` for 2 s and the demo's weather section for 4 s on the path of a page a person opens (the
  canvas filling the page, about 57 frames per second in headless Chrome, the Web Audio output opened), without a
  WebGPU error. A game from `templates/game` built with its `web` preset passed its smoke scenario the same way
  (11 of 11). In Cursor's built-in Chromium the demo's batch passed as well, and every section was opened once on
  the screen at 120 frames per second (a 1450 x 1520 canvas).
- **Safari** (2026-11-10, the user's Mac, Safari 27.0.1): the demo's page was black at first, without an error in
  the console. Cause: SDL's probe of the canvas's CSS size under a page zoom (the user's was 115%): the window
  became 0 x 0 and no frame reached the canvas ([Dependencies](dependencies.md)). With a patch of that probe the
  page reported a 3406 x 2318 canvas and 191 frames in 7 s, and the user saw the demo (user report: "it works").
  The patch was then replaced by the app giving the canvas its CSS size before SDL measures it: a CSS `zoom` of
  0.85 on the body gives the same 0 x 0 window in headless WebKit and Chrome without either fix, and a full canvas
  (2000 x 1280 at a pixel ratio of 2) with the app's. The user then opened the 0.13.0 page (GitHub Pages and the
  same build served locally) in Safari: it ran, but in about 1 of 3 to 5 loads the page stayed black until the
  window's size changed (in Chrome never). A script in those pages showed the black ones the same as the others:
  one configuration of the canvas, every `getCurrentTexture` in an animation frame, about 59 frames per second, no
  error, and a copy of the canvas (`drawImage`) as bright as in the shown pages (mean 140.2 to 140.7 of 255). So
  the frames were rendered and Safari did not show them. Since then the WebGPU backend configures the canvas once
  more, one pixel narrower and back, 20 frames after its first configuration
  (`WebGpuDevice::acquireSwapchainTexture`): the user saw none of 9 such pages black (5 opened as new tabs, 4 with
  the same change made by the page's script) and none in two rounds of about ten reloads, against 1 of 4 new tabs
  and 1 to 2 of 5 reloads without it; a small sample, not a proof. No scripted run in Safari itself (its remote automation is off), nothing listened to.
  In Playwright's headless WebKit 27.2 on the Mac (WebKit's engine, not Safari itself) the demo's `batch.txt`
  passed 757 of 757 offscreen; its captures were not compared there.
- **Phones** (2026-11-10 evening, version 0.15.0). The user ran Outpost's page on an iPhone (user report: 30 to 40
  frames per second, and the phone gets warm; which iPhone and browser was not said). Nothing was measured on the
  phone. On the Mac the game's frame at an iPhone 17's size is bound by the GPU's work per pixel, not by the
  WebAssembly side: the page's main thread takes 0.49 ms a frame in headless Chrome on the Mac
  ([Performance](performance.md#a-games-frame-at-a-phones-size)). What a web build lacked for phones, and has
  since: `App::mobile()` is true in a phone's or tablet's browser, so a game's and the demo's phone defaults apply
  there (checked in headless Chrome on the Mac with an iPhone's user agent only); a frame rate limit keeps the
  frames on `requestAnimationFrame` (headless Chrome on the Mac: 480 frames in 8.0 s at a limit of 60 Hz, 181 in
  6.0 s at 30 Hz); and the opt-in reaction to late frames lowers the render scale where a device does not keep up
  ([App shell](app.md#frame-load)), which a browser needs because it reports no thermal state. Not run on a phone
  since: whether they bring the phone to 60 frames per second and keep it cooler is the user's to check.
- **Hosted** (2026-11-10): `tools/publish_web.sh` assembles the demo and the sample as a static site (`dist/web`:
  `index.html` is the demo's page, `hello.html` the sample; `LICENSE`, `THIRD_PARTY_LICENSES` and Emscripten's
  licences beside them), runs the demo's smoke scenario on it in headless Chrome and, with `--push`, commits it
  to the `gh-pages` branch, which GitHub Pages serves: <https://m-brtn.github.io/Quench/>. What is published is
  the local build of a commit (`version.txt` names it), not a build made on GitHub. Beside it, under
  `games/outpost/`, is the page of Outpost, a small experimental game on the engine (the user's request,
  2026-11-10; its source is not in this repository: the untracked `games/outpost` of the user's checkout, published
  by its own `tools/publish_web.sh`): <https://m-brtn.github.io/Quench/games/outpost/>. Its smoke scenario passed
  in headless Chrome on the Mac (16 checks); no report of how it plays in a browser is recorded. The engine's
  script keeps `games/` when it replaces the site.
- **Not checked.** Firefox, Chrome and Safari on other systems and on phones; touch input and the compact layout in
  a browser; sound by ear; long runs, memory growth, a lost device; resizing and fullscreen beyond the page's size
  at load.
- WebGPU feature support quoted in SDL's pull request: BC texture compression on about 86% of adapters, ASTC on
  about 44%, `float32-filterable` about 91%. Memory and download size are limited (TrailForge has 272 MB of assets
  in Git LFS).

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
