# Quench

A C++20 game engine for games of different genres: rendering, physics, audio, input and assets on top of SDL3.

**Status: early, no renderer yet.** There is the build (CMake presets for macOS and iOS), the core (logging, results,
assertions, JSON, file system, fixed-step clock), a Jolt Physics wrapper, the build-time shader compiler, a GPU layer
over SDL GPU (tested on Metal, in the iOS Simulator and on Vulkan through MoltenVK) and tests. There is no main loop,
renderer or sample app yet; see the [roadmap](docs/roadmap.md).

- Stack: SDL3, SDL GPU (Metal), HLSL shaders compiled at build time, Jolt Physics, Dear ImGui, GLM, miniaudio.
- First platforms: macOS and iOS. Windows, Linux, Android and the web come later.

Documentation: [docs/](docs/README.md). Coding agents: [AGENTS.md](AGENTS.md). Development log: [devlog/](devlog/).

Licence: MIT, see [LICENSE](LICENSE). Third-party libraries keep their own licences.
