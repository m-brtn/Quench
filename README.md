# Quench

A C++20 game engine for games of different genres: rendering, physics, audio, input and assets on top of SDL3.

**Status: version 0.1.0, early, no renderer yet.** There is the build (CMake presets for macOS and iOS), the core
(logging, results, assertions, JSON, file system, fixed-step clock), a Jolt Physics wrapper, the build-time shader
compiler, a GPU layer over SDL GPU, the app shell (main loop through SDL main callbacks, input actions, Dear ImGui,
scripted verification runs, captures, macOS and iOS app bundles), a sample (`examples/hello`), a template for new games
and tests. It has run on a Mac (Metal, and Vulkan through MoltenVK), in the iOS Simulator and on an iPhone. Games draw
with their own passes until the engine's renderer exists; see the [roadmap](docs/roadmap.md).

A new game: `python3 tools/new_game.py "My Game" ../my-game` ([App shell](docs/app.md)).

- Stack: SDL3, SDL GPU (Metal), HLSL shaders compiled at build time, Jolt Physics, Dear ImGui, GLM, miniaudio.
- First platforms: macOS and iOS. Windows, Linux, Android and the web come later.

Documentation: [docs/](docs/README.md). Coding agents: [AGENTS.md](AGENTS.md). Development log: [devlog/](devlog/).

Licence: MIT, see [LICENSE](LICENSE). Third-party libraries keep their own licences.
