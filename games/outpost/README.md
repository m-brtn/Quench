# Outpost

An experiment on the [Quench](https://github.com/m-brtn/Quench) engine: a small outpost seen from an isometric camera
(a hangar with four pods, a tower, a command module, fuel tanks, lamps) and a two-legged walker that gathers energy
cells around it. The walker carries up to three cells to the hangar door; every three delivered cells bring one of the
four pods online.

Every model is built by a script in Blender (`tools/make_models.py`); the shapes follow a concept picture the author
of the experiment supplied (`assets_src/models/CREDITS.md`).

## Controls

| Input | Action |
| --- | --- |
| Click on the ground | the walker goes there |
| W A S D, arrows | steer the walker, relative to the camera |
| Q, E; right mouse drag | turn the camera |
| Mouse wheel | zoom (close in, the camera follows the walker) |
| T | day or the blue hour (the sun slider in the panel) |
| P | autopilot: the walker gathers and delivers by itself (`--autopilot` starts with it) |

The panel also switches the outlines and, where the GPU can, ray-traced shadows and ambient occlusion.

Detail (low, high) in the panel: low renders two thirds of the frame each way with lighter shadows, no screen-space
occlusion and the lamps off by day, and is the default on a phone or tablet (also in its browser), where the game
also runs at 60 frames per second at most. Adapt to load (on) lowers the render scale further while frames come
late and raises it again when they fit (the engine's `App::loadGovernor()`). The panel's last line shows the size
the picture is rendered at. `--mobile` (an engine flag) gives a phone's defaults on a desktop. Not yet tried on a
phone.

## Build

Requirements: CMake 3.28+, Ninja, a C++20 compiler; on Apple platforms Xcode with the Metal toolchain. The first
configure downloads the engine and its dependencies; inside the engine's checkout, add
`-DFETCHCONTENT_SOURCE_DIR_QUENCH=<engine checkout>` to use that checkout.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
build/release/outpost                 # play (opens a window)
build/release/outpost --autopilot     # watch
build/release/outpost --frame-size 1920x1080 --capture /tmp/outpost.png --capture-at 2 --quit-after 2   # offscreen
```

Engine flags (`--help` lists them all): `--quit-after S`, `--capture FILE`, `--capture-at S`, `--script FILE`,
`--window WxH`, `--frame-size WxH`, `--offscreen`, `--onscreen` (automated runs are offscreen unless it is given),
`--gpu-backend metal|sdl`, `--mobile`, `--no-audio`, `--audio-device NAME` (`null`: no sound). The game makes no sound.

Scenario values, commands and points of the game (for `--script`): `scenarios/smoke.txt` uses most of them;
`Outpost::scenarioValue`, `scenarioCommand` and `scenarioPoint` in `src/Game.cpp` list them all.

## Layout

```
src/Game.cpp            the game (quench::app::Game): scene, lights, navigation, the walker's legs, the cells, the UI
tools/make_models.py    builds the models and assets/level.json in Blender, headless (run by hand)
assets_src/models/      the models as GLB, cooked into .qmodel files at build time; CREDITS.md
assets/level.json       where things stand, in game coordinates (written by tools/make_models.py)
scenarios/              scripted checks (--script), run by ctest
icon/icon.png           app icon (iOS; the template's)
devlog/                 development log
```
