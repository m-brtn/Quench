# Quench documentation

Stages 1 to 3 of the [Roadmap](roadmap.md) are implemented (version 0.2.0): the core and physics modules, the GPU
layer over SDL GPU, the app shell, the sample and the game template. Stage 5 came after 0.2.0 and is not in a release
yet: the render module (a retained scene and the standard frame), HDR display output and the engine demo. Where a
document describes something planned, it says so. [Migration](migration.md) describes the engine code in the two
games, what has moved and what is not moved yet.

| Document | Contents |
|---|---|
| [Architecture](architecture.md) | Modules and targets, the graphics layer, rendering, shaders, main loop, files, threads, assets, animation |
| [App shell](app.md) | The game interface, input, ImGui, command line flags, scenarios, HDR output, captures and golden images, `quench_add_game()`, iOS runs |
| [Render module](render.md) | The retained scene, the standard frame, sky and sun, tone mapping and HDR output, fallbacks, game passes, surface shaders, the shader library, limitations |
| [Engine demo](demo.md) | The demo's sections and controls, its checks and golden images, its benchmark, packaged builds (`tools/package_demo.sh`) |
| [Performance](performance.md) | The benchmark, how to run and compare it, budgets per device |
| [Platforms](platforms.md) | Target platforms, what has run where, backend and shader formats, known constraints (WebGPU, Android, texture formats) |
| [Dependencies](dependencies.md) | Pinned libraries, planned libraries with licences and verified capabilities |
| [Migration](migration.md) | Inventory of the engine code in TrailForge and Ashfall, how far the copies diverged, the procedure for moving code, progress per game |
| [Roadmap](roadmap.md) | Stages from the skeleton to ray tracing, with the requested features and their priorities |
| [Releases](releases/) | Release notes per version |

Coding agents: see [AGENTS.md](../AGENTS.md). The daily development log is in [devlog/](../devlog/).
