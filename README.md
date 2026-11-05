# Quench

A C++20 game engine for games of different genres: rendering, physics, audio, input and assets on top of SDL3.

**Status: version 0.4.0, early.** There is the build (CMake presets for macOS and iOS), the core (logging, results,
assertions, JSON, file system, fixed-step clock, a job system), a Jolt Physics wrapper, an asset system
(asynchronous loading with placeholders, a cache with budgets, streaming by distance, hot reload on the Mac) with a
build-time cooker for glTF, PNG, JPEG and KTX2 ([assets](docs/assets.md)), the build-time shader compiler and a shader
library, a GPU layer over SDL GPU, the app shell (main loop through SDL main callbacks, input actions, Dear ImGui, HDR
display output, scripted verification runs, captures, macOS and iOS app bundles), a render module with a retained
scene (meshes, materials, instances and lights registered in the engine) and a standard frame (sun shadow cascades, an
HDR pass, sky and fog, bloom, tone mapping, TAA or FXAA, with fallbacks by device capability), a sample
(`examples/hello`), an engine demo with one section per feature ([`examples/demo`](docs/demo.md)), a template for new
games and tests. It has run on a Mac (Metal, and Vulkan through MoltenVK), in the iOS Simulator and on an iPhone (the
asset system not yet on the iPhone). No audio or animation yet; see the [roadmap](docs/roadmap.md).

A new game: `python3 tools/new_game.py "My Game" ../my-game` ([App shell](docs/app.md)).

- Stack: SDL3, SDL GPU (Metal), HLSL shaders compiled at build time, Jolt Physics, Dear ImGui, GLM, enkiTS,
  meshoptimizer, fastgltf (build time), miniaudio (planned with audio).
- First platforms: macOS and iOS. Windows, Linux, Android and the web come later.

Documentation: [docs/](docs/README.md). Coding agents: [AGENTS.md](AGENTS.md). Development log: [devlog/](devlog/).

Licence: MIT, see [LICENSE](LICENSE). Third-party libraries keep their own licences.
