# Quench

A C++20 game engine for games of different genres: rendering, physics, audio, input and assets on top of SDL3.

**Status: version 0.8.0, early.** There is the build (CMake presets for macOS and iOS), the core (logging, results,
assertions, JSON, file system, fixed-step clock, a job system), a Jolt Physics wrapper, an asset system
(asynchronous loading with placeholders, a cache with budgets, streaming by distance, hot reload on the Mac) with a
build-time cooker for glTF, PNG, JPEG and KTX2 ([assets](docs/assets.md)), the build-time shader compiler and a shader
library, a GPU layer over SDL GPU with a native Metal backend beside it (GPU timestamps per pass, ray-traced sun
shadows and ambient occlusion, MetalFX upscaling; not the default, not yet run on an iPhone), the app shell (main
loop through SDL main callbacks, input actions, Dear ImGui, HDR display output, scripted verification runs,
captures, macOS and iOS app bundles), a render module with a retained
scene (meshes, materials, instances and lights registered in the engine) and a standard frame (sun shadow cascades, an
HDR pass, sky and fog, cut-out and translucent shadows, bloom, tone mapping, TAA or FXAA, with fallbacks by device
capability), audio (a mixer with buses, 3D sound, streamed music and output through miniaudio,
[audio](docs/audio.md); not yet run on an iPhone or listened to), skeletal animation on ozz-animation (state
machines, blending, events, root motion, IK and foot placement, GPU skinning, [animation](docs/animation.md); not
yet run on an iPhone or looked at in motion), a sample (`examples/hello`), an engine demo with one section per
feature, one panel layout and camera control for all of them, a compact layout for phones, a tour, a check mode for
what a person has to look at or listen to, and a benchmark of every section ([`examples/demo`](docs/demo.md)), a
template for new games and tests. It has run on a Mac (Metal, and Vulkan through MoltenVK), in the iOS Simulator and
on an iPhone (the asset system, audio, animation, the native Metal backend, ray tracing and the finished demo not yet
on the iPhone). Next: see the [roadmap](docs/roadmap.md#what-is-next).

A new game: `python3 tools/new_game.py "My Game" ../my-game` ([App shell](docs/app.md)).

- Stack: SDL3, SDL GPU (Metal) and Metal directly, HLSL shaders compiled at build time (glslang; DXC for ray
  queries), Jolt Physics, Dear ImGui, GLM, enkiTS,
  meshoptimizer, fastgltf (build time), miniaudio, ozz-animation.
- First platforms: macOS and iOS. Windows, Linux, Android and the web come later.

Documentation: [docs/](docs/README.md). Coding agents: [AGENTS.md](AGENTS.md). Development log: [devlog/](devlog/).

Licence: MIT, see [LICENSE](LICENSE). Third-party libraries keep their own licences.
