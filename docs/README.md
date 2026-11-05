# Quench documentation

Stages 1 to 3 of the [Roadmap](roadmap.md) are implemented (version 0.1.0): the core and physics modules, the GPU
layer over SDL GPU, the app shell, the sample and the game template. Where a document describes something planned, it
says so. [Migration](migration.md) describes the engine code in the two games that is not moved yet.

| Document | Contents |
|---|---|
| [Architecture](architecture.md) | Modules and targets, the graphics layer, rendering, shaders, main loop, files, threads, assets, animation |
| [App shell](app.md) | The game interface, input, ImGui, command line flags, scenarios, captures and golden images, `quench_add_game()`, iOS runs |
| [Performance](performance.md) | The benchmark, how to run and compare it, budgets per device |
| [Platforms](platforms.md) | Target platforms, what has run where, backend and shader formats, known constraints (WebGPU, Android, texture formats) |
| [Dependencies](dependencies.md) | Pinned libraries, planned libraries with licences and verified capabilities |
| [Migration](migration.md) | Inventory of the engine code in TrailForge and Ashfall, how far the copies diverged, the procedure for moving code |
| [Roadmap](roadmap.md) | Stages from the skeleton to ray tracing, with the requested features and their priorities |
| [Releases](releases/) | Release notes per version |

Coding agents: see [AGENTS.md](../AGENTS.md). The daily development log is in [devlog/](../devlog/).
