# Quench, the next release (draft)

Not released: no version or tag yet (the version in `project()` stays 0.10.0). Changes since [0.10.0](v0.10.0.md)
(tag `v0.10.0`): the demo's dual quaternion skinning showcase made readable after a user report, and the Xcode
project's generation in the full gate. This file becomes `v<version>.md` when the user asks for the release; what
was verified is in the devlog of [2026-11-07](../../devlog/2026-11-07.md).

## What changed

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

### Gates

- **`tools/check.sh step xcode`**, also a step of `full`: generates the `ios-xcode` project into
  `build/ios-xcode-check` (configure only: no build, no signing, no team), so that what the Xcode generator refuses
  and Ninja accepts is caught before a package. 0.10.0's iOS package failed that way (fixed in `81fe747`); with that
  fix taken out, the step fails with the same error. `tools/check.sh quick` names the step when CMake files changed
  ([App shell](../app.md#ios-runs)).

## What was verified

From the devlog of [2026-11-07](../../devlog/2026-11-07.md). Mac (Apple M4 Pro).

- **The showcase's captures**, looked at on Metal and on Vulkan through MoltenVK, linear and with the checkbox on:
  the linear capture against the dual quaternion one differs in 5.08 % of the pixels (mean 4.26); the native
  backend's captures are identical to SDL GPU's (0 pixels differ), Vulkan through MoltenVK within 12 levels (mean
  at most 0.0017). `demo_animation_retarget`'s golden images changed with it: the two showcase characters far in its
  background stand further apart (0.28 % of the pixels, the walking pair unchanged).
- **The xcode step:** about 11 to 13 s with its directory in place, 254.5 s the first time (downloads); configured
  with `QUENCH_IOS_TEAM` empty.
- **Gates:** `tools/check.sh full` with the xcode step (2026-11-07, 378.1 s): format; `debug` 486 tests; `release`
  596 (the captures of every backend, Vulkan through MoltenVK included); `asan` 486; `tsan` 486; `wasm-node` 140
  (under Node.js); `ios-sim-tests` 363 in the iOS Simulator; `ios-xcode` generated; a new game built and started.
  All passed.

## What was not verified

- Nobody has looked at the new showcase yet, on the Mac's screen in motion or on a phone; the user's report was
  about the previous one.
- The xcode step generates the project only: a build or signing problem in `xcodebuild` is still found only by a
  device build or `tools/package_demo.sh --ios`.

## Changes for games

None: the changes are in the demo and the engine's own test and gate scripts. `quench_add_batch()` in
`tests/CMakeLists.txt` is not part of what games use.
