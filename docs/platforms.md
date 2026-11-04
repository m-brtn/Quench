# Platforms

What has run where, the GPU backend and shader format per platform, and the known constraints. Facts were checked on
2026-11-04 against SDL 3.4.18 (the pin both games use) and the SDL repository. All documentation: [index](README.md).

## Priorities

macOS and iOS first: the available hardware is a Mac with an Apple M4 Pro and an iPhone 18 Pro Max. Windows, Linux,
Android and the web are later targets; the portability rules in [AGENTS.md](../AGENTS.md) keep them reachable.

## Status

The engine itself (stage 1, headless only) builds and passes its tests on macOS (arm64; also under the address,
undefined-behaviour and thread sanitizers), and its libraries build for the iOS Simulator and iOS devices; nothing of
it has run on iOS or on a GPU yet. Its shaders are compiled to SPIR-V and macOS / iOS metallibs, but not loaded by any
GPU. The table shows what the games have run, which is what the engine code starts from.

| Platform | SDL GPU backend | Shader format | Run so far (games) |
|---|---|---|---|
| macOS | Metal | metallib | TrailForge and Ashfall |
| iOS | Metal | metallib (iOS SDK) | TrailForge in the iOS Simulator; on the user's iPhone (user reports 2026-11-02 and 2026-11-04) |
| Windows | D3D12 or Vulkan | DXIL or SPIR-V | TrailForge builds and passes headless tests in CI; never run on real hardware |
| Linux | Vulkan | SPIR-V | TrailForge builds in CI and starts on Mesa lavapipe under Xvfb; never run on real hardware |
| Android | Vulkan | SPIR-V | nothing |
| Web | WebGPU (not in SDL yet) | WGSL | nothing |

TrailForge also ran on Vulkan through MoltenVK on the Mac as a test of the SPIR-V path. Ashfall has only run on macOS;
its CMake files keep the SPIR-V and DXIL outputs from TrailForge, unbuilt elsewhere.

## macOS

- Deployment target 14.0 in both games; the metallibs are compiled for the same minimum version.
- GPU time is measured through SDL's internal Metal fence struct (`GpuTimingMetal.m`), tied to the SDL 3.4.18 layout.
  A native Metal backend replaces this hack.

## iOS

- TrailForge presets: `host-tools` (host shader compiler), `ios-sim-release`, `ios-device-release`, `ios-xcode`; iOS
  26+; patches for SDL (Simulator Metal device) and ImGui (MSL shaders on iOS) in `cmake/patches/`.
- The user reported on 2026-11-04 that TrailForge plays well on the iPhone. BC7 support on that device is not
  confirmed explicitly: TrailForge silently drops BC7 level models and terrain textures when the GPU lacks BC7 (log
  lines "level models unavailable" and "BC7 textures are not supported by this GPU"). Check those log lines on the
  device before relying on BC7 there.
- ASTC is supported by every iPhone GPU that runs iOS 26.

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
