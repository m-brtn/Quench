# AGENTS.md

Instructions for coding agents working on Outpost, a game on the Quench engine
(https://github.com/m-brtn/Quench). The overview for people is in `README.md`, the daily development log in `devlog/`.

## Build and test

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
build/debug/outpost                                         # play
build/debug/outpost --capture /tmp/outpost.png --capture-at 1 --quit-after 1
build/debug/outpost --help                                  # engine flags
```

Automated runs (`--script`, `--capture`, `--quit-after`: ctest, captures, scripts) are offscreen on desktops: no window
on the screen, no input from real devices, no sound (the engine's `docs/app.md`, "Offscreen runs"). `--onscreen`
shows the window, for example to watch a scenario or to measure a perf region.

iOS: build the shader compiler and the asset cooker for the Mac first
(`cmake --preset host-tools && cmake --build --preset host-tools`),
then `ios-sim-release` (Simulator, ad-hoc signed) or `ios-xcode` (device, Xcode Automatic signing with
`-DQUENCH_IOS_TEAM=<team id>`). Device runs: install and launch with `xcrun devicectl`.

Web (experimental: WebAssembly on the browser's WebGPU, run so far only in Chrome on a Mac): after `host-tools`,
`emcmake cmake --preset web && cmake --build --preset web` (Homebrew's emscripten), then serve `build/web` over
HTTP and open the game's `.html`; the skill's "Web" section has the scripted check in headless Chrome.

Gates before a commit: build `debug` and `release`, `ctest` for both, take a capture and look at it (do not assume).
Changes that can behave differently on a phone: run on the device, or ask the user to, and log their report.

## Models

`tools/make_models.py` builds every model and `assets/level.json` in Blender, headless; run it by hand after a change
(Blender 5.3 was used) and look at its preview before building the game:

```sh
/Applications/Blender.app/Contents/MacOS/Blender -b --python tools/make_models.py -- --preview build/preview/base.png
```

The build cooks `assets_src/models/*.glb` into `.qmodel` files (`quench_cook_assets` in `CMakeLists.txt`). Blender's
Z up and -Y front become the game's Y up and +Z: game (x, y, z) = Blender (x, z, -y). A model `<name>_outline` is the
black shell drawn around `<name>` (single-sided, turned inside out); the game places both together (`Part`).

## Engine

- How to build a game on the engine (modules, assets, scenarios, captures, iOS, pitfalls): the agent skill in
  `.agents/skills/quench-game/`, copied from the engine by `tools/new_game.py`. When the engine pin moves, copy it
  again from the engine's `.agents/skills/quench-game/` at that commit.
- The engine is fetched at the commit pinned in `CMakeLists.txt` (`GIT_TAG`). For engine work at the same time,
  configure with `-DFETCHCONTENT_SOURCE_DIR_QUENCH=<engine checkout>`; before committing the game, pin a pushed engine
  commit and build without the override.
- The engine's own rules, flags and scenario commands: its `AGENTS.md` and `docs/app.md`.
- Read assets and write user files only through `quench::fs` (assets come from the bundle on iOS); no `std::ifstream`,
  `std::ofstream` or `fopen` in game code.
- Graphics only through `quench::gpu` and `quench::render`, never SDL GPU directly. The game has no shaders
  of its own; if it gets some, uniform blocks are defined once in `shaders/*.h` with 16-byte members only.
- No blocking `while (running)` loop: the engine owns `main` and calls the game (`quench::app::Game`).
- Right-handed, Y up, meters, kilograms, seconds. Reversed depth (near 1, far 0).

## Conventions

- All repository content is in English: code, comments, UI strings, logs, commit messages, docs and the devlog.
- Keep a devlog in `devlog/YYYY-MM-DD.md` (template in `devlog/README.md`): verified results with numbers, separate
  from what was not verified.
- Every asset needs a licence that allows its use, recorded in a `CREDITS.md` next to the files.
- Commit and push only when the user asks; never commit `build/`, captures or secrets.
