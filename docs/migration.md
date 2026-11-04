# Migration

Inventory of the engine code in the two games and the procedure for moving it into Quench. All documentation:
[index](README.md).

Snapshot: TrailForge `0188a3a`, Ashfall `2d5db57`, both with a clean working tree, measured on 2026-11-05. "Diff
lines" counts added plus removed lines of `diff` between the Ashfall and the TrailForge file after replacing the
namespaces (`tf`, `af`), the CMake prefixes, the entry guard prefixes and the game names with one placeholder.

## Summary

There is no clean engine layer to copy. What both games share unchanged is infrastructure (shader toolchain, physics
wrapper, fixed-step clock); the renderers, the shaders and the application classes diverged after the copy on
2026-11-02. The engine is therefore built in layers: first the shared infrastructure, then a GPU layer and an app shell
designed for both games, then render modules extracted one at a time with capture comparisons.

## Identical apart from names

| Ashfall file | TrailForge file | Lines | Remaining differences |
|---|---|---|---|
| `tools/shaderc/ShaderCompiler.cpp` | same path | 246 | entry guard prefix |
| `cmake/ShaderToolchain.cmake` | same path | 78 | whitespace |
| `cmake/EmbedShaders.cmake` | same path | 44 | namespace |
| `src/render/ShaderBlobs.h` | same path | 34 | namespace |
| `src/render/GpuTimingMetal.m` | same path | 18 | symbol prefix |
| `src/core/FixedStepClock.h` | `src/sim/FixedStepClock.h` | 58 | namespace |
| `src/physics/PhysicsWorld.h` / `.cpp` | same paths | 112 + 75 (TrailForge 113 + 84) | comments; Ashfall adds `tempAllocator()`; TrailForge has the Apple QoS fix for Jolt's workers (`0188a3a`) |

## Diverged

| File | Ashfall lines | TrailForge lines | Diff lines |
|---|---|---|---|
| `shaders/scene.hlsl` | 2069 | 4411 | 5786 |
| `src/app/Application.cpp` | 1224 | 3307 | 4109 |
| `src/render/Renderer.cpp` | 1158 | 3301 | 3429 |
| `src/render/Renderer.h` | 409 | 708 | 847 |
| `src/app/Application.h` | 195 | 285 | 354 |
| `src/render/Mesh.cpp` | 221 | 320 | 107 |
| `src/app/main.cpp` | 96 | 69 | 95 |
| `cmake/Dependencies.cmake` | 89 | 142 | 89 |
| `src/render/Mesh.h` | 79 | 109 | 70 |
| `cmake/Shaders.cmake` | 120 | 155 | 65 |

Shader functions (top-level functions in `scene.hlsl`): TrailForge has 166, Ashfall 80, 26 share a name. Identical
in both: `aces`, `envBRDFApprox`, `fogLayer`, `fullscreen_vs`, `ggxSpec`, `hash12`, `luminance`, `shadow_fs`,
`shadow_vs`, `valueNoise`. Diverged: `applyFog`, `bloom_down_fs`, `bloom_up_fs`, `cloudShadow`, `hash22`, `ign`,
`lit_fs`, `lit_vs`, `model_shadow_vs`, `model_vs`, `phaseHG`, `shadowFactor`, `skyRadiance`, `sky_fs`, `tonemap_fs`,
`viewDepth`.

Material IDs conflict: both games encode a material ID in the lit vertex (`MatId` in `src/render/Mesh.h`), with
different meanings for the same numbers (TrailForge 1 = `Dirt`, 2 = `Concrete`; Ashfall 1 = `Ground`, 2 = `Asphalt`;
Ashfall also packs a structure index with `kStructureStride` 16). The engine needs material registration instead of a
fixed enum.

Model formats differ too: TrailForge converts glTF with `trailforge_modelc` (`tools/texc/modelc.cpp`, read by
`src/core/ModelAsset.h`: LODs, collision mesh, BC7 KTX2 maps, impostor atlas); Ashfall converts with Python scripts to
`.afm` (`tools/assets/convert_*.py`, read by `src/game/Model.h`). The engine replaces both with one cooked format.

## Engine candidates in one game only

TrailForge (the larger code base):

- Core: `core/Json`, `core/Ktx2` (KTX2 reader), `core/ModelAsset`, `app/Paths`.
- App: `app/Scenario` (scripted runs for tests and captures), `app/AppIcon`, `render/Camera`, `render/DebugDraw`,
  `render/TimeOfDay`, `render/RenderSettings` (quality presets), `render/MathConvert.h` (Jolt and GLM conversion).
- Input: `input/Input`, `input/ControlLayout`, `input/TouchControls` (game-specific bindings stay in the game).
- Audio: `audio/AudioMixer`, `audio/AudioSystem`, `audio/AudioTypes`, `audio/AudioManifest` (the vehicle mappings stay
  in the game).
- Streaming: `track/TerrainTiles` (asynchronous physics terrain tiles).
- Tools: `tools/texc` (texture and model compiler), `tools/render/compare_captures.py`, `tools/ios/`.
- CMake: `IosBundle.cmake`, `AppIcon.cmake`, the iOS presets and `cmake/patches/`.

Ashfall:

- GPU mipmap generation for uploaded textures (`SDL_GenerateMipmapsForGPUTexture` in `Renderer.cpp`).
- Screen-space effects at half resolution: ambient occlusion, indirect bounce, reflections and sun shafts; the
  occlusion and volume textures alternate as their own temporal history.
- Procedural audio synthesis (`app/Audio.cpp`, sounds generated at startup).
- `animatePose` in `scene.hlsl` is a procedural vertex deformation (walk cycle, recoil), not skeletal animation; it
  stays in the game.

## Procedure for moving code

1. Before: build the game's current state and record its gates (test counts, scenario results) and reference
   captures.
2. Move the code into the engine under `quench`, with the source repository and commit in the devlog. Keep the
   behaviour; refactor in a separate step.
3. In the game, replace its copy with the engine target. Build with `FETCHCONTENT_SOURCE_DIR_QUENCH` pointing at the
   local engine.
4. Run the game's gates and compare captures with the references (`compare_captures.py`; same-process A/B for identity
   claims). Record the numbers in both devlogs.
5. Commit the engine first, then the game with the engine pin updated, and only when the user asks.

Order: TrailForge first (the engine was written for it, it has the most tests and the iOS build), then Ashfall.
