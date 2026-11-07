# Quench

A C++20 game engine for games of different genres: rendering, physics, audio, input and assets on top of SDL3.

**Status: version 0.10.0, early.** There is the build (CMake presets for macOS and iOS), the core (logging, results,
assertions, JSON, file system, fixed-step clock, a job system), a Jolt Physics wrapper, an asset system
(asynchronous loading with placeholders, a cache with budgets, streaming by distance, hot reload on the Mac and on a
device) with a build-time cooker for glTF, PNG, JPEG and KTX2 (BC7, ASTC in iOS builds; [assets](docs/assets.md)),
the build-time shader compiler and a shader library (shader hot reload on desktops), a GPU layer over SDL GPU with a
native Metal backend beside it (GPU timestamps per pass, ray-traced shadows of the sun, local and area lights,
ambient occlusion, reflections and indirect light with a probe grid, a temporal denoiser, MetalFX upscaling and
denoising, a reference path tracer; not the default), the app shell (main loop through SDL main callbacks, input
actions, Dear ImGui, HDR display output, scripted verification runs, captures, macOS and iOS app bundles), a render
module with a retained scene (meshes, materials, instances and lights registered in the engine) and a standard frame
(sun shadow cascades, an HDR pass, sky and fog, cut-out and translucent shadows, area lights, anisotropic highlights,
bloom, tone mapping, TAA or FXAA, with fallbacks by device capability, among them shadow maps for local lights and
screen-space occlusion and reflections), audio (a mixer with buses and reverb, 3D sound panned or through an HRTF,
streamed music, WAV, FLAC, MP3 and Ogg Vorbis, output through miniaudio, [audio](docs/audio.md); not yet run on an
iPhone or listened to), skeletal animation on ozz-animation (state machines, 1D and 2D blending, events, root motion,
IK and foot placement, LOD, retargeting, GPU skinning with blend shapes, [animation](docs/animation.md); not yet run
on an iPhone or looked at in motion), a sample (`examples/hello`), an engine demo with one section per feature, one
panel layout and camera control for all of them, a compact layout for phones, a tour, a check mode for what a person
has to look at or listen to, and a benchmark of every section ([`examples/demo`](docs/demo.md)), a template for new
games and tests. It has run on a Mac (Metal, and Vulkan through MoltenVK), in the iOS Simulator and on two iPhones
(device tests and benchmarks; ray tracing, the area lights, ASTC textures and asset hot reload on one of them only,
where the demo's ray tracing section was tried by hand; nobody has looked at the area lights on a phone yet). Next:
see the [roadmap](docs/roadmap.md#what-is-next).

A new game: `python3 tools/new_game.py "My Game" ../my-game` ([App shell](docs/app.md)).

- Stack: SDL3, SDL GPU (Metal) and Metal directly, HLSL shaders compiled at build time (glslang; DXC for ray
  queries), Jolt Physics, Dear ImGui, GLM, enkiTS,
  meshoptimizer, fastgltf and astc-encoder (build time), miniaudio, ozz-animation.
- First platforms: macOS and iOS. Windows, Linux, Android and the web come later.

Documentation: [docs/](docs/README.md). Coding agents: [AGENTS.md](AGENTS.md). Development log: [devlog/](devlog/).

Licence: MIT, see [LICENSE](LICENSE). Third-party libraries keep their own licences.
