# Template Game

A game on the [Quench](https://github.com/m-brtn/Quench) engine. Move the cube with WASD or the arrow keys, the left
stick of a gamepad, or by dragging a finger on a touch screen.

## Build

Requirements: CMake 3.28+, Ninja, a C++20 compiler; on Apple platforms Xcode with the Metal toolchain. The first
configure downloads the engine and its dependencies.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
build/debug/template_game
```

Engine flags (`--help` lists them all): `--quit-after S`, `--capture FILE`, `--capture-at S`, `--script FILE`,
`--window WxH`, `--frame-size WxH`, `--offscreen`, `--onscreen` (automated runs are offscreen unless it is given),
`--gpu-backend metal|sdl` (the native Metal backend is the default on Apple platforms, SDL GPU elsewhere),
`--gpu-driver NAME`, `--gpu-debug`, `--present-mode vsync|immediate|mailbox`, `--no-audio`, `--audio-device NAME`
(`null`: no sound), `--audio-capture FILE.wav`.

## Layout

```
src/Game.cpp        the game (quench::app::Game)
shaders/            HLSL compiled at build time; GameUniforms.h is shared with C++
assets/             files read through quench::fs (copied next to the executable, or into the app bundle)
scenarios/          scripted checks (--script), run by ctest
icon/icon.png       app icon (iOS)
devlog/             development log
```
