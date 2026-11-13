# Quench documentation

Stages 1 to 3 and 5 to 14 of the [Roadmap](roadmap.md) are implemented (stages 11 and 12 except their runs on the iPhone
18 Pro Max and the checks by eye, stage 13 except its costs on a phone and its runs on the iPhone 18 Pro Max, stage 14
except its runs on a phone and the checks by eye; browser audio still open), stage 4 (games on the engine) for one game (version 0.17.0): the core and physics modules (with the
wind's drag on chosen bodies), the job system, profiling with Tracy (opt-in), the asset system and the asset cooker
(BC7, ASTC in iOS builds, Basis Universal as an option, transcoded at load time; asset hot reload on the Mac and on a
device), the GPU layer over SDL GPU and a native Metal backend with GPU timestamps per pass, acceleration structures and
MetalFX (the default on Apple platforms since 0.11.0; shader hot reload on desktops), the app shell with HDR display
output and a game's opt-in reaction to the device's heat, the render module (a retained scene and the standard frame
with cut-out and translucent shadows, normal and emissive maps, area lights, anisotropic GGX, GPU skinning with blend
shapes, and on the native backend ray-traced shadows of the sun, local and area lights, ambient occlusion, reflections,
indirect light with a probe grid, the engine's temporal denoiser, MetalFX upscaling and denoising and a reference path
tracer, with shadow maps, screen-space occlusion and reflections, the sky's light and bilinear scaling as the fallbacks;
ray tracing and the area lights run on the iPhone 17 only), audio (the mixer, 3D sound with an optional HRTF, reverb
buses, Ogg Vorbis, device output, offline captures; its tests not yet run on the iPhone; the weather's sounds listened
to by the user on the Mac and the iPhone 17, the rest not yet), animation (skeletons and clips, state machines, 1D and
2D blending, events, root motion, IK and foot placement, LOD, retargeting, blend shape weights from clips; not yet run
on the iPhone or looked at in motion by the user), navigation (navmeshes built on jobs or cooked, obstacles, paths and
crowds; the demo's section looked at in motion by the user on the Mac and the iPhone 17, its tests not yet run on the
iPhone), stage 13, weather (a weather map that drifts and evolves, volumetric clouds with a 2D layer as the fallback,
their shadows, overcast light and a sky cube for reflections; rain kept from under roofs, shafts of rain from afar, wet
ground that dries, puddles with ripples and the rain's sound; snow by the temperature with flakes, a cover that builds
and melts and white shafts; wind with gusts that moves the rain, the snow, the clouds, plants and chosen physics bodies
and that sounds, lightning and thunder; looked at in motion and listened to by the user on the Mac and the iPhone 17,
its costs on a phone not measured), stage 14, particles and trails (systems stepped by compute or on the CPU, spawns in
shapes and along paths, curl noise and the weather's wind, twinkling glows, sparks and smoke, ribbons of light with
bright heads and fading tails that shed sparkles; not yet run on a phone or looked at in motion), HTTPS requests
(`quench_net`, after 0.17.0, not released: NSURLSession on Apple platforms, fetch in a browser, a fake for tests;
checked on the Mac and in headless Chrome only, [Network](net.md)), the assistant on it (`quench_assistant`, the same
day, not released: a language model that operates a game through tools it registers, typed or spoken turns, speech;
checked with a fake network and once against OpenRouter on the Mac, [Assistant](assistant.md)), the sample, the
engine demo (finished in stage 10, with a tour, a check mode and a
benchmark of every section) and the game template. Where a document describes something planned, it says so.
[Migration](migration.md) describes the engine code in the two games, what has moved and what is not moved yet.

It has run on a Mac (Metal, and Vulkan through MoltenVK), in the iOS Simulator and on two iPhones (device tests and
benchmarks; ray tracing, the area lights, the native backend as the default, ASTC textures, asset hot reload, the
weather and navigation on one of them only, where the demo's ray tracing, Weather and Navigation sections were looked at
by hand; nobody has looked at the area lights on a phone yet); what ran where, per platform: [Platforms](platforms.md).

Stack: SDL3, SDL GPU (Metal) and Metal directly, HLSL shaders compiled at build time (glslang; DXC for ray queries),
Jolt Physics, Dear ImGui, GLM, enkiTS, meshoptimizer, Basis Universal, fastgltf and astc-encoder (build time),
miniaudio, ozz-animation, Recast & Detour, Tracy (opt-in); pins and licences: [Dependencies](dependencies.md).

Building and testing: the commands of every preset (debug, release, the sanitizers, headless, iOS, WebAssembly under
Node.js) and the gates are in [AGENTS.md](../AGENTS.md), "Build and test"; a new game starts with
`python3 tools/new_game.py "My Game" ../my-game` ([App shell](app.md)). The screenshots of the top-level README are
in `images/`.

| Document | Contents |
|---|---|
| [Architecture](architecture.md) | Modules and targets, the graphics layer and its backends (SDL GPU, native Metal), GPU timestamps per pass, the ray tracing design, rendering, weather and clouds, particles, shaders, main loop, files, threads, assets, audio, animation, navigation, network, the assistant |
| [App shell](app.md) | The game interface, input, ImGui, command line flags, scenarios, the thermal state and a game's reaction to it, HDR output, captures and golden images, `quench_add_game()`, iOS runs |
| [Jobs, assets and the cook](assets.md) | The job system, asset IDs and the manager, budgets and placeholders, textures and models in a scene, streaming by distance, hot reload, the cooked formats (skins, skeletons and clip sets included), `quench_cook` and `quench_cook_assets()`, limitations |
| [Audio](audio.md) | The mixer, buses, sounds and streams, generators, 3D sound, voice limit, the audio thread, device output and the iOS audio session, offline mixing and captures, `audio_measure`, the demo section and tests, platforms, limitations |
| [Animation](animation.md) | Skeletons and clip sets, graphs (state machines), events, root motion, IK, foot placement and attachments, cooking glTF skins and animations, the file formats, GPU skinning, budgets and failures, the demo section and tests, platforms, limitations |
| [Navigation](navigation.md) | Navmeshes built at run time on jobs or cooked (`.qnav`), area types, off-mesh links, obstacles and rebuilds, queries, crowds and their link events, debug geometry, cooking, threads, cost, the demo section and tests, platforms, limitations |
| [Weather](weather.md) | Stage 13's design and what is built: the headless weather map, its queries and the wetness and snow cover it remembers, the clouds (volumetric and the 2D layer), their light and shadows, the sky cube, the rain (occlusion map, streaks, splashes, wet surfaces, puddles, ripples, shafts, rainbow) and its sound, the snow (flakes, the cover on surfaces, shafts, quiet), the wind (gusts and eddies, plants' sway, drag on bodies, its sound), lightning and thunder, phone defaults, cost, the demo section and tests, platforms, limitations |
| [Particles](particles.md) | Stage 14: particle systems in the retained scene (spawning, motion with curl noise and the weather's wind, the look, GPU and CPU simulation), trails behind moving points, paths, where they are in the frame, budgets, cost, the demo section and tests, platforms, limitations |
| [Network](net.md) | `quench_net`: asynchronous HTTPS requests (streamed bodies, cancel, timeouts, a size limit) delivered on the main thread, the HTTPS rule, the log's redaction, redirects, the backends per platform (NSURLSession, fetch, none), the fake for tests, what is not there |
| [Assistant](assistant.md) | `quench_assistant`: a language model that operates a game through its tools (OpenAI-compatible chat with tool calls, speech-to-text, text-to-speech; OpenRouter, OpenAI or a local server by settings), tools in MCP's shape and their schema check, the turn and its states, cancel, the history, the errors, privacy, the manual check against OpenRouter, what is not there |
| [Render module](render.md) | The retained scene, the standard frame, sky and sun, tone mapping and HDR output, fallbacks, game passes, surface shaders, the shader library, skinned meshes, limitations |
| [Engine demo](demo.md) | The demo's code, sections, coverage, layouts and controls, its modes (tour, check mode, benchmark of every section), flags and files, About and settings, the check mode on the iPhone, its checks and golden images, its benchmark, packaged builds (`tools/package_demo.sh`) |
| [Performance](performance.md) | The benchmark, how to run and compare it, GPU time per pass, budgets per device, the thermal state |
| [Platforms](platforms.md) | Target platforms, what has run where, backend and shader formats, known constraints (the experimental web build, Android, texture formats) |
| [Dependencies](dependencies.md) | Pinned libraries, planned libraries with licences and verified capabilities |
| [Migration](migration.md) | Inventory of the engine code in TrailForge and Ashfall, how far the copies diverged, the procedure for moving code, progress per game |
| [Roadmap](roadmap.md) | Stages from the skeleton to ray tracing and the finished demo, the weather stage (its four parts built), particles and trails, what is next, the requested features and their priorities |
| [Releases](releases/) | Release notes per version, the latest [0.17.0](releases/v0.17.0.md) |

Coding agents: see [AGENTS.md](../AGENTS.md). The daily development log is in [devlog/](../devlog/).
