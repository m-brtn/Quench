# README screenshots

| File | Source | Rights |
| --- | --- | --- |
| `particles.jpg` | Quench Demo 0.12.1, Particles section: light streams, sparks and fireflies | Original project capture, MIT; procedural scene assets |
| `weather.jpg` | Quench Demo 0.12.1, Weather section: towering clouds at sunset | Original project capture, MIT; procedural scene |
| `raytracing.jpg` | Quench Demo 0.12.1, Ray tracing section: cut-out shadows, glass and reflective spheres | Original project capture, MIT; [demo asset credits](../../examples/demo/assets/CREDITS.md) |
| `cloudsong.jpg` | [Cloudsong](https://github.com/m-brtn/cloudsong), version 0.1.1: starting meadow and ruins | Reproduced with the project owner's permission for this showcase; [game artwork credits](https://github.com/m-brtn/cloudsong/blob/main/CREDITS.md) |

Captured on 2026-11-09 on an Apple M4 Pro Mac, native Metal, offscreen at 1920 x 1080 with the interface hidden.
The demo uses the regular renderer, including real-time ray tracing in its Ray tracing section. The images are
resized to 1600 x 900 with Lanczos filtering and saved as optimized JPEGs at quality 90, with no colour retouching
or compositing. The Cloudsong file is identical to the image in the game's README. Its generated character and
architecture are game artwork, separate from the demo's procedural content; the linked credits retain their terms.

## Reproduce the demo captures

From the repository root, after building the Release demo:

```sh
build/release/examples/demo/quench-demo --gpu-backend metal --frame-size 1920x1080 \
  --audio-device null --user-dir build/showcase \
  --script examples/demo/scenarios/showcase.txt
```

The scenario writes `weather.png`, `particles.png` and `raytracing.png` under `build/showcase/` and exits.
It freezes the weather clock, restarts the particle streams and waits for the ray tracing scene to load.
It is a gallery capture scenario, independent of the regression tests and their golden images.

The game's [capture scenario and settings](https://github.com/m-brtn/cloudsong/blob/main/docs/images/CREDITS.md)
reproduce the Cloudsong view. Captures document the Mac renderer, not the appearance of an iPhone display.
