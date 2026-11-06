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
build/release/examples/hello/hello --frame-size 1920x1080 --script examples/hello/scenarios/benchmark.txt \
    --audio-device null
```

`--audio-device null` (since stage 7): miniaudio's null backend pulls the mix on its own thread like a real output,
without sound; the stage 7 runs below used it.

On the iPhone: `ios-xcode` build, copy the scenario into the app's user directory with `devicectl device copy to`,
launch with `--script benchmark.txt` and `--console` ([App shell](app.md#ios-runs)); the device renders at its
native size.

Why vsync: a window is paced to its display even with `--present-mode immediate` (60 Hz on the Mac's LG UltraFine 5K),
and then the measured GPU time grows to about 15.5 ms, probably because the drawable wait is counted inside the
command buffer (not verified). With vsync the GPU time matches the work (0.55 ms). On 2026-11-06 the same Mac ran
`immediate` unpaced (4286 frames in 10 s, GPU time 0.62 ms on average; cause not known): vsync stays the benchmark's
mode, its frame rate is the display's either way.

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

The engine demo (`examples/demo/scenarios/benchmark.txt`, [Engine demo](demo.md#benchmark); 5 s per section in real
time) has a CPU budget per section from the runs of 2026-11-05 19:34 below, at about twice the largest median. Its
Mac GPU budget comes from the runs of 20:32-20:34 below (after the second GPU app was closed): GPU p50 7.6 to 9.4 ms
at 60 Hz, a value that follows the GPU clock, which follows the load (5.1 ms in a region paced at 120 Hz); the budget
is 15 ms, 90% of the 60 Hz interval, because twice the measured value would allow a missed display interval. The
iPhone budget comes from the run of 2026-11-05 20:28 below: CPU at about twice the largest median; GPU p50 at 7.5 ms,
not twice the measured 6.1 ms, because twice would allow frames longer than the 8.33 ms of the 120 Hz display.

| Device | Render size | Display | CPU p50 | CPU p99 | GPU p50 | Frames |
|---|---|---|---|---|---|---|
| Mac, Apple M4 Pro | 1920 x 1080 | 60 Hz | 2.0 ms | 3.5 ms | 15 ms | p95 within 1.1 display intervals |
| iPhone 18 Pro Max, A20 Pro GPU | 1320 x 2868 (native) | 120 Hz | 2.0 ms | 3.0 ms | 7.5 ms | p95 within 1.1 display intervals |

In the Mac runs of 20:32-20:34 the frame p95 was above 1.1 intervals (18.33 ms) in most sections, up to 18.85 ms,
with frame p50 16.67 ms: presentation jitter outside the budget, not a missed interval in the p95; cause not checked.

## Measurements

Stage 7 A/B, 2026-11-06 01:12-01:41, Release, vsync, Mac, Apple M4 Pro, LG UltraFine 5K at 60 Hz,
`--frame-size 1920x1080`: HEAD `162ae77` (built from `git archive` in `/tmp/qs7head`) against the stage 7 working
tree with `--audio-device null` (miniaudio's null backend: its device thread pulls the mix, no sound), interleaved.
Medians of the runs, HEAD and stage 7, in milliseconds:

| Run | Runs | CPU p50 | CPU p99 | GPU p50 |
|---|---|---|---|---|
| hello, before the fix below (01:12) | 5 + 5 | 0.398, 0.415 (+4.3 %) | 0.536, 0.582 (+8.6 %) | 0.550, 0.550 |
| hello, before the fix, focused (01:22) | 6 + 6 | 0.4145, 0.4295 (+3.6 %) | 0.5455, 0.573 (+5.0 %) | 0.551, 0.551 |
| hello, after the fix (01:33) | 6 + 6 | 0.420, 0.4255 (+1.3 %) | 0.557, 0.548 (-1.6 %) | 0.5495, 0.549 |
| demo post, focused, after the fix (01:39) | 5 + 5 | 1.012, 1.041 (+2.9 %) | 1.417, 1.435 (+1.3 %) | 6.073, 6.075 |

The fix: the monitor of the mix that `audio_measure` reads was on in every scripted run, the benchmarks included.
A temporary timer around the frame's audio update read 9.2 to 9.9 µs at the median and 30 to 37 µs at p99 with it,
4.2 to 4.3 µs and 5.8 to 12 µs without; now only scripts with `audio_measure` turn it on. Without a device
(`--no-audio`, offline mixing on the frame thread) the same timer read 22.9 µs at the median, and hello's CPU p50
0.4485 ms (four runs at 01:22, before the fix).

The complete demo benchmark, three runs per build at 01:12-01:21 (before the fix), gave CPU p50 between -7.1 %
(shadows) and +16.0 % (post) per section and CPU p99 between -4.0 % (physics) and +3.9 % (hdr_on); post on its own
read -3.0 % and +2.0 % (5 + 5 at 01:22) and, after the fix, the row above. Three runs of the fixed build at
01:36-01:39 against the same HEAD runs: CPU p50 -4.9 % (sky) to +18.2 % (post, whose HEAD runs read 0.813 to
1.041), CPU p99 -6.6 % (assets) to +2.9 % (hdr). GPU p50 the same within 0.01 ms in every section. The new `audio`
region (three 3D emitters, the generated tone and streamed music on the null device), fixed build: 300 to 301
frames, frame p95 at most 17.270, CPU p50 1.050, CPU p99 1.486, GPU p50 6.574 ms (three runs), within the demo's
budget.

Stage 6 A/B, 2026-11-05 23:23-23:37, Release, vsync, Mac, Apple M4 Pro, LG UltraFine 5K at 60 Hz,
`--frame-size 1920x1080`: HEAD `8898598` (built from `git archive` in `/tmp/qs6head`) against the stage 6 working
tree, interleaved, nothing else rendering. The CPU times of this session are about 1.5 to 2 times those of 21:58
(hello CPU p50 0.255 ms then, 0.41 ms now, in both builds): a difference between sessions, which is why only the
interleaved runs are compared. Medians of the runs, HEAD and stage 6, in milliseconds:

| Run | Runs | CPU p50 | CPU p99 | GPU p50 |
|---|---|---|---|---|
| hello | 5 + 5 | 0.409, 0.411 (+0.5 %) | 0.594, 0.616 (+3.7 %) | 0.548, 0.548 |
| demo materials (focused) | 4 + 4 | 1.2145, 1.220 (+0.5 %) | 1.567, 1.612 (+2.9 %) | 7.182, 7.190 |
| demo sky (focused) | 4 + 4 | 1.1085, 1.142 (+3.0 %) | 1.333, 1.395 (+4.7 %) | 6.394, 6.399 |
| demo physics (focused) | 4 + 4 | 1.3585, 1.374 (+1.1 %) | 1.7045, 1.7495 (+2.6 %) | 5.553, 5.556 |

The complete demo benchmark, three runs per build, gave CPU p50 between -10 % (sky) and +3.4 % (materials) per
section and CPU p99 between -7.2 % (post) and +8.9 % (sky), with the per-run values of the two builds overlapping in
every section; the three sections above 5 % in p99 (materials +6.6 %, sky +8.9 %, physics +6.4 %) were measured
again on their own (the focused rows). GPU p50 the same within 0.012 ms in every section. The new `assets` region (streaming with the moving
focus, the default budgets): CPU p50 1.232, p99 1.641, GPU p50 6.445 ms (three runs), within the demo's budget.

Jolt's step time, a host program against `PhysicsWorld` (2,000 boxes falling on a floor, 600 steps of 1/120 s after
120), three interleaved runs each: Jolt's own pool (HEAD, 4 workers, concurrency 5) median 0.662, 0.576, 0.588 ms
(p99 3.411, 0.689, 1.299); the engine's job system (11 workers, concurrency 5) median 0.580, 0.576, 0.591 ms (p99
0.729, 0.705, 0.763). Without workers: 1.315 ms (HEAD) and 1.322 ms.

2026-11-05 19:34, the engine demo's benchmark, Release, vsync, Mac, Apple M4 Pro, LG UltraFine 5K at 60 Hz,
`--frame-size 1920x1080`, three runs, medians of the runs (frame p95 and max: the largest of the three). During these
runs a second app was rendering on the same GPU (an interactive `QuenchDemo.app` started from `dist/` at 18:32), so
the GPU times are not the demo's alone: hello's GPU p50 read 0.6 to 2.2 ms in the same session against 0.545 ms in
clean runs.

| Section | Frames | Frame p50 | Frame p95 | Frame max | CPU p50 | CPU p99 | GPU p50 (shared GPU) |
|---|---|---|---|---|---|---|---|
| materials | 301 | 16.682 | 17.887 | 18.607 | 0.899 | 1.433 | 8.167 |
| shadows | 300 | 16.669 | 17.669 | 32.709 | 0.857 | 1.737 | 7.799 |
| sky | 300 | 16.664 | 17.868 | 21.320 | 0.885 | 1.399 | 7.108 |
| post | 300 | 16.694 | 17.840 | 20.256 | 0.920 | 1.291 | 8.410 |
| hdr (SDR) | 300 | 16.690 | 17.829 | 33.116 | 0.871 | 1.297 | 6.110 |
| hdr (HDR output on) | 300 | 16.677 | 17.946 | 32.639 | 0.900 | 1.348 | 6.011 |
| physics | 301 | 16.667 | 17.746 | 32.928 | 1.038 | 1.553 | 7.418 |

All values in milliseconds. HDR output (an `Rgba16Float` frame and the UI composite pass; headroom 1 on this display)
cost 0.03 ms of CPU p50 and 0.05 ms of CPU p99 over the same section in SDR.

2026-11-05 20:28, the engine demo's benchmark on the iPhone 18 Pro Max (A20 Pro GPU, iOS 27), Release, vsync, native
size 1320 x 2868 (portrait), 120 Hz, launched through `devicectl`, no validation layer, one complete run (9 passed,
0 failed; no "entering background" in its log). HDR output on the phone ran at the headroom the display reported
then (1.20).

| Section | Frames | Frame p50 | Frame p95 | Frame max | CPU p50 | CPU p99 | GPU min | GPU p10 | GPU p50 | GPU p95 |
|---|---|---|---|---|---|---|---|---|---|---|
| materials | 601 | 8.334 | 8.397 | 8.608 | 0.805 | 1.315 | 5.621 | 5.897 | 6.028 | 6.194 |
| shadows | 601 | 8.331 | 8.411 | 8.610 | 0.836 | 1.271 | 5.810 | 5.872 | 5.984 | 6.163 |
| sky | 601 | 8.332 | 8.403 | 8.559 | 0.759 | 1.125 | 4.533 | 5.894 | 6.058 | 6.179 |
| post | 601 | 8.333 | 8.403 | 8.687 | 0.837 | 1.237 | 5.650 | 5.915 | 6.006 | 6.137 |
| hdr (SDR) | 601 | 8.332 | 8.414 | 8.733 | 0.782 | 1.274 | 5.736 | 5.858 | 6.037 | 6.264 |
| hdr (HDR output on) | 601 | 8.334 | 8.418 | 8.735 | 0.824 | 1.350 | 5.843 | 5.872 | 6.010 | 6.104 |
| physics | 601 | 8.331 | 8.409 | 8.519 | 0.948 | 1.377 | 5.597 | 5.914 | 6.003 | 6.140 |

All values in milliseconds. The GPU time is about the same in every section (full-screen passes at 3.8 million
pixels dominate); 6.0 ms is 72% of the 120 Hz frame. An earlier run (20:26) gave the same numbers for its first four
sections (CPU p50 0.788 to 0.838, CPU p99 up to 1.547, GPU p50 6.061 to 6.102) before it was killed by another
agent's launch of the same app (devlog).

hello A/B for stage 5, 2026-11-05 19:30, six interleaved pairs of v0.2.0 (`e731178`, built from `git archive` in
`/tmp/qs5_base`) and the stage 5 working tree, same conditions and the same second app on the GPU: medians of the runs
CPU p50 0.3225 and 0.335 ms (+3.9 %), CPU p99 0.538 and 0.5555 ms (+3.3 %); the per-run differences go both ways
(run 4: 0.394 base, 0.388 new; run 5: 0.313 base, 0.338 new). hello does not use the render module; stage 5 changed
its app-shell path only (the display output, ImGui's paper white).

2026-11-05 20:32-20:34, the engine demo's benchmark again on the Mac (same settings as at 19:34), after the second
app had been closed; the two booted iOS Simulators were still running, and a hello run right after read GPU min
0.540, p10 0.550 but p50 0.934 ms (0.545 ms in clean runs), so the GPU was still not entirely free. Three runs,
medians of the runs (frame p95 and max: the largest of the three); run 3 was paced at 120 Hz from the `hdr_on`
region on (599 frames, GPU p50 5.19 and 5.11 ms there):

| Section | Frames | Frame p50 | Frame p95 | Frame max | CPU p50 | CPU p99 | GPU p50 |
|---|---|---|---|---|---|---|---|
| materials | 301 | 16.682 | 18.701 | 19.017 | 0.948 | 1.369 | 9.395 |
| shadows | 300 | 16.644 | 18.387 | 98.908 | 0.875 | 1.278 | 8.573 |
| sky | 301 | 16.652 | 18.458 | 19.335 | 0.761 | 1.224 | 8.685 |
| post | 300 | 16.655 | 18.093 | 22.202 | 0.822 | 1.285 | 8.019 |
| hdr (SDR) | 299 | 16.669 | 18.851 | 1019.100 | 0.867 | 1.272 | 7.613 |
| hdr (HDR output on) | 301 | 16.641 | 18.798 | 25.629 | 0.881 | 1.344 | 7.849 |
| physics | 301 | 16.624 | 18.476 | 24.103 | 1.091 | 1.473 | 7.745 |

All values in milliseconds. The 98.9 ms and 1019.1 ms maxima are single stalls in run 3 (294 and 266 frames in
those regions); cause not checked.

A second complete iPhone run of the demo's benchmark, 2026-11-05 20:26:59 (same settings as the 20:28 run above),
agrees with it: 601 frames in every region, CPU p50 0.771 to 0.975, CPU p99 1.275 to 1.552, GPU p50 5.959 to
6.064 ms. Two more runs were ended early by a second agent's launches of the same app on the phone (devlog); their
complete regions (materials, shadows, sky) read the same.

hello on the iPhone for stage 5, 2026-11-05 20:23 and 20:29-20:30, same settings as the 16:45 runs below. Two runs of
the working tree at 20:22:57 and 20:23:20 (a first at 20:20 went to the background after 1.8 s, when a second agent
launched another app on the phone, and is not used): CPU p50
0.157 / 0.155, CPU p99 0.393 / 0.349, GPU p50 0.809 / 0.807 ms. Their CPU p99 is above the 16:45 runs (0.251 /
0.215), so an interleaved A/B followed, v0.2.0 (`e731178`, built for the device from `git archive` in `/tmp/qs5_base`)
against the working tree, two pairs, 1201 frames each:

| Run | CPU p50 | CPU p99 | GPU p50 | GPU p95 |
|---|---|---|---|---|
| v0.2.0, 1 | 0.161 | 0.342 | 0.808 | 1.666 |
| stage 5, 1 | 0.164 | 0.355 | 0.810 | 0.816 |
| v0.2.0, 2 | 0.156 | 0.329 | 0.809 | 0.817 |
| stage 5, 2 | 0.160 | 0.316 | 0.809 | 0.817 |

All values in milliseconds. Medians of the runs: CPU p50 0.1585 and 0.162 (+2.2 %), CPU p99 0.3355 and 0.3355 (0 %),
GPU p50 0.8085 and 0.8095. The higher CPU p99 against 16:45 is in v0.2.0 as well: a difference between sessions, not
the change. Within the iPhone budgets.

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
