# Dependencies

Libraries the engine starts from (the pins of the games) and the planned additions. Licences and capabilities were
checked on 2026-11-04 in the libraries' repositories. All documentation: [index](README.md). The dependency rules
(pinning, permissive licences, `THIRD_PARTY_LICENSES`) are in [AGENTS.md](../AGENTS.md#dependencies).

## Carried over from the games

All are fetched as GitHub archives of a fixed commit with a SHA256 hash. The SHA256 values are in TrailForge's
`cmake/Dependencies.cmake`, `cmake/ShaderToolchain.cmake` and `tools/texc/CMakeLists.txt`; the engine copies them
when the CMake files move.

| Library | Version | Commit | Licence | Used by | Purpose |
|---|---|---|---|---|---|
| SDL3 | 3.4.18 | `829a65d769d935c4852f8159e964312c0957260a` | zlib | both | window, input, audio device, SDL GPU, IO streams |
| Jolt Physics | 5.6.0 | `e77f175595e64cb44218cc9d9d56fc365ad0e36a` | MIT | both | physics |
| Dear ImGui | 1.92.9b | `f1cc2ae15e53a861a874c3034aae6798fde194ab` | MIT | both | debug UI, menus |
| GLM | 1.0.3 | `8d1fd52e5ab5590e2c81768ace50c72bae28f2ed` | MIT | both | math |
| miniaudio | 0.11.25 | `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d` | public domain or MIT-0 | TrailForge | audio device, WAV reading and offline WAV capture (the mixer is TrailForge's own) |
| cgltf | 1.15 | `360db1a95480fe102ae9c69b27c5d101167ff5ba` | MIT | TrailForge | glTF loading |
| meshoptimizer | 1.3 | `9e1f07b159d3cb777f1c67ed31fc11fd117986f4` | MIT | TrailForge | mesh optimization and simplification |

Patches applied by TrailForge (`cmake/patches/`): `SdlIosSimulator.cmake` (in the iOS Simulator only: skips SDL
GPU's Metal GPU family check and its `setDepthClipMode` calls) and `ImguiIos.cmake` (ImGui's SDL GPU backend uses MSL
source on iOS instead of the macOS metallibs). Ashfall has no iOS build and no
patches. Ashfall plays audio through an SDL3 audio stream with sounds synthesized at startup, without miniaudio.

Build-time shader toolchain (host tools, not shipped in the binary):

| Tool | Commit or version | Purpose |
|---|---|---|
| SPIRV-Headers | `29981f65241605e08b0ede4cfeb999fe3b723c6a` | SPIR-V definitions |
| SPIRV-Tools | `9a49b0883b9b635689a85b5647dbfcb223268151` | legalization of glslang's HLSL output |
| glslang | `168d452a4f460d24b588fed08477a81c44ee27a1` | HLSL -> SPIR-V |
| SPIRV-Cross | `6c09849fe88c48eaed08413aa022aaa136a3a057` | SPIR-V -> MSL |
| DXC | 1.9.2609 (`dxc_2026_09_29.zip`) | HLSL -> DXIL, Windows hosts only |
| `xcrun metal` | Xcode | MSL -> metallib |

TrailForge's texture and model compiler (`tools/texc`, host tool) also uses stb
(`003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5`) and bc7enc_rdo (`b9438627eef73a1157e84201b6fa6eb2ffd6d9f0`).

## Planned

Added only when the stage that needs them starts ([Roadmap](roadmap.md)). Latest releases as of 2026-11-04.

| Library | Latest release | Licence | Planned use |
|---|---|---|---|
| enkiTS | 1.12 (2026-07-04) | zlib | job system; runs Jolt's jobs |
| ozz-animation | 0.17.0 (2026-08-01) | MIT | skeletal animation runtime and offline tools |
| fastgltf | 1.0.0 (2026-10-31) | MIT | glTF / GLB import in the asset cooker |
| KTX-Software (libktx) | 4.4.2 (2025-10-04) | Apache-2.0, with exceptions (see below) | KTX2 reading, writing, Basis Universal transcoding |
| Basis Universal | 2_50 (2026-08-03) | Apache-2.0 | supercompressed textures (bundled in libktx) |
| xxHash | 0.8.4 (2026-09-19) | BSD-2-Clause | asset IDs |
| Recast & Detour | 1.6.0 (2023-05-21) | zlib | navmesh building, path queries, crowds |
| Tracy | 0.14.1 (2026-08-22) | BSD-3-Clause | CPU and GPU profiler (development builds) |
| Flecs | 4.2.0 (2026-11-04) | MIT | optional ECS module for games that want one |

Already pinned and kept: meshoptimizer (cooker: LODs, quantization, compressed vertex and index buffers; gltfpack is
its command line tool), miniaudio (audio device and capture), Dear ImGui (diagnostics).

## Verified capabilities and caveats

- **ozz-animation**: the runtime has sampling, blending with per-joint weights, `track_triggering_job` (events from
  user channel tracks), `ik_two_bone_job` and `ik_aim_job`; the samples include additive blending, partial blending,
  attachments, foot IK, two-bone IK, user channels, motion extraction and skinning. It is tested on WebAssembly and its
  runtime needs C++17. It has no state machine, no retargeting and no GPU skinning: the engine implements those.
- **fastgltf**: current versions require C++20 (`cxx_std_20` in its CMake); C++17 support ended with the 0.9 series.
  The engine is C++20, so this is fine.
- **KTX-Software**: bundles Basis Universal (with zstd inside it), astc-encoder, dfdutils and an ETC decoder in
  `external/`. Most files are Apache-2.0, but `external/etcdec/etcdec.cxx` is not open source (Ericsson licence):
  configure libktx with `LIBKTX_FEATURE_ETC_UNPACK=NO` so it is not built. Apache-2.0 NOTICE files must be shipped.
- **Jolt Physics 5.6.0** (pinned commit): provides `JobSystemWithBarrier` (base for a custom job system),
  `JobSystemSingleThreaded` and `JobSystemThreadPool`. The engine job system implements `JobSystemWithBarrier` on
  enkiTS.
- **enkiTS**: `ProfilerCallbacks::threadStart` runs on each worker at start; the engine sets Apple QoS there.
- **SDL GPU**: no ray tracing, no GPU timestamps, no native handles (see
  [Architecture](architecture.md#graphics-layer)).
