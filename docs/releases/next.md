# Quench, the next release (draft)

Not released: no version or tag yet (the version in `project()` stays 0.10.0). Changes since [0.10.0](v0.10.0.md)
(tag `v0.10.0`): the native Metal backend as the default on Apple platforms; an opt-in reaction to the device's
heat for games, with the demo's Adapt to heat; an agent skill on building a game on the engine, copied into every
new game; the demo's dual quaternion skinning showcase made readable after a user
report, and its characters' wrists closed; the Xcode project's generation in the full gate; soft shadows of area
lights from shadow maps where the device cannot trace (stage 12's remaining work); a game's surface shaders in ray
hits (stage 11's remaining work). This file becomes
`v<version>.md` when the user asks for the release; what was verified is in the devlog of
[2026-11-07](../../devlog/2026-11-07.md).

## What changed

### The GPU backend

- **The native Metal backend is the default on macOS, iOS and the iOS Simulator** (the user's decision,
  2026-11-07; [Architecture](../architecture.md#native-metal-backend), [Platforms](../platforms.md#native-metal-backend)).
  `gpu::defaultBackend()` is the one place that decides it: Metal on Apple platforms, SDL GPU elsewhere.
  `Backend::Default` (`DeviceDesc::backend`, `AppDesc::gpuBackend`) follows it; `QUENCH_GPU_BACKEND=sdl|metal` and
  `--gpu-backend sdl|metal` override it, and a driver only SDL GPU has (`--gpu-driver vulkan`, `SDL_GPU_DRIVER`)
  selects SDL GPU. Nothing else changes by default: `RenderSettings` keep shadow maps and every ray tracing and
  MetalFX effect off.
- **A fallback:** a native Metal device that fails to start gives an SDL GPU device with one warning (`the native
  Metal backend failed to start (...): SDL GPU instead`); when both fail, the error names both reasons.
- **Buffers cycled on the native backend:** a GPU buffer written with `cycle` (an upload or copy into it, a compute
  pass's storage write) moves to a backing no unfinished command buffer used, as on SDL GPU. Before, Metal's hazard
  tracking ordered the write after the previous frame's reads: the same pixels, but each frame's instance and
  skinning uploads waited for the previous frame's draws. `cycle` on a texture is still a no-op there.
- **Tests and batches name their backend:** `gpu_device.*` and `render.*` run on SDL GPU's Metal driver
  (`QUENCH_GPU_BACKEND=sdl`), `gpu_metal_native.*` and `render_metal_native.*` on the native backend, `*_vulkan` on
  Vulkan through MoltenVK; the test harness fails a case whose device runs on another backend than its variant (a
  fallback included), and each batch checks the app's `gpu: backend` line (`quench_add_batch(... GPU
  <backend>:<driver>)`). New cases: `default_backend`, `native_fallback`, `buffer_cycle`. Debug and sanitizer builds
  run `demo_smoke_metal` and `demo_smoke_metal_native` on Apple platforms.

### Rendering: shadows of area lights without ray tracing

- **Area lights take shadow maps** (stage 12's remaining work; [Render module](../render.md#local-light-shadow-maps)):
  with `RenderSettings::localShadowMapSize` (off by default, as before) the nearest lights with `castsShadows` that
  are not traced, area lights among them, get maps. An area light's cube of maps is in its own frame, one-sided rects
  and disks skip the face behind them (five maps), and the maps reach the range plus the shape's radius.
- **A penumbra that follows the shape:** percentage-closer soft shadows whose blocker search and filter use points
  of the rect, disk or tube (the tube's half that faces the receiver), each weighted by the light it sends to the
  receiver, with depths measured across the shape. A narrow strip casts shadows soft across it and sharp along it.
- **Read in screen space:** in a frame where an area light has maps, every map (the spot and point lights' too) is
  read by a screen-space pass at half resolution over a depth prepass, blurred by a depth-aware 3 x 3 filter, and
  the surfaces read the result as they read traced shadows (`options.w` bit 4); frames with point and spot maps only
  read them in the surfaces as before. Timed ranges `local_shadows` (the maps) and `local_filter` (the pass).
  Transparent surfaces leave area lights unshadowed.
- **Cost** on the M4 Pro (offscreen, the tunnel's four nearest lamps, 21 maps at 512): 0.92 ms of GPU time at the
  tunnel's 0.67 render scale and 1.7 ms at 1080p ([Performance](../performance.md#area-light-shadow-maps)).

### Rendering: surface shaders in ray hits

- **`quench_add_ray_hits(<target> NAME <set> SURFACES <file>...)`** (`cmake/QuenchShaders.cmake`; stage 11's
  remaining work; [Render module](../render.md#ray-hits-of-surfaces)): the engine's tracing entries (traced
  reflections, indirect light, the probe grid's rays, the path tracer; with and without normal maps) compiled again
  at build time with the game's surface files, each in a namespace of its own, and a generated switch over their
  number. `SurfaceDesc::rayHitShaders` and `rayHit` name the set and the surface's number in it; the hits of its
  materials then run its function (albedo, opacity, roughness, metallic, normal, emissive, anisotropy) with the
  engine's lighting, instead of the standard surface from the material's values. One set per renderer; its 8
  pipelines are made in the first frame that traces such a surface, and frames without one use the engine's own.
- **`QUENCH_SURFACE_HIT`** is 1 in those hits: `Surface.hlsli` declares no resources or lighting there, the
  material and its textures come from the hit tables (the textures' 256 x 256 copies at level 0), `ddx`, `ddy` and
  `fwidth` are 0, `pixel` is 0 and `view` points toward the ray's origin.
- **`RenderStats::rayHitSurfaces`** counts the instances whose hits run their function; the hit table's entry
  (`RayInstanceData`) grew from 144 to 256 bytes per instance (the material's base color and params, the
  instance's color and custom vector).
- **The demo's checker grounds** have their hits: the ray tracing section's mirror and gold spheres show the
  ground's cells instead of its mean color. A native batch part `raytracing_mirror` with its golden image; the
  demo's `camera` command takes the point it orbits (`camera YAW PITCH DISTANCE X Y Z`).
- **Cost** on the M4 Pro (offscreen, 1080p, the demo's ray tracing section, whose ground is the checker; GPU p50 of
  the pass, the lowest of alternated runs under other agents' load): traced reflections 0.93 to 0.97 ms, the probe
  grid's rays 1.24 to 1.27, per-pixel indirect light 3.98 to 4.09, the path tracer 11.7 to 12.7 (6 to 8 % more);
  the tunnel (no such surface) unchanged ([Performance](../performance.md#surface-functions-in-ray-hits)).

### The app: a reaction to heat

- **`App::thermalGovernor()`** (`quench/app/ThermalGovernor.h`; [App shell](../app.md#thermal-state)): a game's
  opt-in reaction to the system's thermal state, off by default; the engine still changes nothing by itself. Enabled
  with a `ThermalGovernorDesc`, it maps the states to limits the game allows (`ThermalLimits`: a frame rate limit, a
  factor on the render scale, `reduceEffects` for the game's own choice of effects). Defaults: nothing at fair; 60 Hz
  and x0.8 from serious on; 30 Hz, x0.6 and `reduceEffects` at critical. A higher state applies at once, a lower one
  only after `recoverSeconds` (60 s) of app time below, then to the highest state of that time; unknown counts as
  nominal. The App applies the frame rate limit on top of `setMaxFrameRate`, never above it (`App::frameRateLimit()`
  is the result), and the player's value comes back when it is lifted; `renderScale(playerScale)` gives the game the
  reduced scale without touching its settings. A log line per change, `onChange`, a perf line field and
  `PerfReport::reactionStart`, `reactionEnd`, `reactionMax`.
- **Checks without a hot device:** `--thermal-state nominal|fair|serious|critical|unknown` and the scenario command
  `thermal_state NAME` (`system` gives the state back) replace the system's state; new scenario values
  `thermal_reaction`, `thermal_frame_rate_cap`, `thermal_render_scale`, `thermal_reduce_effects`,
  `thermal_reaction_changes`, `frame_rate_limit`, and after `perf_end` `perf_thermal_reaction_start`, `_end`, `_max`.
- **The demo's Adapt to heat** (Settings, off; `adapt_to_heat` in `settings.json`, still version 1): the engine's
  defaults; the render scale goes into what the renderer gets and, at critical, the traced reflections and indirect
  light fall back to the sky's and the lamp shadows and the shadow and occlusion rays go down to one. The Performance
  panel shows the thermal state and what is reduced (in both layouts), the Graphics panel a line while limits apply;
  `benchmark.json` gets the reaction per section. A batch part `heat` (no capture) and a check mode item
  `tunnel.heat` (the tunnel's phone defaults for 10 minutes with it on; 41 items now).
- **The template** has the same setting as an example (`"adapt_to_heat": false` in `assets/settings.json`, a
  checkbox, the scenario command `adapt_to_heat on|off`), and its smoke scenario checks it with `thermal_state`.

### Building games

- **The agent skill `quench-game`** (`.agents/skills/quench-game/`: `SKILL.md` and `reference.md`): how a coding
  agent builds a game on Quench: `tools/new_game.py`, the `Game` interface and the fixed-step loop,
  `quench_add_game()`, the renderer, cooked assets, physics, audio, animation, jobs, input, ImGui, `quench::fs`,
  offscreen scenarios, captures and golden images (with the game's own build of `quench_image_compare`), iOS builds,
  the rules a game keeps and pitfalls. `tools/new_game.py` copies it unchanged into every new game, whose
  `AGENTS.md` points to it. `AGENTS.md`: a change to what games build on (public API, `quench_add_game()`, the
  template, common flags, scenario commands, asset cooking) updates the skill in the same commit. Linked from the
  README.
- **The template's iOS presets cook assets:** `ios-base` passes `QUENCH_HOST_COOK` and the `host-tools` build
  preset builds `quench_cook` too; before, a template game that called `quench_cook_assets()` stopped at its iOS
  configure (`cmake/QuenchCook.cmake` needs the host cooker in a cross build). See "Changes for games".

### The demo

- **The skinning showcase** (section 10, view "Skinning"; [Engine demo](../demo.md#sections)): the user toggled
  "Dual quaternion skinning" there and saw nothing change (2026-11-07): the checkbox switched only the other
  characters, whose joints hardly twist, and the showcase was two small mannequins with a fixed skinning each. Now
  the view is a close-up of two red and white checkered bars on posts, their upper halves turned by 170 degrees
  about their length, with a standing character beside each whose inner forearm is turned the same way. The left
  pair is labelled "Linear (reference)" and always skinned linearly; the right pair follows the checkbox and its
  label reads "Linear" or "Dual quaternion". Linear skinning pinches the bars and forearms to a thin neck at the
  twist; with the checkbox on, the right bar stays round and its squares run around it in a spiral. A "Twist"
  slider (0 to 180 degrees, scenario value and command `twist`) and a line in the panel say what to look at. The
  bars are drawn in that view only. The check mode's `animation.skinning` item says what a person should see.
- **Two captures in one part:** `animation_skinning` captures the view linear (`demo_animation_skinning.png`) and
  with the checkbox on (`demo_animation_skinning_dq.png`); a batch part can name several golden images
  (`quench_add_batch(... PARTS <file>:<golden>:<golden>)`, `tests/CheckPart.cmake`). Its waits end half a frame off
  the fixed clock's steps, as the retarget and blend shape parts' do, so the native backend's captures match SDL
  GPU's.
- **The characters' wrists:** a gap showed the sky between the forearms and the hands of the mannequin (the user's
  screenshots of 2026-11-07 in the Skinning view) and of the strider. Their forearm tubes ended 1 cm (mannequin) and
  1.3 cm (strider) short of the wrist, where the rigid hand box begins. Now each forearm runs through the wrist into
  the hand box, narrowed to the hand's width there; the ring at the wrist is skinned half to the hand, the one inside
  the box wholly, so the join follows the hand when it turns (`tools/assets/make_demo_character.py`,
  `make_demo_strider.py`: 884 vertices each, 836 before). The Skinning view's camera is a little further back and
  lower, so that the bars clear the demo's caption and the menu at the default window size.
- **The tunnel's lamp shadow maps** (section 12 without ray tracing: SDL GPU, or a device that cannot trace): a
  "Lamp shadow maps" choice in its panel (Off, 256, 512, 1024; off by default), a batch part `tunnel_maps` with golden
  captures on SDL GPU's Metal driver and on Vulkan through MoltenVK, and a check mode item `tunnel.maps` (42 items
  now).
- **The native backend by default:** the ray tracing section and the tunnel trace where `gpu::Caps` allow it
  without a flag now (on the Mac and on phones with ray tracing: the iPhone 17 and the iPhone 18 Pro Max), with their
  phone defaults on phones; `--gpu-backend sdl` shows their fallbacks. About names the platform's default backend and
  says when a run is not on it; the Graphics panel's relaunch buttons mark the default. The settings (version 1)
  store no backend, so nothing is migrated. The `*_phone` scenarios measured on SDL GPU say `--gpu-backend sdl`.

### Gates

- **`tools/check.sh step new-game`** also runs `tools/check_skill.py` (the engine paths, linked files, flags,
  scenario commands and values, `quench_*` CMake names and engine C++ names the skill uses exist in the checkout)
  and checks that the new game's copy of the skill equals the engine's.

- **`tools/check.sh step xcode`**, also a step of `full`: generates the `ios-xcode` project into
  `build/ios-xcode-check` (configure only: no build, no signing, no team), so that what the Xcode generator refuses
  and Ninja accepts is caught before a package. 0.10.0's iOS package failed that way (fixed in `81fe747`); with that
  fix taken out, the step fails with the same error. `tools/check.sh quick` names the step when CMake files changed
  ([App shell](../app.md#ios-runs)).

## What was verified

From the devlog of [2026-11-07](../../devlog/2026-11-07.md). Mac (Apple M4 Pro).

- **The native backend as the default** (offscreen): hello and the demo without `--gpu-backend` run on it;
  `--gpu-backend sdl`, `QUENCH_GPU_BACKEND=sdl` and `--gpu-driver vulkan` give SDL GPU; `native_fallback` gets an SDL
  GPU device from a native device that fails; `buffer_cycle` passes on SDL GPU's Metal driver, the native backend
  and Vulkan through MoltenVK; every batch checks the backend it ran on.
- **The native backend as the default in the iOS Simulator** (iPhone 18 Pro Max Simulator, iOS 27.0, no backend
  flag): hello and the demo's smoke scenario (every section once, 17 of 17) ran on it, the ray tracing section and
  the tunnel with their fallbacks (no ray tracing there); the captures were looked at.
- **The native backend as the default on the iPhone 17** (iOS 27.2, no backend flag; [Performance](../performance.md#the-native-default-on-the-screen-and-on-the-iphone-17)):
  the device tests with the Metal API validation layer, 262 cases, 258 passed, 0 failed, 4 skipped (SDL GPU by
  `QUENCH_GPU_BACKEND=sdl`: 232 passed, 0 failed, 30 skipped); hello and the demo's smoke run on it; every section
  of the demo's benchmark but the two that trace held 120 Hz, GPU p50 as on 2026-11-06 and CPU p50 within -3 to
  +7 % (0.8.0's demo then), and the ray tracing section and the tunnel held 60 Hz at their phone defaults; the costs
  of the ray-traced effects within 3 % of 2026-11-06's but the first region's (5.5 % lower; the native backend by
  flag then). Numbers only; nobody looked at the screen during the runs.
- **The hello benchmark on the Mac's screen, both backends** (60 Hz, vsync, two runs each, alternated): GPU p50
  0.549 ms on both; CPU p50 0.298 ms on both (means of the runs), p99 0.525 against 0.512 ms.
- **Performance against SDL GPU** (offscreen, unpaced; [Performance](../performance.md#sdl-gpu-and-the-native-backend-before-it-became-the-default)):
  at 480 x 270 (CPU-bound) the native backend's CPU p50 is within -11 to +2 % of SDL GPU's without per-pass
  timestamps and +2 to +8.5 % (8 to 12 µs) with them, as in every perf region; the animation section 0.646 against
  0.673 ms (it was 0.882 against 0.647 before buffers were cycled). At 1920 x 1080 (GPU-bound) 0.3 % more to 3.1 %
  fewer frames. hello: CPU p50 0.022 ms and GPU p50 0.145 to 0.146 ms on both. The memory footprint in the
  animation section with 256 characters: 505 MB on the native backend, 761 MB on SDL GPU.
- **Gates with the native default:** `tools/check.sh full` (2026-11-07, 1130.6 s): format; `debug` 496 tests;
  `release` 605; `asan` 496; `tsan` 496; `wasm-node` 140 (under Node.js); `ios-sim-tests` 369 in the iOS Simulator;
  `ios-xcode` generated; a new game built and started. All passed.

- **The showcase's captures**, looked at on Metal and on Vulkan through MoltenVK, linear and with the checkbox on:
  the linear capture against the dual quaternion one differs in 4.70 % of the pixels (mean 4.02; 5.08 % before the
  camera moved back); the native backend's captures are identical to SDL GPU's (0 pixels differ), Vulkan through
  MoltenVK within 15 levels (mean at most 0.0017). `demo_animation_retarget`'s golden images changed with it: the two showcase characters far in its
  background stand further apart (0.28 % of the pixels, the walking pair unchanged).
- **The xcode step:** about 11 to 13 s with its directory in place, 254.5 s the first time (downloads); configured
  with `QUENCH_IOS_TEAM` empty.
- **The skill:** a dry run built a game from it alone (a lit scene with a cooked texture and model, a Jolt box, a
  cooked sound, an input action, a scenario with a capture and a golden image test): Debug and Release ctest 3 of
  3, the iOS Simulator build (ASTC textures cooked) launched offscreen; `tools/check_skill.py` passes on it and
  reported two deliberately broken copies line by line; `step new-game` 142.1 s.
- **The wrists:** the generated characters' weights sum to 1 with at most two joints per vertex; the 12 vertices of
  each wrist ring are half forearm, half hand. Close-ups at 2560 x 1440 looked at: no gap on either character,
  untwisted or twisted. The skinning part's golden images were replaced (camera and wrists: mean 37.56 and 38.28
  against the previous images); the other captures with characters stay within their golden images (at most 62
  pixels over 16, 0.012 %), the native backend's skinning captures equal SDL GPU's.
- **Gates:** `tools/check.sh full` with the xcode step (2026-11-07, 378.1 s): format; `debug` 486 tests; `release`
  596 (the captures of every backend, Vulkan through MoltenVK included); `asan` 486; `tsan` 486; `wasm-node` 140
  (under Node.js); `ios-sim-tests` 363 in the iOS Simulator; `ios-xcode` generated; a new game built and started.
  All passed.

- **The reaction to heat** (offscreen, fake states): 10 unit cases of the policy (`thermal_governor_*`,
  `thermal_limits_text`: escalation at once, the hold and its restart, recovery to the highest state in the hold,
  the player's frame rate first, unknown as nominal, accumulated limits, disable and callbacks) and the flag's
  parsing; the demo's `heat` part 48 of 48 checks in each batch: SDL GPU's Metal driver, the native backend (there
  1 lamp shadow of 4 and the sky's reflections and light at critical, all back after the recovery) and Vulkan
  through MoltenVK; the template's smoke scenario 11 of 11 in a game made by `tools/new_game.py`. No golden image
  changed: the reaction is off by default.
- **Gates with the thermal reaction:** `tools/check.sh full` (2026-11-07, 353.5 s): format; `debug` 506 tests;
  `release` 618; `asan` 506; `tsan` 506; `wasm-node` 140 (under Node.js); `ios-sim-tests` 379 in the iOS
  Simulator; `ios-xcode` generated; a new game built and started. All passed.

- **Surface shaders in ray hits** (native Metal backend, offscreen): `renderer_ray_hits_run_surface_functions`:
  the test surface `flat` (green from its params; the material's base color red) on two boxes over a mirror, one
  with its hits in the set: the raster shows both green (15 243 15), the mirror that one green (16 243 16) and the
  other red from the material's values (200 20 20), the path tracer the same (15 243 15 and 208 17 17); `addSurface`
  refuses a `rayHit` without a set and a second set. The demo's `demo_raytracing_mirror_native.png` looked at: the
  mirror shows the ground's cells and the sun's shadows on them; `demo_raytracing_native.png` changed within its
  thresholds (mean 0.136, 3 pixels over 16) and was replaced.
- **Gates with surface shaders in ray hits:** `tools/check.sh full` (2026-11-07, 547.3 s): format; `debug` 512
  tests; `release` 627; `asan` 512; `tsan` 512; `wasm-node` 140 (under Node.js); `ios-sim-tests` 383 in the iOS
  Simulator; `ios-xcode` generated; a new game built and started. All passed.

## What was not verified

- **The reaction to heat on a hot phone:** it has run with fake states only. Its defaults are a starting point: in
  the iPhone 17's ten-minute run of 2026-11-06 the tunnel missed intervals at fair, about a minute before serious,
  where the defaults begin. On a phone the frame rate limit acts through the display link; offscreen runs do not
  limit frames, so the checks read `frame_rate_limit`, not the frame rate. Nobody has looked at the demo's
  Performance and Graphics lines on a phone.

- **The native backend as the default on the iPhone 18 Pro Max:** it ran there only when chosen, and nobody has
  looked at the default on a phone's screen. The iPhone 18 Pro Max has not run the ray tracing section's and the
  tunnel's phone defaults, which the demo now shows there without a flag (the iPhone 17 has). On the iPhone 17 the
  tunnel's phone defaults turned the phone fair within their minute at 60 Hz in a run that followed a quarter of an
  hour of other load (nominal on 2026-11-06); not repeated from a cold phone.
- Per-pass timestamps cost the native backend 8 to 12 µs of CPU per frame, and 6.5 % of the frames in the animation
  section (perf regions and `App::requestPassTiming` only); `cycle` on a texture is still a no-op there.

- Nobody has looked at the new showcase yet, on the Mac's screen in motion or on a phone; the user's report was
  about the previous one. The closed wrists were looked at in still captures only, not while a hand turns (the
  hero's wave) or in motion.
- The skill: one dry run on the Mac (Debug, Release, the iOS Simulator); its animation, jobs, surface shader and
  device parts were not run. `tools/check_skill.py` finds renamed or removed names, not changed behaviour.
- Surface shaders in ray hits on a phone: built into the iOS presets, not run on an iPhone (the device tests include
  the new render case); nobody has looked at the checker in the mirror on a screen in motion. A game's surface with
  resources of its own, or several surfaces in one set, is not tested beyond the one-file sets of the test and the
  demo.
- The xcode step generates the project only: a build or signing problem in `xcodebuild` is still found only by a
  device build or `tools/package_demo.sh --ios`.

## Changes for games

- **The default backend on Apple platforms is the native Metal backend.** A game that leaves
  `AppDesc::gpuBackend` (or `DeviceDesc::backend`) at `Backend::Default` runs on it on the Mac, the iPhone and in the
  iOS Simulator after moving its pin, through the same `quench::gpu` API and shaders; the engine's captures are the
  same on both backends. What differs: `gpu::Caps` (`backend`, `gpuPassTiming`, `rayTracing`, the upscaling caps,
  `driver` stays `metal`), per-pass GPU times in perf regions, and the memory a run takes. A game that wants SDL GPU
  there sets `AppDesc::gpuBackend = gpu::Backend::SdlGpu`, or passes `--gpu-backend sdl` or `QUENCH_GPU_BACKEND=sdl`;
  `--gpu-driver vulkan` still gives SDL GPU. A game whose own tests or scripts assumed SDL GPU on Apple platforms
  names it now.
- **Area lights with `castsShadows` take shadow maps** where a game sets `RenderSettings::localShadowMapSize` and
  the lights are not traced: they cast soft shadows now and cost a screen-space pass in those frames (0.9 to 1.7 ms
  of the M4 Pro for four lamps). A game that wants them unshadowed sets `castsShadows = false` on them. Custom
  surface shaders built on the engine's `Surface.hlsli` need no change: in those frames opaque surfaces read the
  screen-space term as they read traced shadows, and `quenchLocalShadows` skips area lights.
- **Surface shaders in ray hits (optional):** `quench_add_ray_hits()`, `SurfaceDesc::rayHitShaders` and `rayHit`,
  `RenderStats::rayHitSurfaces`, the macro `QUENCH_SURFACE_HIT` (0 in every other entry). A game that does nothing
  keeps the standard surface in its hits, as before. A surface listed in a set compiles once more inside the
  engine's tracing source: its own `cbuffer`s and textures go behind `#if !QUENCH_SURFACE_HIT`, and its macros must
  not clash with another listed surface's. `Surface.hlsli` declares its bindings and lighting only where
  `QUENCH_SURFACE_HIT` is 0, so shader code of a game's own that defines `QUENCH_SURFACE_HIT` for another purpose
  breaks.
- **New:** `gpu::defaultBackend()` (`quench/gpu/Device.h`). `Device::create` no longer returns an error when only
  the native device fails to start: it returns an SDL GPU device (check `caps().backend` where it matters).
- **iOS presets of games made from the template before this release:** a game that cooks assets
  (`quench_cook_assets()`) and builds for iOS adds the host cooker to its `CMakePresets.json`, as the template now
  has it. In the configure preset that the iOS presets inherit (`ios-base` in the template), next to
  `QUENCH_HOST_SHADERC`:

  ```json
  "QUENCH_HOST_COOK": "${sourceDir}/build/host-tools/_deps/quench-build/quench_cook"
  ```

  and in the `host-tools` build preset:

  ```json
  { "name": "host-tools", "configurePreset": "host-tools", "targets": ["quench_shaderc", "quench_cook"] },
  ```

  Without them its iOS configure stops at `quench_cook_assets()`. A game without cooked assets needs nothing.
- **The thermal state's header:** `app::ThermalState` and `thermalStateName` moved from `quench/app/App.h` to
  `quench/app/ThermalGovernor.h`, which `App.h` includes: no change for code that includes `App.h`. New and optional:
  `App::thermalGovernor()`, `App::frameRateLimit()`, `PerfReport::reactionStart`, `reactionEnd`, `reactionMax`
  (`Unknown` while the reaction is off), the flag `--thermal-state` and the scenario command `thermal_state`. A game
  that does not enable the governor behaves as before; a game whose own flag or scenario command is named
  `--thermal-state` or `thermal_state` loses it to the engine's.
- **The skill:** a game made before this release has no copy; copy `.agents/skills/quench-game/` from the engine
  at the pinned commit if its agents should use it (and again when the pin moves).
- The rest is in the demo and the engine's own test and gate scripts; `quench_add_batch()` in
  `tests/CMakeLists.txt` (now with `GPU <backend>:<driver>`) is not part of what games use.
