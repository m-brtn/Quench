# Quench, the next release (draft)

Not released: no version or tag yet (the version in `project()` stays 0.10.0). Changes since [0.10.0](v0.10.0.md)
(tag `v0.10.0`): the native Metal backend as the default on Apple platforms; an agent skill on building a game
on the engine, copied into every new game; the demo's dual quaternion skinning showcase made readable after a user
report, and its characters' wrists closed; the Xcode project's generation in the full gate. This file becomes
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

## What was not verified

- **The native backend as the default on an iPhone and in the iOS Simulator:** the tests and batches name their
  backend, so neither ran it as the default, and nobody has looked at it on a screen. The iPhone 18 Pro Max has not
  run the ray tracing section's and the tunnel's phone defaults, which the demo now shows there without a flag
  (the iPhone 17 has, 2026-11-06). The hello benchmark on the Mac's screen on both backends is still to run.
- Per-pass timestamps cost the native backend 8 to 12 µs of CPU per frame, and 6.5 % of the frames in the animation
  section (perf regions and `App::requestPassTiming` only); `cycle` on a texture is still a no-op there.

- Nobody has looked at the new showcase yet, on the Mac's screen in motion or on a phone; the user's report was
  about the previous one. The closed wrists were looked at in still captures only, not while a hand turns (the
  hero's wave) or in motion.
- The skill: one dry run on the Mac (Debug, Release, the iOS Simulator); its animation, jobs, surface shader and
  device parts were not run. `tools/check_skill.py` finds renamed or removed names, not changed behaviour.
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
- **The skill:** a game made before this release has no copy; copy `.agents/skills/quench-game/` from the engine
  at the pinned commit if its agents should use it (and again when the pin moves).
- The rest is in the demo and the engine's own test and gate scripts; `quench_add_batch()` in
  `tests/CMakeLists.txt` (now with `GPU <backend>:<driver>`) is not part of what games use.
