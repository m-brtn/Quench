# Platforms

What has run where, the GPU backend and shader format per platform, and the known constraints. Facts were checked on
2026-11-04 against SDL 3.4.18 (the pin both games use) and the SDL repository. All documentation: [index](README.md).

## Priorities

macOS and iOS first: the available hardware is a Mac with an Apple M4 Pro and an iPhone 18 Pro Max. Windows, Linux,
Android and the web are later targets; the portability rules in [AGENTS.md](../AGENTS.md) keep them reachable.

## Status

The engine (version 0.2.0) builds and passes its tests on macOS (arm64; also under the address, undefined-behaviour
and thread sanitizers). Its GPU tests render offscreen and check the pixels read back on Metal (Apple M4 Pro, with the
Metal API validation layer), on Vulkan through MoltenVK on the same Mac (with the Khronos validation layer; a test of
the SPIR-V path, not of a real Vulkan driver), in the iOS Simulator (iOS 27.0, iPhone 18 Pro Max; the test executables
run through `simctl spawn`) and on the user's iPhone 18 Pro Max (A20 Pro GPU, iOS 27, Metal with the API validation
layer; all cases in one signed app, `quench_device_tests`, launched through `devicectl`; last run at version 0.1.0).
The sample `hello` runs on the Mac (Metal, and Vulkan through MoltenVK, with capture checks against golden images), in
the iOS Simulator and on the iPhone at 120 Hz; on the iPhone the picture, touch input and rotation were checked by the
user (report of 2026-11-05). The headless modules also pass as single-threaded WebAssembly under Node.js (Emscripten
6.0.13; the cases that need threads are skipped there). Nothing of the engine has run in a browser.

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
  `src/gpu/sdlgpu/SdlGpuTimingMetal.m` in the engine), tied to the SDL 3.4.18 layout. A native Metal backend replaces
  this hack.
- Present modes on Metal: `vsync` and `immediate`; `mailbox` is not supported and falls back to vsync. A window is
  paced to its display (60 Hz on the LG UltraFine 5K used here) even with `immediate`, and then the measured GPU time
  grows to about 15.5 ms (0.6 ms with vsync), probably a drawable wait counted inside the command buffer (not
  verified). Benchmarks therefore use vsync ([Performance](performance.md)).

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
  for BC1 and BC7; no BC7 texture has been sampled on the device yet.
- Phones run at 60 Hz unless Info.plist sets `CADisableMinimumFrameDurationOnPhone` (`quench_add_game()` does); with
  it hello runs at 120 Hz on the iPhone 18 Pro Max.
- The app bundle is read-only and SDL's UIKit delegate keeps the process alive after `SDL_main` returns: the device
  test app calls `std::exit`, and file tests that write next to the executable skip there.
- The iOS Simulator's Metal device (engine GPU tests, 2026-11-05) reports GPU family Apple2 only, no BC textures
  (`Caps::bcTextures` false, ASTC true), no depth clip mode (`Caps::depthClamp` false) and no indirect draws
  (`Caps::indirectDraw` false: Metal validation aborts on them). SDL GPU needs its video subsystem even for offscreen
  work; test executables without `SDL_main` call `SDL_SetMainReady()` first.

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

## Windows and Linux

- Shader formats: SPIR-V (Vulkan) on both, DXIL (D3D12) on Windows. Today DXIL is compiled only on Windows hosts,
  because only the Windows build of DXC is downloaded. DXC also exists for Linux and builds on macOS, so all formats
  could be produced on a Mac.
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
  threaded. Audio starts only after a user gesture. Memory and download size are limited (TrailForge has 272 MB of
  assets in Git LFS).
- Until the WebGPU backend is usable, web compatibility is checked for headless modules only: built with Emscripten
  and run as CTest under Node.js.

## Hardware ray tracing

Available on both devices: Apple GPUs have hardware ray tracing from the M3 and the A17 Pro. Not available through SDL
GPU (see [Architecture](architecture.md#graphics-layer)); planned through the native Metal backend. On the web there
is no ray tracing API.
