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
| DXC | 1.9.2609 (`dxc_2026_09_29.zip`; elsewhere built from source, see "Added in stage 9") | NCSA | HLSL -> DXIL on Windows hosts; HLSL -> SPIR-V for the entries with ray queries on every host |
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
| miniaudio | 0.11.25 | `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d` (TrailForge's), SHA256 `1a3a79b80fc6f0b0cc155e28b954a598e0ddfa2db64e2afa8466be88c476fa55` | public domain (Unlicense) or MIT-0 | `quench_audio` (decoders), `quench_audio_device` (device API), `quench_cook` (through `quench_audio`) | WAV, FLAC, MP3 and Ogg Vorbis decoding; the playback device (Core Audio on Apple platforms, a null backend for tests) |
| stb_vorbis (in miniaudio's archive) | 1.22 (2021-07-11) | the same archive, `extras/stb_vorbis.c` | MIT or public domain (Unlicense), the reader's choice | `quench_audio` (through miniaudio's Vorbis backend) | Ogg Vorbis decoding (since 2026-11-07) |
| verblib (in miniaudio's archive) | 0.5 (2022-10-25) | the same archive, `extras/nodes/ma_reverb_node/verblib.h` | MIT-0 or public domain (Unlicense), the reader's choice | `quench_audio` (the mixer) | the reverb of reverb buses, Freeverb (since 2026-11-07) |

How it is built (`quench_miniaudio`, a static library from `src/audio/Miniaudio.c`, which includes `miniaudio.h`
with `MINIAUDIO_IMPLEMENTATION` between the declarations of `extras/stb_vorbis.c` and its definitions, in the order of
miniaudio's own `tools/audioconverter`; no new archive): the engine, node graph, resource manager,
waveform generation and encoders are compiled out (`MA_NO_ENGINE`, `MA_NO_NODE_GRAPH`, `MA_NO_RESOURCE_MANAGER`,
`MA_NO_GENERATION`, `MA_NO_ENCODING`; the engine writes WAV itself, `quench::audio::encodeWav`). Headless and web
builds add `MA_NO_DEVICE_IO` (the web build also `MA_NO_THREADING`). On Apple platforms only the Core Audio and null
backends are compiled, with the frameworks linked at build time (`MA_NO_RUNTIME_LINKING`, miniaudio's advice for
App Store builds); on iOS that unit is compiled as Objective-C (miniaudio's backend uses `AVAudioSession`).

Checked in the 0.11.25 source and in the tests: the decoders read WAV (16-bit PCM and 32-bit float), FLAC, MP3 and
Ogg Vorbis from memory (`audio.decode_wav_flac_mp3`, `audio.decode_vorbis`); streams read through a miniaudio VFS over
`quench::fs` readers (`audio.stream_*`, with WAV and Ogg Vorbis files). The MP3 decoder (dr_mp3) does not
remove the encoder delay and padding of the test file (12672 frames decoded for 11025 encoded at 22050 Hz), so
gapless loops should be WAV, FLAC or Vorbis. Vorbis from memory uses stb_vorbis' pull mode
(`stb_vorbis_open_memory`), which knows the length and trims the last frame to the final granule position (the test
loop decodes to its 48000 frames); from a reader miniaudio uses the push mode, where
`ma_decoder_get_length_in_pcm_frames` is always 0 (`miniaudio.h`, its documentation of that function and
`ma_stbvorbis_get_length_in_pcm_frames`) and a seek back reopens the decoder. The engine's streams need neither: they
read until the decoder gives no frames and then seek to the start, and the push mode gives the same frames as the
pull mode (`audio.stream_vorbis_loop`, bit for bit). The decoder opens a file that starts with `OggS` as
Vorbis directly, since miniaudio's trial order puts MP3 before Vorbis. stb_vorbis allocates with `malloc` and
`alloca` while it decodes; that runs on jobs, never on the audio thread. The null backend runs a device thread that discards the mix
(`audio_device.null_device`).

verblib's implementation is compiled into the same unit (`VERBLIB_IMPLEMENTATION`); miniaudio's own `ma_reverb_node`
around it is not (no node graph). Checked in its source: Freeverb's eight combs and four allpasses per channel with
their buffers inside the `verblib` struct (407960 bytes with the default `VERBLIB_MAXIMUM_SAMPLE_RATE` of 4 x 44100;
the engine allocates one per reverb bus when the mixer is created), so `verblib_process` and the parameter setters
allocate nothing (`audio.mixer_render_allocates_nothing` counts it); `verblib_initialize` fails outside 22050 to
176400 Hz and only fills the struct and clears the buffers (the mixer calls it again to clear an idle reverb). Its
tail does not decay to exact zeros: with only exact zeros counted as silence, an impulse's tail still had non-zero
samples after 10 s at room sizes 0.2, 0.5 and 0.8. The likely cause is its `undenormalise`, which adds and subtracts
1 and so rounds the filters to steps of about 2^-23, enough to keep a small cycle going with feedback above 0.5 (not
traced further). The mixer therefore treats a reverb under -100 dB as silent ([Audio](audio.md#reverb)).

HRTF ([Audio](audio.md#hrtf), since 2026-11-07). No library and no FFT: measured head-related impulse responses,
reduced by `tools/make_hrtf_table.py` into `src/audio/HrtfTable.inc` (checked into the repository) and listed with
their licence in `THIRD_PARTY_LICENSES` (shipped).

| Source | Commit | Licence | In | Purpose |
|---|---|---|---|---|
| SADIE HRTF database, Subject 002 (a Neumann KU100 dummy head, measured by the University of York's SADIE project), as Resonance Audio distributes it in `third_party/SADIE_hrtf_database` (`WAV/Subject_002/DFC/48K_24bit`: 1555 directions, diffuse-field compensated, 256 taps, 24-bit stereo WAV at 48 kHz) | `4556a46afd4ffae092aa281bfd072eb0279d3a29`; archive SHA256 `f7f3fbb53dbcd4f14812a7e4c2825c37423f45487a4710a34ff3f922a5efec77` (32,866,260 bytes) | Apache-2.0 (the folder's `LICENSE`, without a copyright line; the repository has no NOTICE file); York's own page was not checked | `quench_audio`: `src/audio/HrtfTable.inc` (386 directions x 2 ears x 128 taps of 16 bits, 198 KB in the binary; 479 KB of source) | binaural rendering of spatial sounds |

A table in the repository rather than a pinned archive, as with the LTC tables (stage 12): the archive is the whole of
Resonance Audio, 33 MB, and every configure (headless and web builds included) would download it for 0.2 MB of data.
The script reads the archive with Python's standard library, checks its SHA256 and writes the same file on every run;
the file's header states what was changed (the Apache-2.0 condition for modified files). The filters are convolved
directly (128 multiply-adds per ear and sample at 48 kHz, about 1.2 us per millisecond of audio and voice on the M4
Pro, [Audio](audio.md#hrtf)); an FFT library would be a new dependency for a speed the default of 8 HRTF voices does
not need. Neither game uses it, so no game pin is affected.

**Games.** TrailForge builds its own `miniaudio_lib` from the same commit with another configuration (encoder on,
FLAC off, every backend). When it moves to an engine with `quench_audio` (0.5.0), both libraries end up in its link
(`quench_app` links `quench_audio` and `quench_audio_device`). Checked with a link experiment on the Mac (two static
archives of `miniaudio.c` with the two configurations, outside both repositories): the linker loads one of the two
objects for every `ma_*` symbol, without an error or a warning, so engine code compiled against one configuration
may call the other (no FLAC; different device and context structures). TrailForge therefore has to drop
`miniaudio_lib` and use the engine's audio (or link `quench_miniaudio`) when it moves to 0.5.0; its WAV capture uses
`ma_encoder_*`, which the engine's build does not have (`quench::audio::encodeWav` or `--audio-capture` replace it).
Ashfall does not use miniaudio.

## Added in stage 8

Animation ([Animation](animation.md)). One library, in `cmake/Dependencies.cmake`, listed with its licence in
`THIRD_PARTY_LICENSES`; every configure downloads it (headless and web builds included).

| Library | Version | Commit | Licence | Linked into | Purpose |
|---|---|---|---|---|---|
| ozz-animation | 0.17.0 | `744eb9d99f606eda849acb0b1204f7a3dc20bca1`, SHA256 `a29a8a014b571487d9a6dabebeed57627fc19bf1f0ebf531b0c76384840c3b74` | MIT | `quench_animation` (`quench_ozz`: the runtime; `quench_ozz_offline`: the builders, for the build functions only), `quench_cook` | sampling, blending, IK, local-to-model matrices; skeleton and clip building, key reduction, additive clips |

How it is built: ozz's own CMake needs 3.30 (the engine's minimum is 3.28), so the engine fetches the archive
without it (`SOURCE_SUBDIR no-cmake`) and compiles the runtime (`base` and `animation/runtime`) and the offline
library (`animation/offline`, without its glTF and FBX importers and tools) as two static libraries, without
warnings (`-w`, third-party code). Its headers are private to `quench_animation` (no ozz type in a public header).
Neither game uses ozz, so no game pin is affected.

Checked in the 0.17.0 source and in the tests: `SamplingJob` with a per-clip context, `BlendingJob` with per-joint
weights and additive layers, `LocalToModelJob`, `IKTwoBoneJob`, `IKAimJob`, `SkeletonBuilder`, `AnimationBuilder`,
`AnimationOptimizer`, `AdditiveAnimationBuilder` and `MotionExtractor` (root motion into position and rotation
tracks when a clip set is built) are what the engine calls. Its SIMD math is SSE and AVX only, so on ARM (the Mac,
the iPhone) and WebAssembly it runs its scalar reference implementation (the cost is in
[Performance](performance.md#measurements)). Its runtime interpolates rotations normalized-linearly, glTF
spherically: the cook adds keys where it matters ([Animation](animation.md#cooking)). Its archives check their tags
only with asserts, so the engine wraps them in a container with a checksum. Events, state machines, root motion
accumulation, foot placement and GPU skinning are the engine's own; ozz's `track_triggering_job` is not used (events
are markers stored with each clip).

## Added in stage 9

DXC for the shader entries with ray queries ([Architecture](architecture.md#shaders)): a build-time host tool, not
linked into or shipped with anything. glslang cannot parse `RayQuery` or `RaytracingAccelerationStructure`, and DXC
publishes release binaries for Windows and Linux x86-64 only, so on macOS (and other non-Windows hosts) the engine
builds the release tag from its source archives (`cmake/QuenchDxc.cmake`, `cmake/BuildDxc.cmake`), not from an SDK
installed on the system (the LunarG Vulkan SDK ships a dxc; it is not used). On Windows hosts the release archive
that `cmake/ShaderToolchain.cmake` already pins for DXIL is used.

| Archive | Commit | SHA256 | Licence |
|---|---|---|---|
| DirectXShaderCompiler v1.9.2609 | `01b62ad47db7dee3dd33f90e7339cf9e860b3f93` | `c48c223df13ba425fd9beda9843754defbcd379a3811a6a0e590110be57168c7` | University of Illinois/NCSA (`LICENSE.TXT`; `ThirdPartyNotices.txt` lists parts under other permissive licences) |
| SPIRV-Headers (its submodule at that tag) | `496543121ce6419f23d6fa5d7194ba66c36212d2` | `a9bb9c48713245eacf97cc539b6f1d45405a92d8813f8b82e635f0a085ee9898` | MIT |
| SPIRV-Tools (its submodule at that tag) | `ef96ed763b43b59b33b31b362f09a02b729fa1c9` | `82c62146083fd558735a3171cf97cfc47903ca7d368482e87f94bd44883c0f00` | Apache-2.0 |
| DirectX-Headers (its submodule at that tag) | `980971e835876dc0cde415e8f9bc646e64667bf7` | `b5a4b6d8806ff7f29f19879f83d015dbe8740676d4ca0b48647a789cc7773c4e` | MIT |

The first configure on a machine that needs it (a build with shaders: not `headless`, not the web build) downloads
the four archives, builds `dxc` and `libdxcompiler` with DXC's own CMake cache (`cmake/caches/PredefinedParams.cmake`,
Release, no tests) and Ninja, and keeps only them with the two licence files in a per-machine tool cache
(`QUENCH_TOOL_CACHE`; default `~/Library/Caches/Quench/dxc-1.9.2609-01b62ad` on macOS); every build tree and game on
the machine shares it, and a file lock lets concurrent configures wait for one build. Measured on the M4 Pro
(2026-11-06): 135 s of wall time (1146 s user, 95 s system), about 365 MB of scratch space at its peak (sources and
build tree, deleted afterwards), 27 MB kept (`dxc` 0.6 MB, `libdxcompiler.dylib` 27.8 MB). `QUENCH_RAY_QUERY_DXC`
names another dxc; with `QUENCH_RAY_QUERY_SHADERS=OFF`, or when the build fails (a warning), the ray query entries
are left out and the renderer falls back as on a device without ray tracing. The iOS presets use the macOS DXC
through `host-tools`' `quench_shaderc` like every other shader step. Neither game uses it, so no game pin is
affected.

glslang stays for every other entry (decided without the user, 2026-11-06): its SPIR-V of the existing shaders is
what every golden image was made with; DXC's HLSL rules differ in places that would mean checking every shader and
capture again; and glslang is a library inside `quench_shaderc` where DXC is a separate executable built from source.

## Added in stage 11

Vertex tangents ([Assets](assets.md#cooked-formats), [Render](render.md)); the user chose MikkTSpace tangents stored
in the vertices on 2026-11-06. One library, in `cmake/Dependencies.cmake`, listed with its licence in
`THIRD_PARTY_LICENSES`; every configure downloads it (headless and web builds included).

| Library | Version | Commit | Licence | Linked into | Purpose |
|---|---|---|---|---|---|
| MikkTSpace | none (commit, 2020-03-26) | `3e895b49d05ea07e4c2133156cfa94369e19e409`, SHA256 `aeba65ddee85a679133d510d71d00b108f603752f90b15b9ecf7777033f6e351` | zlib (the notice in both files, `mikktspace.c` and `mikktspace.h`; the README has none) | `quench_assets` (`quench_mikktspace`, a static library from `mikktspace.c`), `quench_cook` through it | tangents where a glTF has none (`quench_cook`) and for procedural meshes (`render::computeTangents`), the frame normal map bakers use |

The archive has no CMake project; the engine compiles its one C file without warnings flags (third-party code) and
calls it only from `src/assets/Tangents.cpp` (`assets::generateTangents`), so no MikkTSpace type is in a public
header. Its repository has had no commit since 2020; it is the reference implementation Blender, the glTF sample
models and most bakers use. Neither game uses it, so no game pin is affected. One patch
(`cmake/patches/MikkTSpaceRotate.cmake`, FetchContent's `PATCH_COMMAND`): its two quicksorts rotate a random seed
with a right shift by 32 when the rotation is 0, undefined behaviour that UndefinedBehaviorSanitizer stops in the
`asan` preset; the shift is masked (`>>((32-t)&31)`), the result the hardware gave, and the changed lines are marked
in the source as the zlib licence asks. The demo's cooked models were byte-identical before and after.

## Added in stage 12

Area lights ([Render](render.md#area-lights)). No library: fitted tables and shader code from the LTC paper's
reference code, checked into the repository, listed with their licence in `THIRD_PARTY_LICENSES` (shipped).

| Source | Commit | Licence | In | Purpose |
|---|---|---|---|---|
| selfshadow/ltc_code (`fit/results/ltc.js`; `webgl/shaders/ltc/ltc_quad.fs`, `ltc_disk.fs`, `ltc_line.fs`) | `31e5e96b54f98f33098f8503003119ba2231a1c6`; `ltc.js` SHA256 `21071160163defd419b8f754ab604a8e60c44fcf539bfff9916e0e8e82316c1d` | BSD-2-Clause, with a clause asking for a reference to the paper (Heitz, Dupuy, Hill, Neubelt, "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines", SIGGRAPH 2016) in code that uses or adapts it; the licence was read in the repository's `LICENSE` at that commit | `quench_render`: `src/render/LtcTable.inc` (the 64 x 64 GGX fit, written by `tools/make_ltc_table.py`, which checks the SHA256), `shaders/quench/AreaLights.hlsli` (the quad, disk and line integrals, ported to HLSL) | raster shading of rect, disk and tube lights: diffuse and specular |

The paper's reference is in the header comments of both files, in `THIRD_PARTY_LICENSES` and in
[Render](render.md#area-lights). The tables are 32-bit floats (two RGBA32F layers, 128 KB): at low roughness and
grazing angles their elements go down to 1e-10, below half precision. Neither game uses them, so no game pin is
affected.

## Added for stage 6's remaining work

ASTC 4x4 textures for iOS ([Assets](assets.md#cooked-formats)), 2026-11-07. One library, in
`cmake/CookToolchain.cmake` with the cooker's others, listed in `THIRD_PARTY_LICENSES` with the build-time tools
(it is not linked into any program: devices sample ASTC themselves).

| Library | Version | Commit | Licence | Linked into | Purpose |
|---|---|---|---|---|---|
| astc-encoder | 5.7.1 | `0affb51aaf0e0de3a8b038fa32487d59754a1eed`, SHA256 `2dab18a2d89609d45b83b0395b5b0865358edcf65dba440ff7ebb2c74909a682` | Apache-2.0 (`LICENSE.txt`; every compiled file carries the SPDX line; no NOTICE file) | `quench_cook` only | ASTC 4x4 encoding (the medium preset, LDR sRGB or linear); decoding in the cook's tests |

How it is built: its CMake builds one library per instruction set and its command line tool, so the engine fetches
the archive without it (`SOURCE_SUBDIR no-cmake`) and compiles its 22 library sources into `quench_cook_lib`, without
warnings (`-w`, third-party code) and with `-ffp-contract=off` (upstream's default "invariance" setting: the same
output for any build type). The instruction set comes from the compiler's own macros (NEON on arm64, SSE2 or better
on x86-64). Its `ThirdParty/` (image readers for its command line tool) is not compiled. The archive is 35 MB (test
images); the cook downloads it with its other libraries at every host configure. Neither game uses it.

Checked in the 5.7.1 source and in the tests: `astcenc_compress_image` takes a strip of block rows, so the cook
compresses rows in parallel on the job system with one context per worker (`astcenc_compress_reset` between
images); the output is the same without workers and with 3 (`cook.cook_output_independent_of_threads`). A context
allocated with a parent context (`astcenc_context_alloc`'s `parent` argument) converts the parent's already
converted quality limit again, so the cook allocates each one from the configuration instead. Every
`astcenc_context_alloc` also writes the encoder's global angular tables (`prepare_angular_tables`), which contexts
read while they compress: ThreadSanitizer reports the race (the values written are the same each time). The cook
allocates contexts under an exclusive lock and compresses under a shared one.

Asset hot reload on a device and shader hot reload (2026-11-07) add no library. Shader hot reload uses SDL's
process API (pinned SDL 3.4.18). Checked in its source (`src/process/posix/SDL_posixprocess.c`): `posix_spawnp`, so
a bare program name is looked up in `PATH`, and the app's ends of the pipes are non-blocking, so the frame reads a
compiler's output and asks `SDL_WaitProcess(..., false, ...)` without waiting. On iOS SDL has no processes; the
engine does not try them there.

## Planned

Added only when the stage that needs them starts ([Roadmap](roadmap.md)). Latest releases as of 2026-11-04.

| Library | Latest release | Licence | Planned use |
|---|---|---|---|
| KTX-Software (libktx) | 4.4.2 (2025-10-04) | Apache-2.0, with exceptions (see below) | Basis Universal transcoding (the engine writes and reads KTX2 itself, BC7, ASTC 4x4 and RGBA8; ASTC through astc-encoder, above) |
| Basis Universal | 2_50 (2026-08-03) | Apache-2.0 | supercompressed textures for Android and the web (bundled in libktx) |
| Recast & Detour | 1.6.0 (2023-05-21) | zlib | navmesh building, path queries, crowds |
| Tracy | 0.14.1 (2026-08-22) | BSD-3-Clause | CPU and GPU profiler (development builds) |
| Flecs | 4.2.0 (2026-11-04) | MIT | optional ECS module for games that want one |

Already pinned and kept: Dear ImGui (diagnostics). miniaudio was pinned with stage 7 (above); Vorbis decoding
(`stb_vorbis`, from miniaudio's archive) was added on 2026-11-07.

## Verified capabilities and caveats

- **ozz-animation** (pinned in stage 8, above): the runtime has sampling, blending with per-joint weights,
  `track_triggering_job` (events from user channel tracks), `ik_two_bone_job` and `ik_aim_job`; the samples include
  additive blending, partial blending, attachments, foot IK, two-bone IK, user channels, motion extraction and
  skinning. Its runtime needs C++17; the engine's `wasm-node` check runs it under Node.js. It has no state machine,
  no retargeting and no GPU skinning: the engine implements all three (retargeting at cook time, in
  `tools/cook/Retarget.cpp`).
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
  compiler on the Mac: DXC builds and runs on macOS hosts and emits the SPIR-V of the ray query entries since stage
  9 ("Added in stage 9"), so it is the candidate.
- **SPIRV-Cross**: built from an archive without `.git`, its configure step picks up the git hash of the enclosing
  repository (the engine's or a game's) as its version string. Cosmetic; it does not affect the output.
