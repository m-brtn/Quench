# Quench documentation

Stages 1 to 3 and 5 to 12 of the [Roadmap](roadmap.md) are implemented (stages 11 and 12 except their runs on the
iPhone 18 Pro Max and the checks by eye; Basis Universal textures and browser audio still open), stage 4 (games on the
engine) for one game (version 0.10.0): the core and physics modules, the job system, the asset system and the asset
cooker (BC7, ASTC in iOS builds; asset hot reload on the Mac and on a device), the GPU layer over SDL GPU (and a native
Metal backend with GPU timestamps per pass, acceleration structures and MetalFX; not the default yet; shader hot
reload on desktops), the app shell with HDR display output, the render module (a retained scene and the standard
frame with cut-out and translucent shadows, normal and emissive maps, area lights, anisotropic GGX, GPU skinning with
blend shapes, and on the native backend ray-traced shadows of the sun, local and area lights, ambient occlusion,
reflections, indirect light with a probe grid, the engine's temporal denoiser, MetalFX upscaling and denoising and a
reference path tracer, with shadow maps, screen-space occlusion and reflections, the sky's light and bilinear scaling
as the fallbacks; ray tracing and the area lights run on the iPhone 17 only), audio (the mixer, 3D sound with an
optional HRTF, reverb buses, Ogg Vorbis, device output, offline captures; not yet run on the iPhone or listened to),
animation (skeletons and clips, state machines, 1D and 2D blending, events, root motion, IK and foot placement, LOD,
retargeting, blend shape weights from clips; not yet run on the iPhone or looked at in motion by the user), the
sample, the engine demo (finished in stage 10, with a tour, a check mode and a benchmark of every section) and the
game template. Where a document describes something planned, it says so. [Migration](migration.md) describes the
engine code in the two games, what has moved and what is not moved yet.

It has run on a Mac (Metal, and Vulkan through MoltenVK), in the iOS Simulator and on two iPhones (device tests and
benchmarks; ray tracing, the area lights, ASTC textures and asset hot reload on one of them only, where the demo's
ray tracing section was tried by hand; nobody has looked at the area lights on a phone yet); what ran where, per
platform: [Platforms](platforms.md).

Stack: SDL3, SDL GPU (Metal) and Metal directly, HLSL shaders compiled at build time (glslang; DXC for ray queries),
Jolt Physics, Dear ImGui, GLM, enkiTS, meshoptimizer, fastgltf and astc-encoder (build time), miniaudio,
ozz-animation; pins and licences: [Dependencies](dependencies.md).

Building and testing: the commands of every preset (debug, release, the sanitizers, headless, iOS, WebAssembly under
Node.js) and the gates are in [AGENTS.md](../AGENTS.md), "Build and test"; a new game starts with
`python3 tools/new_game.py "My Game" ../my-game` ([App shell](app.md)). The screenshots of the top-level README are
in `images/`.

| Document | Contents |
|---|---|
| [Architecture](architecture.md) | Modules and targets, the graphics layer and its backends (SDL GPU, native Metal), GPU timestamps per pass, the ray tracing design, rendering, shaders, main loop, files, threads, assets, audio, animation |
| [App shell](app.md) | The game interface, input, ImGui, command line flags, scenarios, HDR output, captures and golden images, `quench_add_game()`, iOS runs |
| [Jobs, assets and the cook](assets.md) | The job system, asset IDs and the manager, budgets and placeholders, textures and models in a scene, streaming by distance, hot reload, the cooked formats (skins, skeletons and clip sets included), `quench_cook` and `quench_cook_assets()`, limitations |
| [Audio](audio.md) | The mixer, buses, sounds and streams, generators, 3D sound, voice limit, the audio thread, device output and the iOS audio session, offline mixing and captures, `audio_measure`, the demo section and tests, platforms, limitations |
| [Animation](animation.md) | Skeletons and clip sets, graphs (state machines), events, root motion, IK, foot placement and attachments, cooking glTF skins and animations, the file formats, GPU skinning, budgets and failures, the demo section and tests, platforms, limitations |
| [Render module](render.md) | The retained scene, the standard frame, sky and sun, tone mapping and HDR output, fallbacks, game passes, surface shaders, the shader library, skinned meshes, limitations |
| [Engine demo](demo.md) | The demo's code, sections, coverage, layouts and controls, its modes (tour, check mode, benchmark of every section), flags and files, About and settings, the check mode on the iPhone, its checks and golden images, its benchmark, packaged builds (`tools/package_demo.sh`) |
| [Performance](performance.md) | The benchmark, how to run and compare it, GPU time per pass, budgets per device |
| [Platforms](platforms.md) | Target platforms, what has run where, backend and shader formats, known constraints (WebGPU, Android, texture formats) |
| [Dependencies](dependencies.md) | Pinned libraries, planned libraries with licences and verified capabilities |
| [Migration](migration.md) | Inventory of the engine code in TrailForge and Ashfall, how far the copies diverged, the procedure for moving code, progress per game |
| [Roadmap](roadmap.md) | Stages from the skeleton to ray tracing and the finished demo, what is next, the requested features and their priorities |
| [Releases](releases/) | Release notes per version |

Coding agents: see [AGENTS.md](../AGENTS.md). The daily development log is in [devlog/](../devlog/).
