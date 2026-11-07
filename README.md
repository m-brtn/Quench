<p align="center">
  <img src="examples/demo/icon/icon.png" alt="Quench" width="128" height="128">
</p>

<h1 align="center">Quench</h1>

<p align="center">
  A game engine designed for making games entirely with AI coding agents.<br>
  Version 0.10.0 · macOS and iOS · MIT
</p>

## Make your game with AI agents

You describe the game; an AI coding agent builds it on Quench, runs it, looks at the result and checks it. You play,
look and listen.

- **A new game in one command.** `tools/new_game.py` sets up a game project that builds and runs right away.
- **A skill for the agent.** Every game comes with the [quench-game](.agents/skills/quench-game/SKILL.md) skill: how
  to build a game on Quench, module by module, with working code.
- **Rules for the agent.** Every game has its own `AGENTS.md` and a devlog: how the agent works on the game, and a
  record of what it did.
- **The agent sees its own game.** Scripted scenarios run the game without a window, take screenshots the agent looks
  at and compares with reference images, and measure frame times.

<p align="center">
  <img src="docs/images/tunnel.jpg" alt="A tunnel lit by area lights, with a reflecting puddle and brushed metal" width="100%">
</p>
<p align="center">
  <img src="docs/images/raytracing.jpg" alt="Ray-traced shadows through a fence, a mirror and gold spheres" width="49%">
  <img src="docs/images/animation.jpg" alt="A crowd of animated characters on a ramp and a step" width="49%">
</p>

<p align="center"><sub>
  The engine demo: area lights and brushed metal · ray-traced shadows, reflections and bounced light · animated
  characters. All content is procedural.
</sub></p>

## What it can do

- **Graphics:** physically based materials, cascaded sun shadows, sky and atmosphere, fog, bloom, ACES and AgX tone
  mapping, TAA, HDR displays; your own render passes and surface shaders in HLSL.
- **Ray tracing** on Apple GPUs: shadows, ambient occlusion, reflections, global illumination, a path tracer,
  denoisers and MetalFX upscaling.
- **Lights and materials:** rect, disk and tube area lights with soft shadows, brushed metal, glass with tinted
  shadows, cut-outs.
- **Physics:** Jolt Physics, multithreaded on the engine's job system.
- **Sound:** 3D sound with HRTF for headphones, reverb, buses and mixing, streamed music with crossfades; WAV, FLAC,
  MP3 and Ogg Vorbis.
- **Animation:** blend trees and blend spaces, state machines, layers, IK and foot placement, look-at, root motion,
  retargeting, blend shapes, GPU skinning.
- **Assets:** a build-time cooker for glTF models and images (BC7 on the Mac, ASTC on the iPhone), loading in the
  background, streaming, hot reload.
- **Input and UI:** keyboard, mouse, gamepad and touch through named actions; Dear ImGui for tools.
- **Testing:** scripted scenarios, runs without a window, screenshot comparison, frame time and memory measurements.
- **Platforms:** macOS and iOS; Windows, Linux, Android and the web are planned.

## Getting started

Quench and its games are C++20 and build on a Mac with Xcode, CMake 3.28 or newer, Ninja and Python 3. The first
configure downloads the dependencies (a few minutes).

```sh
git clone https://github.com/m-brtn/Quench.git && cd Quench
python3 tools/new_game.py "My Game" ../my-game    # a new game, built and started once
```

Then open `../my-game` in your AI coding agent and describe the game you want.

To see what the engine can do, build and run the demo:

```sh
cmake --preset release && cmake --build --preset release
build/release/examples/demo/quench-demo
```

iPhone builds, the architecture and every module are in the [documentation](docs/README.md); what comes next is on the
[roadmap](docs/roadmap.md#what-is-next).

Built on SDL3, Metal, Jolt Physics, Dear ImGui, GLM, enkiTS, meshoptimizer, miniaudio and ozz-animation
([dependencies](docs/dependencies.md)).

## Licence

Quench is MIT licensed, see [LICENSE](LICENSE). Third-party libraries and data keep their own licences, collected in
[THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES).
