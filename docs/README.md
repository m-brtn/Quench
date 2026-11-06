# Quench documentation

Stages 1 to 3, 5, 7 and 10 of the [Roadmap](roadmap.md) are implemented, stage 4 (games on the engine) for one
game, stage 6 except shader hot reload, stage 8 except retargeting and stage 9 except ray-traced reflections and
indirect light (version 0.8.0): the core and physics modules, the job system, the asset system and the asset
cooker, the GPU layer over SDL GPU (and a native Metal backend with GPU timestamps per pass, acceleration structures
and MetalFX; not the default yet), the app shell with HDR display output, the render module (a retained scene and
the standard frame with cut-out and translucent shadows, GPU skinning, and on the native backend ray-traced sun
shadows and ambient occlusion and MetalFX upscaling, with shadow maps and bilinear scaling as the fallbacks; not yet
run on the iPhone), audio (the mixer, 3D sound, device output, offline captures; not yet run on the iPhone or
listened to), animation (skeletons and clips, state machines, blending, events, root motion, IK and foot placement;
not yet run on the iPhone or looked at in motion by the user), the sample, the engine demo (finished in stage 10,
with a tour, a check mode and a benchmark of every section; its compact layout not yet on the iPhone) and the game
template. Where a document describes something planned, it says so. [Migration](migration.md) describes the engine code in the two
games, what has moved and what is not moved yet.

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
