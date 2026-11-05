# Performance

The benchmark, how to run and compare it, and the budgets per device. The rule ([AGENTS.md](../AGENTS.md#verification)):
a change that makes the CPU median or 99th percentile more than 5% worse is explained in the devlog or not made. All
documentation: [index](README.md).

## The benchmark

`examples/hello/scenarios/benchmark.txt`: two seconds to settle (deterministic, one 1/120 s tick per frame), then
10 s between `perf_begin` and `perf_end`. Since 2026-11-05 14:05 that region runs in real time like normal play
(frame time from the clock, as many ticks per frame as the time needs), so it measures the real frame cost: about 600
frames at 60 Hz, 1200 at 120 Hz (the script expects at least 300). Runs no longer render identical frames; before,
the region was deterministic too (1201 frames of one tick each). `perf_end` logs one line; a game's
`Game::perfDetails` fields come at its end:

```
perf: hello: 600 frames; frame ms avg 16.667 p10 16.243 p50 16.657 p95 17.248 p99 17.433 max 17.830; cpu ms avg 0.246 p50 0.242 p95 0.314 p99 0.355; gpu ms avg 0.545 min 0.537 p10 0.540 p50 0.544 p95 0.549 (600 samples)
```

- **frame**: wall time between frames. With vsync it is the display interval; it shows missed frames, not cost.
- **cpu**: the frame's CPU work up to the submit, without the wait for a swapchain texture.
- **gpu**: the GPU time of the frame's command buffer (`gpu::Caps::gpuTiming`: Metal only, through SDL's internal
  fence), one sample per frame. Its minimum and 10th percentile are the frames least disturbed by other work on the
  machine.

Run it in Release, with vsync (the default), at a fixed render size on the Mac:

```sh
cmake --build --preset release
build/release/examples/hello/hello --frame-size 1920x1080 --script examples/hello/scenarios/benchmark.txt
```

On the iPhone: `ios-xcode` build, copy the scenario into the app's user directory with `devicectl device copy to`,
launch with `--script benchmark.txt` and `--console` ([App shell](app.md#ios-runs)); the device renders at its
native size.

Why vsync: a window is paced to its display even with `--present-mode immediate` (60 Hz on the Mac's LG UltraFine 5K),
and then the measured GPU time grows to about 15.5 ms, probably because the drawable wait is counted inside the
command buffer (not verified). With vsync the GPU time matches the work (0.55 ms).

## Comparing runs

Timings drift between runs: compare before and after in the same session, three runs each, and compare the medians
of the runs. At today's sizes the CPU time of one run swings by up to two times between runs (CPU median 0.119 to
0.257 ms on the Mac, below), so the 5% rule cannot resolve CPU changes yet; the GPU median is stable to 0.001 ms in
normally paced runs (one run paced like a 120 Hz display read 0.587 ms, last table). Say in the devlog which numbers
a claim rests on.

## Budgets

First budgets, set on 2026-11-05 from the version 0.1.0 runs below (deterministic region) at about twice the measured
values; the real-time runs (Mac 14:05, iPhone 16:45) stay within them. They catch gross
regressions of the sample, not small ones; they are not a frame budget for games. hello is one lit mesh, a ground
plane and an ImGui window.

| Device | Render size | Display | CPU p50 | CPU p99 | GPU p50 | Frames |
|---|---|---|---|---|---|---|
| Mac, Apple M4 Pro | 1920 x 1080 | 60 Hz | 0.5 ms | 1.0 ms | 1.2 ms | p95 within 1.1 display intervals |
| iPhone 18 Pro Max, A20 Pro GPU | 1320 x 2868 (native) | 120 Hz | 0.3 ms | 0.5 ms | 1.6 ms | p95 within 1.1 display intervals |

## Measurements

2026-11-05, version 0.1.0, Release, vsync. These two tables were measured with the deterministic region (1201 frames
of one 1/120 s tick each); the real-time region is in the last table.

Mac, Apple M4 Pro, LG UltraFine 5K at 60 Hz, `--frame-size 1920x1080`, three runs:

| Run | Frame p50 | Frame p95 | Frame max | CPU avg | CPU p50 | CPU p99 | GPU avg | GPU p50 | GPU p95 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 16.667 | 17.264 | 33.033 | 0.140 | 0.119 | 0.335 | 0.557 | 0.545 | 0.588 |
| 2 | 16.671 | 17.247 | 33.327 | 0.263 | 0.257 | 0.436 | 0.554 | 0.545 | 0.608 |
| 3 | 16.669 | 17.085 | 21.518 | 0.248 | 0.243 | 0.363 | 0.548 | 0.545 | 0.549 |

All values in milliseconds. A single frame of 33 ms (one missed display interval) appears in two of the three runs.

iPhone 18 Pro Max (A20 Pro GPU, iOS 27), native size, 120 Hz, launched through `devicectl`, two runs:

| Run | Frame p50 | Frame p95 | Frame max | CPU avg | CPU p50 | CPU p99 | GPU avg | GPU p50 | GPU p95 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 8.332 | 8.438 | 8.902 | 0.149 | 0.145 | 0.223 | 0.808 | 0.807 | 0.812 |
| 2 | 8.332 | 8.437 | 8.667 | 0.149 | 0.146 | 0.222 | 0.810 | 0.808 | 0.813 |

A run before these was interrupted: the app went to the background for 2.2 s during the measured region (cause not
known; the phone had just been unlocked). The engine pauses in the background and resets its clock on return, so the
run still reported 1201 frames, but with a CPU p99 of 0.402 ms; it is not used. Check the log for "entering
background" before using a device run.

2026-11-05 14:05, the region in real time (after the change), Release, vsync, Mac, Apple M4 Pro, LG UltraFine 5K at
60 Hz, `--frame-size 1920x1080`, three runs:

| Run | Frames | CPU p50 | CPU p99 | GPU p50 |
|---|---|---|---|---|
| 1 | 598 | 0.246 | 0.412 | 0.545 |
| 2 | 600 | 0.242 | 0.355 | 0.544 |
| 3 | 601 | 0.231 | 0.346 | 0.544 |

All values in milliseconds; run 2's full line is the example above. Three runs with the deterministic region just
before, in the same session (1201 frames each), had CPU p50 0.247 / 0.221 / 0.240, CPU p99 0.406 / 0.412 / 0.373 and
GPU p50 0.545 / 0.587 / 0.545; one of them was paced like a 120 Hz display (frame p50 8.342 ms) and had a single
frame of 1014.6 ms (cause unknown). Medians of the runs, before and after: CPU p50 0.240 and 0.242 (+0.8 %, within
the run-to-run swing above), CPU p99 0.406 and 0.355, GPU p50 0.545 and 0.544.

2026-11-05 16:45, the region in real time, Release, vsync, iPhone 18 Pro Max (A20 Pro GPU, iOS 27), native size,
120 Hz, launched through `devicectl`, two runs (no "entering background" in either log):

| Run | Frames | Frame p50 | Frame p95 | Frame max | CPU avg | CPU p50 | CPU p99 | GPU avg | GPU p50 | GPU p95 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 1201 | 8.333 | 8.408 | 8.442 | 0.161 | 0.158 | 0.251 | 0.927 | 0.827 | 1.257 |
| 2 | 1201 | 8.333 | 8.403 | 8.477 | 0.147 | 0.142 | 0.215 | 0.824 | 0.809 | 0.875 |

All values in milliseconds. 1201 frames in 10 s of real time is the display's 120 Hz, the same count as the
deterministic region's. Run 1, the first launch after installing, had more slow GPU frames (avg and p95 above run 2
and the deterministic runs, min 0.796 and p10 0.802 as in run 2; cause not checked).
