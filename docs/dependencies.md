# Dependencies

Libraries the engine starts from (the pins of the games) and the planned additions. Licences and capabilities were
checked on 2026-11-04 in the libraries' repositories. All documentation: [index](README.md). The dependency rules
(pinning, permissive licences, `THIRD_PARTY_LICENSES`) are in [AGENTS.md](../AGENTS.md#dependencies).

## Carried over from the games

All are fetched as GitHub archives of a fixed commit with a SHA256 hash. Pinned in the engine so far, with
TrailForge's commits and hashes: Jolt Physics, SDL3, GLM and Dear ImGui (`cmake/Dependencies.cmake`; SDL and ImGui
only when `QUENCH_HEADLESS` is off), the shader toolchain (`cmake/ShaderToolchain.cmake`) and, since stage 6,
meshoptimizer, bc7enc_rdo and stb; stage 6 also pins enkiTS, xxHash and fastgltf, which no game uses (see "Added in
stage 6"); stage 7 pins miniaudio at TrailForge's commit (see "Added in stage 7"). cgltf is not pinned in the engine
(it is replaced by fastgltf in the cooker); its SHA256 is in TrailForge's `cmake/Dependencies.cmake`. The engine builds ImGui with its SDL3 platform backend only and draws it through
`quench::gpu` (`src/app/ImGuiRenderer.cpp`), so ImGui's SDL GPU renderer backend and TrailForge's iOS patch for it are
not needed. GLM is configured with `GLM_FORCE_DEPTH_ZERO_TO_ONE` and `GLM_FORCE_RADIANS`; it is offered under the
Happy Bunny License or MIT, and the engine uses it under MIT.

| Library | Version | Commit | Licence | Used by | Purpose |
|---|---|---|---|---|---|
| SDL3 | 3.4.18 | `829a65d769d935c4852f8159e964312c0957260a` | zlib | both | window, input, audio device, SDL GPU, IO streams |
| Jolt Physics | 5.6.0 | `e77f175595e64cb44218cc9d9d56fc365ad0e36a` | MIT | both | physics |
| Dear ImGui | 1.92.9b | `f1cc2ae15e53a861a874c3034aae6798fde194ab` | MIT | both | debug UI, menus |
| GLM | 1.0.3 | `8d1fd52e5ab5590e2c81768ace50c72bae28f2ed` | MIT | both | math |
| miniaudio | 0.11.25 | `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d` | public domain or MIT-0 | TrailForge; the engine since stage 7 | TrailForge: audio device, WAV reading and offline WAV capture (its mixer is its own); the engine: see "Added in stage 7" |
| cgltf | 1.15 | `360db1a95480fe102ae9c69b27c5d101167ff5ba` | MIT | TrailForge | glTF loading |
| meshoptimizer | 1.3 | `9e1f07b159d3cb777f1c67ed31fc11fd117986f4` | MIT | TrailForge | mesh optimization and simplification |

Patches applied by TrailForge (`cmake/patches/`): `SdlIosSimulator.cmake` (in the iOS Simulator only: skips SDL
GPU's Metal GPU family check and its `setDepthClipMode` calls) and `ImguiIos.cmake` (ImGui's SDL GPU backend uses MSL
source on iOS instead of the macOS metallibs). The engine carries `SdlIosSimulator.cmake` since stage 2 (the same two
changes, ported from TrailForge `0188a3a`); compiled for the Simulator it leaves SDL's `SDLToMetal_DepthClipMode`
unused, which SDL's build reports as one warning. Ashfall has no iOS build and no
patches. Ashfall plays audio through an SDL3 audio stream with sounds synthesized at startup, without miniaudio.

Build-time shader toolchain (host tools, not shipped in the binary):

| Tool | Commit or version | Licence | Purpose |
|---|---|---|---|
| SPIRV-Headers | `29981f65241605e08b0ede4cfeb999fe3b723c6a` | MIT | SPIR-V definitions |
| SPIRV-Tools | `9a49b0883b9b635689a85b5647dbfcb223268151` | Apache-2.0 | legalization of glslang's HLSL output |
| glslang | `168d452a4f460d24b588fed08477a81c44ee27a1` | BSD-3-Clause, BSD-2-Clause, MIT, Apache-2.0 | HLSL -> SPIR-V |
| SPIRV-Cross | `6c09849fe88c48eaed08413aa022aaa136a3a057` | Apache-2.0 | SPIR-V -> MSL |
| DXC | 1.9.2609 (`dxc_2026_09_29.zip`) | NCSA | HLSL -> DXIL, Windows hosts only |
| `xcrun metal` | Xcode | Apple | MSL -> metallib |
| Emscripten | 6.0.13 (Homebrew, not pinned) | MIT or NCSA; Apache-2.0 (with LLVM exception) parts | the `wasm-node` web check; needs Python 3.10+ |
| Node.js | 26.13.0 (Homebrew, installed with Emscripten) | MIT | runs the WebAssembly tests |
| Vulkan loader, MoltenVK | 1.4.363.0, 1.4.2 (Homebrew `vulkan-loader`, `molten-vk`, not pinned) | Apache-2.0 | the `gpu_vulkan` tests on the Mac |
| Khronos validation layer | 1.4.363.0 (Homebrew `vulkan-validationlayers`, not pinned) | Apache-2.0 | validation in the `gpu_vulkan` tests |

TrailForge's texture and model compiler (`tools/texc`, host tool) also uses stb
(`003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5`) and bc7enc_rdo (`b9438627eef73a1157e84201b6fa6eb2ffd6d9f0`).

## Added in stage 6

The job system, the asset runtime and the asset cooker ([Jobs, assets and the cook](assets.md)). Runtime libraries are
in `cmake/Dependencies.cmake` and listed with their licence texts in `THIRD_PARTY_LICENSES`; the cooker's libraries are
in `cmake/CookToolchain.cmake`: every configure for the host downloads them (also when nothing is cooked; not cross
builds given a `QUENCH_HOST_COOK`, not the web build), and they are built only where something cooks (a
`quench_cook_assets()` call, the cook tests, the `host-tools` preset).

| Library | Version | Commit | Licence | Linked into | Purpose |
|---|---|---|---|---|---|
| enkiTS | 1.12 | `0289cf6ffce86697e1f46cb4eeca1c946bffec71` | zlib | `quench_core` | `quench::JobSystem` (tasks, dependencies, parallel-for); Jolt's jobs |
| xxHash | 0.8.4 | `c87183a77d67f7d37e3d2d1b7eaac5e7c695e4f0` | BSD-2-Clause | `quench_assets` | asset IDs (XXH64 of the path), header only |
| meshoptimizer | 1.3 | `9e1f07b159d3cb777f1c67ed31fc11fd117986f4` (TrailForge's) | MIT | `quench_assets`, `quench_cook` | decoding cooked vertex and index streams; optimization, quantization, LOD simplification and encoding in the cooker |
| bc7enc_rdo | none (commit) | `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0` (TrailForge's) | MIT or public domain; `bc7e.ispc` Apache-2.0 (not used) | `quench_assets` (`bc7decomp.cpp`), `quench_cook` (`bc7enc.cpp`) | BC7 decoding where a device cannot sample BC7; BC7 encoding in the cooker |
| fastgltf | 1.0.0 | `0b800b4a584704f9e9add67c79c4c8732e552787` | MIT | `quench_cook` only | glTF and GLB import |
| simdjson, simdutf | 5.1.0, 9.3.3 | release files, SHA256 checked by fastgltf | Apache-2.0 or MIT | `quench_cook` only | fastgltf's JSON parser and UTF conversion |
| stb (`stb_image.h`) | none (commit) | `003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5` (TrailForge's) | MIT or public domain | `quench_cook` only | PNG and JPEG decoding |

None of the pins of the games changes: TrailForge keeps its own meshoptimizer, stb and bc7enc_rdo pins (the same
commits), Ashfall uses none of these libraries.

## Added in stage 7

Audio ([Audio](audio.md)). One library, in `cmake/Dependencies.cmake`, listed with its licence in
`THIRD_PARTY_LICENSES`; every configure downloads it (headless and web builds included, which use its decoders).

| Library | Version | Commit | Licence | Linked into | Purpose |
|---|---|---|---|---|---|
| miniaudio | 0.11.25 | `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d` (TrailForge's), SHA256 `1a3a79b80fc6f0b0cc155e28b954a598e0ddfa2db64e2afa8466be88c476fa55` | public domain (Unlicense) or MIT-0 | `quench_audio` (decoders), `quench_audio_device` (device API), `quench_cook` (through `quench_audio`) | WAV, FLAC and MP3 decoding; the playback device (Core Audio on Apple platforms, a null backend for tests) |

How it is built (`quench_miniaudio`, a static library from `miniaudio.c`): the engine, node graph, resource manager,
waveform generation and encoders are compiled out (`MA_NO_ENGINE`, `MA_NO_NODE_GRAPH`, `MA_NO_RESOURCE_MANAGER`,
`MA_NO_GENERATION`, `MA_NO_ENCODING`; the engine writes WAV itself, `quench::audio::encodeWav`). Headless and web
builds add `MA_NO_DEVICE_IO` (the web build also `MA_NO_THREADING`). On Apple platforms only the Core Audio and null
backends are compiled, with the frameworks linked at build time (`MA_NO_RUNTIME_LINKING`, miniaudio's advice for
App Store builds); on iOS `miniaudio.c` is compiled as Objective-C (its backend uses `AVAudioSession`).

Checked in the 0.11.25 source and in the tests: the decoders read WAV (16-bit PCM and 32-bit float), FLAC and MP3
from memory (`audio.decode_wav_flac_mp3`); streams read through a miniaudio VFS over `quench::fs` readers
(`audio.stream_*`, tested with WAV files only). The MP3 decoder (dr_mp3) does not
remove the encoder delay and padding of the test file (12672 frames decoded for 11025 encoded at 22050 Hz), so
gapless loops should be WAV or FLAC. The null backend runs a device thread that discards the mix
(`audio_device.null_device`).

**Games.** TrailForge builds its own `miniaudio_lib` from the same commit with another configuration (encoder on,
FLAC off, every backend). When it moves to an engine with `quench_audio` (0.5.0), both libraries end up in its link
(`quench_app` links `quench_audio` and `quench_audio_device`). Checked with a link experiment on the Mac (two static
archives of `miniaudio.c` with the two configurations, outside both repositories): the linker loads one of the two
objects for every `ma_*` symbol, without an error or a warning, so engine code compiled against one configuration
may call the other (no FLAC; different device and context structures). TrailForge therefore has to drop
`miniaudio_lib` and use the engine's audio (or link `quench_miniaudio`) when it moves to 0.5.0; its WAV capture uses
`ma_encoder_*`, which the engine's build does not have (`quench::audio::encodeWav` or `--audio-capture` replace it).
Ashfall does not use miniaudio.

## Planned

Added only when the stage that needs them starts ([Roadmap](roadmap.md)). Latest releases as of 2026-11-04.

| Library | Latest release | Licence | Planned use |
|---|---|---|---|
| ozz-animation | 0.17.0 (2026-08-01) | MIT | skeletal animation runtime and offline tools |
| KTX-Software (libktx) | 4.4.2 (2025-10-04) | Apache-2.0, with exceptions (see below) | Basis Universal transcoding, ASTC encoding (stage 6 writes and reads KTX2 itself, BC7 and RGBA8 only) |
| Basis Universal | 2_50 (2026-08-03) | Apache-2.0 | supercompressed textures for Android and the web (bundled in libktx) |
| Recast & Detour | 1.6.0 (2023-05-21) | zlib | navmesh building, path queries, crowds |
| Tracy | 0.14.1 (2026-08-22) | BSD-3-Clause | CPU and GPU profiler (development builds) |
| Flecs | 4.2.0 (2026-11-04) | MIT | optional ECS module for games that want one |

Already pinned and kept: Dear ImGui (diagnostics). miniaudio was pinned with stage 7 (above). Vorbis decoding
(`stb_vorbis`) is not planned until a game needs `.ogg` files.

## Verified capabilities and caveats

- **ozz-animation**: the runtime has sampling, blending with per-joint weights, `track_triggering_job` (events from
  user channel tracks), `ik_two_bone_job` and `ik_aim_job`; the samples include additive blending, partial blending,
  attachments, foot IK, two-bone IK, user channels, motion extraction and skinning. It is tested on WebAssembly and its
  runtime needs C++17. It has no state machine, no retargeting and no GPU skinning: the engine implements those.
- **fastgltf**: current versions require C++20 (`cxx_std_20` in its CMake); C++17 support ended with the 0.9 series.
  The engine is C++20, so this is fine. Checked in the 1.0.0 source: unless a `simdjson` or `simdutf` package is
  found, its configure downloads their single-file releases (5.1.0, 9.3.3) from GitHub with `EXPECTED_HASH SHA256`;
  `cmake/CookToolchain.cmake` disables `find_package` for both, so only the pinned files are used. Their headers
  trigger about 200 `-Wundef` warnings, so the cooker's third-party targets build without the engine warnings.
- **KTX-Software**: bundles Basis Universal (with zstd inside it), astc-encoder, dfdutils and an ETC decoder in
  `external/`. Most files are Apache-2.0, but `external/etcdec/etcdec.cxx` is not open source (Ericsson licence):
  configure libktx with `LIBKTX_FEATURE_ETC_UNPACK=NO` so it is not built. Apache-2.0 NOTICE files must be shipped.
- **Jolt Physics 5.6.0** (pinned commit): provides `JobSystemWithBarrier` (base for a custom job system),
  `JobSystemSingleThreaded` and `JobSystemThreadPool`. The engine job system implements `JobSystemWithBarrier` on
  enkiTS. Single-threaded, Jolt runs dependent jobs nested on the caller's stack: under Emscripten one `step()` of a
  sphere on a box overflowed the 64 KB default stack by about 12 KB. `quench_physics` therefore links WebAssembly
  programs with `-sSTACK_SIZE=1048576`, the value Jolt uses for its own WebAssembly programs.
- **enkiTS**: `ProfilerCallbacks::threadStart` runs on each worker at start; the engine calls
  `raiseWorkerThreadPriority()` there. `WaitforTask` runs other tasks while it waits, so a wait on the frame thread
  helps instead of blocking. Built from its one source file (`src/TaskScheduler.cpp`) with three task priorities.
- **SDL GPU**: no ray tracing, no GPU timestamps, no native handles (see
  [Architecture](architecture.md#graphics-layer)). Checked in the 3.4.18 source: device creation needs SDL's video
  subsystem for every driver; parameter checks (mipmap generation, texture format support) run only in its debug
  mode; the Vulkan backend requests `depthClamp` but not `depthBiasClamp`, so the bias clamp is zeroed there; the
  `SDL_GPU_DRIVER` hint overrides the driver name a program asks for.
- **SDL on macOS with Vulkan**: SDL looks the Vulkan loader up by bare names, which `dlopen` does not search for in
  Homebrew's `/opt/homebrew/lib`; the tests set `SDL_VULKAN_LIBRARY` to the loader `find_library` found. In its
  debug mode SDL enables `VK_LAYER_KHRONOS_validation` when the loader lists it and fails to create the device if the
  layer then does not load; Homebrew's layer manifest names its library without a path, so the tests also set
  `DYLD_FALLBACK_LIBRARY_PATH`. SDL registers no debug messenger: the layer prints its messages to stdout, and the
  `gpu_vulkan` tests fail on "Validation Error" or "Validation Warning" in the output.
- **SDL revision**: the archive has no `REVISION.txt`, and SDL's configure would take `git describe` of the enclosing
  repository as its revision (`SDL_GetRevision()`); `cmake/Dependencies.cmake` sets `SDL_REVISION` to
  `SDL-3.4.18-829a65d7`.
- **glslang** (pinned commit): its configure step warns that `ENABLE_HLSL` is deprecated and that the HLSL front end
  will be removed in a future major release (KhronosGroup/glslang#4210). The pin keeps working. Before moving the
  glslang pin, check that the HLSL front end is still there; if it is gone, the SPIR-V path needs another HLSL
  compiler on the Mac (DXC can emit SPIR-V; whether it builds and runs on macOS hosts is not checked yet).
- **SPIRV-Cross**: built from an archive without `.git`, its configure step picks up the git hash of the enclosing
  repository (the engine's or a game's) as its version string. Cosmetic; it does not affect the output.
