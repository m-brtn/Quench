# Quench, the next release (draft)

Not released: no version or tag yet (the version in `project()` stays 0.10.0). Changes since [0.10.0](v0.10.0.md)
(tag `v0.10.0`): an agent skill on building a game on the engine, copied into every new game; the demo's dual
quaternion skinning showcase made readable after a user report, and its characters' wrists closed; the Xcode
project's generation in the full gate. This file becomes `v<version>.md` when the user asks for the release; what
was verified is in the devlog of [2026-11-07](../../devlog/2026-11-07.md).

## What changed

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

- Nobody has looked at the new showcase yet, on the Mac's screen in motion or on a phone; the user's report was
  about the previous one. The closed wrists were looked at in still captures only, not while a hand turns (the
  hero's wave) or in motion.
- The skill: one dry run on the Mac (Debug, Release, the iOS Simulator); its animation, jobs, surface shader and
  device parts were not run. `tools/check_skill.py` finds renamed or removed names, not changed behaviour.
- The xcode step generates the project only: a build or signing problem in `xcodebuild` is still found only by a
  device build or `tools/package_demo.sh --ios`.

## Changes for games

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
  `tests/CMakeLists.txt` is not part of what games use.
