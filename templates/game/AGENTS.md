# AGENTS.md

Instructions for coding agents working on Template Game, a game on the Quench engine
(https://github.com/m-brtn/Quench). The overview for people is in `README.md`, the daily development log in `devlog/`.

## Build and test

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
build/debug/template_game                                         # play
build/debug/template_game --capture /tmp/template_game.png --capture-at 1 --quit-after 1
build/debug/template_game --help                                  # engine flags
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
- Graphics only through `quench::gpu` and `quench::render`, never SDL GPU directly. Uniform blocks are defined once in `shaders/*.h` with
  16-byte members only.
- No blocking `while (running)` loop: the engine owns `main` and calls the game (`quench::app::Game`).
- Right-handed, Y up, meters, kilograms, seconds. Reversed depth (near 1, far 0).

## Conventions

- All repository content is in English: code, comments, UI strings, logs, commit messages, docs and the devlog.
- Keep a devlog in `devlog/YYYY-MM-DD.md` (template in `devlog/README.md`): verified results with numbers, separate
  from what was not verified.
- Every asset needs a licence that allows its use, recorded in a `CREDITS.md` next to the files.
- Commit and push only when the user asks; never commit `build/`, captures or secrets.
