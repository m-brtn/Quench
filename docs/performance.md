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
  fence on SDL GPU, directly on the native Metal backend), one sample per frame. Its minimum and 10th percentile are
  the frames least disturbed by other work on the machine.

On the native Metal backend (`--gpu-backend metal`, since stage 9) a second line follows with the GPU time per pass
([below](#gpu-time-per-pass)).

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

## GPU time per pass

With `gpu::Caps::gpuPassTiming` (the native Metal backend; SDL GPU has no timestamp queries, so it reports false and
logs nothing of this) the frame's command buffer is timed per range of passes ([Architecture](architecture.md#gpu-timestamps-per-pass)):
the app's `game` (around `Game::render`), `ui` and `blit`, and inside `game` the renderer's `upload` (its per-frame
buffers), `accel` (acceleration structure builds and refits), `prepass` (the depth prepass for the rays), `raytrace`
(with `rays` and `denoise` inside), `shadows` (all cascades), `hdr` (the main pass with the sky), `taa`, `upscale`
(MetalFX), `bloom` and `tonemap`; the ray tracing and MetalFX ranges only where those run. A range's time is its busy
time: how long any stage of its passes ran, from timestamps at the stage boundaries.

The ranges are recorded only while something reads them: a scenario's perf region, a frame panel that calls
`App::requestPassTiming()` (the demo's menu and ray tracing windows) or `--gpu-pass-timing`
([App shell](app.md#command-line-flags)). Recording costs CPU time in every frame (0.09 ms in the demo's shadows
section and 0.14 ms in its ray tracing section, CPU p50, below). `perf_end` logs one more line after the perf line:

```
perf: materials: GPU busy ms p50 per timed range: game 3.908, upload 0.006, shadows 0.344, hdr 2.070, taa 0.860, bloom 0.471, tonemap 0.269, ui 0.081, blit 0.113; top-level ranges: sum 4.143, span 4.106 = 99.5% of the frame's GPU time
```

- **sum**: the busy times of the top-level ranges (`game`, `ui`, `blit`) added up, per frame.
- **span**: from the first timestamp of the first top-level range to the last of the last, per frame.
- **%**: span / the frame's GPU time (the command buffer's start to end), per frame. It shows how much of the
  measured frame the ranges cover; 99 to 100 % means nothing big runs outside them.

The values are medians per range over the region (scenario values `perf_pass_<range>_p50`, `perf_pass_sum_p50`,
`perf_pass_span_p50`, `perf_pass_share_p50`), so they do not add up exactly. The sum can exceed the span: an Apple GPU
starts the vertex stage of a pass while the previous pass still shades fragments, and that time counts for both. The
demo's frame panel shows the last frame's ranges with their start and end in the frame. Not in the ranges: the
skinning compute pass (its own command buffer, `RenderStats::skinningGpuMs`) and `Device::upload`s.

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

The demo's `animation` region (a crowd of 256, 259 characters; since stage 8) is a load test and has its own Mac
budget, at about twice the medians of the runs of 2026-11-06 04:22-04:34 below: CPU p50 5.5 ms, CPU p99 7.5 ms,
GPU p50 15 ms as for the other sections. It is above the sections' budget of 2.0 and 3.5 ms (they have a few dozen
instances). No iPhone budget for it yet: it has not run on the phone.

## Measurements

Stage 9 (second half: ray tracing, MetalFX, timing off unless read), 2026-11-06 08:21-08:49, Release, vsync, Mac,
Apple M4 Pro, `--frame-size 1920x1080`, `--audio-device null`. The display paced at about 120 Hz (hello 1167 to 1199
frames in 10 s, the demo 493 to 601 in 5 s). All values in milliseconds, medians of the runs.

SDL GPU (the default) before and after, interleaved: `96a50e7` (the first half, its Release binaries in a temporary
copy) against the working tree. The demo ran `96a50e7`'s `benchmark.txt` in both builds (the same regions; the new
ray tracing region is measured below):

| Run | Runs | CPU p50 | CPU p99 | GPU p50 |
|---|---|---|---|---|
| hello | 11 + 11 | 0.265 / 0.266 (+0.4 %) | 0.455 / 0.427 (-6.2 %) | 0.551 / 0.551 |
| demo materials | 3 + 3 | 1.091 / 1.083 (-0.7 %) | 1.487 / 1.438 (-3.3 %) | 5.687 / 5.125 |
| demo shadows | 3 + 3 | 0.982 / 0.991 (+0.9 %) | 1.315 / 1.337 (+1.7 %) | 5.375 / 5.732 |
| demo sky | 3 + 3 | 0.949 / 0.917 (-3.4 %) | 1.269 / 1.256 (-1.0 %) | 5.575 / 3.539 |
| demo post | 3 + 3 | 0.985 / 0.973 (-1.2 %) | 1.256 / 1.294 (+3.0 %) | 3.346 / 3.286 |
| demo hdr | 3 + 3 | 0.897 / 0.859 (-4.2 %) | 1.234 / 1.153 (-6.6 %) | 2.739 / 4.171 |
| demo hdr_on | 3 + 3 | 0.939 / 0.929 (-1.1 %) | 1.246 / 1.248 (+0.2 %) | 2.840 / 3.351 |
| demo physics | 3 + 3 | 1.050 / 1.036 (-1.3 %) | 1.426 / 1.381 (-3.2 %) | 4.109 / 2.896 |
| demo assets | 3 + 3 | 0.954 / 0.998 (+4.6 %) | 1.357 / 1.421 (+4.7 %) | 3.393 / 3.505 |
| demo audio | 3 + 3 | 0.950 / 0.955 (+0.5 %) | 1.296 / 1.326 (+2.3 %) | 3.452 / 4.485 |
| demo animation | 3 + 3 | 2.707 / 2.699 (-0.3 %) | 3.656 / 3.633 (-0.6 %) | 5.311 / 5.134 |

Every row within 5 %: the SDL GPU path is unchanged (the renderer checks the new settings and skips the ray tracing
and MetalFX work; hello does not use the renderer). The GPU p50 moves with the GPU clock run to run, as before.

The native backend without timestamps against SDL GPU (the first half measured 6 to 11 % more CPU time on the native
backend with timestamps on in every frame): the working tree, one section per process (`section`, `wait 1`, a 5 s
region), interleaved, 3 + 3 runs:

| Section | CPU p50 SDL GPU / native | CPU p99 SDL GPU / native | GPU p50 SDL GPU / native |
|---|---|---|---|
| materials | 1.094 / 1.033 (-5.6 %) | 1.510 / 1.423 (-5.8 %) | 5.509 / 5.286 |
| shadows | 0.948 / 0.898 (-5.3 %) | 1.303 / 1.297 (-0.5 %) | 3.591 / 3.376 |
| animation (crowd of 24) | 1.554 / 1.544 (-0.6 %) | 2.117 / 2.095 (-1.0 %) | 4.669 / 4.634 |

Without timestamps the native backend's CPU time is that of SDL GPU or lower, within the noise; CPU profiles of the
two (`sample` during the shadows section, one run each) showed the same distribution between the renderer, the
encoders and the driver. Nothing in the backend needed a fix: the gap was the timestamps, which are now off unless read. Their
cost when on, the native backend, one process per run, 3 + 3 runs:

| Section | CPU p50 off / on | CPU p99 off / on | GPU p50 off / on |
|---|---|---|---|
| shadows | 0.909 / 0.999 (+0.09) | 1.188 / 1.405 | 3.375 / 3.389 |
| raytracing | 1.311 / 1.455 (+0.14) | 1.871 / 2.011 | 6.160 / 6.223 |

The ray tracing section on the native backend (`scenarios/raytracing_benchmark.txt`: the sun turning, the character
walking, 12 instances in the top level; 3 runs) and, for the fallback rows, the same scenario on SDL GPU (1 run;
every region there runs shadow maps with bilinear scaling at the region's render size). GPU busy time per range,
medians of the runs:

| Region | Render size | CPU p50 | CPU p99 | GPU p50 | accel | prepass | rays | denoise | shadows | hdr | taa | upscale |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| RT shadows (2 rays, softness 1) + RT AO (2 rays) | 1920 x 1080 | 1.420 | 2.003 | 6.184 | 0.446 | 0.040 | 3.075 | 0.972 | | 0.478 | 0.603 | |
| RT shadows, no AO | 1920 x 1080 | 1.478 | 1.985 | 5.961 | 0.643 | 0.062 | 2.320 | 0.547 | | 0.711 | 0.878 | |
| RT hard shadows (1 ray, softness 0) | 1920 x 1080 | 1.416 | 1.913 | 5.551 | 0.846 | 0.072 | 1.570 | | | 0.738 | 0.880 | |
| Shadow maps (3 cascades) | 1920 x 1080 | 1.287 | 1.709 | 4.906 | | | | | 0.320 | 1.767 | 0.885 | |
| RT + AO, MetalFX temporal, scale 1 | 1920 x 1080 | 1.217 | 1.918 | 6.513 | 0.380 | 0.034 | 2.296 | 0.836 | | 0.421 | 0.081 | 2.143 |
| RT + AO, MetalFX temporal, scale 0.67 | 1286 x 724 | 1.321 | 2.014 | 6.056 | 0.603 | 0.037 | 1.774 | 0.526 | | 0.280 | 0.079 | 2.185 |
| RT + AO, MetalFX temporal, scale 0.5 | 960 x 540 | 1.342 | 2.154 | 5.593 | 0.643 | 0.033 | 1.470 | 0.378 | | 0.215 | 0.068 | 2.089 |
| RT + AO, MetalFX spatial, scale 0.5 | 960 x 540 | 1.566 | 2.134 | 4.604 | 0.645 | 0.034 | 1.396 | 0.381 | | 0.225 | 0.281 | 0.677 |
| RT + AO, bilinear, scale 0.5 | 960 x 540 | 1.380 | 1.832 | 3.317 | 0.614 | 0.029 | 1.188 | 0.377 | | 0.216 | 0.264 | |
| SDL GPU fallback (shadow maps) | 1920 x 1080 | 1.226 | 1.696 | 4.668 | | | | | | | | |
| SDL GPU fallback, bilinear, scale 0.5 | 960 x 540 | 1.042 | 1.530 | 3.022 | | | | | | | | |

Ray-traced shadows cost about 1 ms of GPU time per frame more than shadow maps at 1080p in this scene (5.6 to 6.0 ms
against 4.9 ms; the frame's GPU time) and 0.1 to 0.2 ms more CPU time (the acceleration structure pass, the prepass
and two more passes). The rays are the largest part: 1.6 ms for one hard shadow ray per pixel, 2.3 ms for two soft
ones, 3.1 ms with two occlusion rays on top. The ranges of one run do not compare exactly across regions: the GPU
clock follows the load (the same `hdr` pass reads 0.48 ms in the heaviest region and 0.74 ms in lighter ones), so
the frame's GPU time is the number to compare. MetalFX's temporal scaler costs about 2.1 ms at a 1920 x 1080 output
whatever the input size, more than the rays it saves at scale 0.67 or 0.5 here: in this scene it is a quality
option (anti-aliasing and reconstruction in TAA's place, whose own pass drops to 0.07 ms), not a speed-up. The
spatial scaler costs 0.68 ms. The section's CPU time stays within the demo's budget (2.0 ms p50, 3.5 ms p99) in
every region.

The demo benchmark's new `raytracing` region (`benchmark.txt`, one run per backend, 09:13-09:16): SDL GPU (the
fallbacks) CPU p50 1.307, CPU p99 1.757, GPU p50 5.441; the native backend (rays and occlusion) CPU p50 1.426, CPU
p99 1.958, GPU p50 6.202; within the demo's budget.

Skinned bottom levels, the animation section with a crowd of 256 (259 characters) and ray-traced shadows on the
native backend, one run each: the `accel` range 1.57 ms with refits (a build after every 60), 5.12 ms with a
temporary change that built every bottom level every frame. With the counts not yet staggered, every character
built again in the same frame once a second, and the region's CPU p99 read 4.4 ms; staggered, 3.8 ms.

Stage 9 (first half: the native Metal backend and GPU timestamps), 2026-11-06 06:41-07:10, Release, vsync, Mac,
Apple M4 Pro, `--frame-size 1920x1080`, `--audio-device null`, interleaved with the order rotated: `3a96779` (its
Release binaries, copied to `/tmp/quench-baseline`), the working tree on SDL GPU (the default) and the working tree
on the native backend (`--gpu-backend metal`). The display paced at about 120 Hz (hello 1179 to 1199 frames in 10 s,
the demo 456 to 587 in 5 s, its frame p95 near 25 ms as in the sessions below). Medians of the runs, `3a96779` / SDL
GPU / native, in milliseconds; SDL GPU against `3a96779` and native against SDL GPU in parentheses. The focused rows
ran the four sections alone (`section`, `wait 1`, 5 s region each) to resolve the full runs' differences:

| Run | Runs | CPU p50 | CPU p99 | GPU p50 |
|---|---|---|---|---|
| hello | 6 + 6 + 6 | 0.267 / 0.258 (-3.2 %) / 0.264 (+2.1 %) | 0.440 / 0.451 (+2.5 %) / 0.462 (+2.3 %) | 0.551 / 0.550 / 0.550 |
| demo materials | 3 + 3 + 3 | 1.032 / 1.029 (-0.3 %) / 1.055 (+2.5 %) | 1.437 / 1.419 (-1.3 %) / 1.478 (+4.2 %) | 4.852 / 4.674 / 4.112 |
| demo shadows | 3 + 3 + 3 | 0.883 / 0.991 (+12.2 %) / 0.993 (+0.2 %) | 1.224 / 1.318 (+7.7 %) / 1.342 (+1.8 %) | 3.315 / 5.402 / 4.343 |
| demo shadows (focused) | 6 + 6 + 6 | 0.905 / 0.897 (-0.8 %) / 0.981 (+9.4 %) | 1.231 / 1.279 (+3.9 %) / 1.353 (+5.8 %) | 3.415 / 3.389 / 4.554 |
| demo sky | 3 + 3 + 3 | 0.880 / 0.897 (+1.9 %) / 0.982 (+9.5 %) | 1.226 / 1.251 (+2.0 %) / 1.375 (+9.9 %) | 4.443 / 5.051 / 5.246 |
| demo sky (focused) | 6 + 6 + 6 | 0.910 / 0.905 (-0.5 %) / 1.004 (+11.0 %) | 1.250 / 1.200 (-4.0 %) / 1.372 (+14.3 %) | 5.475 / 3.734 / 5.692 |
| demo post | 3 + 3 + 3 | 0.913 / 0.934 (+2.3 %) / 1.019 (+9.1 %) | 1.283 / 1.263 (-1.6 %) / 1.368 (+8.3 %) | 3.680 / 4.207 / 4.957 |
| demo hdr | 3 + 3 + 3 | 0.892 / 0.842 (-5.6 %) / 0.905 (+7.5 %) | 1.270 / 1.101 (-13.3 %) / 1.295 (+17.6 %) | 3.759 / 2.750 / 2.776 |
| demo hdr_on | 3 + 3 + 3 | 0.928 / 0.887 (-4.4 %) / 0.914 (+3.0 %) | 1.262 / 1.253 (-0.7 %) / 1.227 (-2.1 %) | 2.931 / 3.060 / 3.051 |
| demo physics | 3 + 3 + 3 | 0.961 / 0.986 (+2.6 %) / 1.034 (+4.9 %) | 1.382 / 1.460 (+5.6 %) / 1.442 (-1.2 %) | 2.956 / 2.883 / 2.863 |
| demo physics (focused) | 6 + 6 + 6 | 0.909 / 0.907 (-0.3 %) / 0.959 (+5.8 %) | 1.291 / 1.246 (-3.5 %) / 1.367 (+9.7 %) | 2.881 / 2.920 / 2.878 |
| demo assets | 3 + 3 + 3 | 0.971 / 0.934 (-3.8 %) / 0.974 (+4.3 %) | 1.344 / 1.300 (-3.3 %) / 1.448 (+11.4 %) | 4.767 / 3.404 / 3.393 |
| demo audio | 3 + 3 + 3 | 0.879 / 0.843 (-4.1 %) / 1.008 (+19.6 %) | 1.291 / 1.204 (-6.7 %) / 1.367 (+13.5 %) | 3.779 / 3.443 / 4.367 |
| demo audio (focused) | 6 + 6 + 6 | 0.955 / 0.953 (-0.2 %) / 1.051 (+10.3 %) | 1.251 / 1.304 (+4.3 %) / 1.437 (+10.2 %) | 3.963 / 4.558 / 5.659 |
| demo animation | 3 + 3 + 3 | 2.633 / 2.617 (-0.6 %) / 2.578 (-1.5 %) | 3.540 / 3.568 (+0.8 %) / 3.468 (-2.8 %) | 5.261 / 5.338 / 5.300 |

SDL GPU against `3a96779`: within 5 % in hello and in every focused row (the full runs' shadows +12.2 % and
physics p99 +5.6 % did not repeat with six runs each); the default path is unchanged. Native against SDL GPU: the
same GPU time (hello 0.550 ms both; the demo's GPU medians move with the GPU clock run to run, as before) and in
the demo 6 to 11 % more CPU time in the focused rows. That is the timestamps, not the backend: in a second
interleaved set (07:02-07:08, the four focused sections, 5 + 5 + 5 runs) with the timestamps switched off by a
temporary change (removed again), the native backend without timestamps / SDL GPU / native with them read CPU p50
0.868 / 0.909 / 1.014 (shadows), 0.915 / 0.892 / 0.998 (physics), 0.852 / 0.914 / 0.925 (sky) and 0.937 / 0.930 /
1.030 (audio): without timestamps the native backend is within 7 % of SDL GPU either way, and the timestamps add
about 0.1 ms per frame. Of that, measured with temporary timers in one run: resolving the previous frame's samples
(`Device::gpuTimings`) 19 µs per frame, and the demo's frame panel lines for the ranges 22 µs (their drawing comes
on top); the rest is presumably the driver's work for the sample attachments of each encoder (not measured on its
own). hello has three ranges and no panel lines, and shows +2.1 %.

GPU busy time per range on the native backend, medians of the runs (`perf_pass_*`, [above](#gpu-time-per-pass)), in
milliseconds; "span %" is the span of the top-level ranges as a share of the frame's GPU time:

| Run | game | upload | shadows | hdr | taa | bloom | tonemap | ui | blit | sum | span | span % |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| hello | 0.341 | | | | | | | 0.041 | 0.204 | 0.587 | 0.548 | 100.0 |
| demo materials | 3.895 | 0.006 | 0.346 | 2.067 | 0.858 | 0.464 | 0.267 | 0.074 | 0.112 | 4.123 | 4.085 | 99.5 |
| demo shadows | 4.115 | 0.007 | 0.239 | 1.520 | 0.888 | 0.499 | 0.275 | 0.078 | 0.118 | 4.363 | 4.337 | 99.8 |
| demo sky | 4.959 | 0.011 | 0.449 | 2.087 | 0.872 | 0.509 | 0.277 | 0.071 | 0.123 | 5.228 | 5.220 | 99.8 |
| demo post | 4.562 | 0.009 | 0.382 | 2.064 | 0.933 | 0.502 | 0.275 | 0.073 | 0.120 | 4.809 | 4.929 | 99.8 |
| demo hdr | 2.610 | 0.006 | 0.253 | 0.874 | 0.820 | 0.470 | 0.268 | 0.061 | 0.112 | 2.797 | 2.768 | 99.7 |
| demo hdr_on | 2.779 | 0.006 | 0.268 | 0.879 | 0.741 | 0.428 | 0.274 | 0.522 | 0.101 | 3.690 | 3.039 | 99.7 |
| demo physics | 2.702 | 0.006 | 0.234 | 0.811 | 0.682 | 0.380 | 0.212 | 0.062 | 0.094 | 2.886 | 2.856 | 99.7 |
| demo assets | 3.229 | 0.007 | 0.224 | 1.510 | 0.834 | 0.409 | 0.216 | 0.068 | 0.098 | 3.429 | 3.386 | 99.8 |
| demo audio | 4.128 | 0.006 | 0.234 | 1.578 | 0.878 | 0.498 | 0.274 | 0.077 | 0.120 | 4.396 | 4.362 | 99.8 |
| demo animation | 4.683 | 0.010 | 0.583 | 2.562 | 1.535 | 0.458 | 0.268 | 0.061 | 0.112 | 4.845 | 5.229 | 99.7 |

Plausibility: the top-level ranges cover 99.5 to 100 % of the frame's GPU time in every run, so hardly any GPU work
of the frame runs outside them; the sum of the busy times is within 0.4 ms of the span
except in `hdr_on` (the UI in the HDR composition takes 0.52 ms there; which passes overlap was not examined). The
renderer's ranges add up to slightly more than `game` (materials: 4.02 against 3.90 ms) because consecutive passes
overlap. The main HDR pass is the largest everywhere (0.8 to 2.6 ms), then TAA (0.7 to 0.9 ms; 1.5 ms in the
animation section, cause not examined) and bloom (0.4 to 0.5 ms). The animation section's skinning compute is in its own command buffer and not in this table.

Stage 8 with the shadow fix, 2026-11-06 05:01-05:32, Release, vsync, Mac, Apple M4 Pro, `--frame-size 1920x1080`,
`--audio-device null`, interleaved: `5bace7f` (`/tmp/qs8head`), stage 8 alone (`2117093`, from `git archive` in
`/tmp/qs8stage8`) and the merged tree (stage 8 with the cut-out and translucent shadows). The display paced at about
120 Hz (hello 1159 to 1201 frames in 10 s, the demo 415 to 601 in 5 s) with the demo's frame p95 near 25 ms, as in the
session below. Medians of the runs, `5bace7f` / stage 8 / merged, in milliseconds, and the merged tree against
`5bace7f`:

| Run | Runs | CPU p50 | CPU p99 | GPU p50 |
|---|---|---|---|---|
| hello | 5 + 5 + 5 | 0.262 / 0.288 / 0.287 (+9.5 %) | 0.451 / 0.485 / 0.480 (+6.4 %) | 0.551 / 0.550 / 0.550 |
| hello (again, order rotated) | 8 + 8 + 8 | 0.273 / 0.287 / 0.285 (+4.6 %) | 0.463 / 0.498 / 0.480 (+3.6 %) | 0.550 / 0.552 / 0.551 |
| demo materials | 3 + 3 + 3 | 0.901 / 0.867 / 0.987 (+9.5 %) | 1.294 / 1.244 / 1.390 (+7.4 %) | 4.916 / 4.884 / 5.361 |
| demo materials (focused, first region) | 5 + 5 + 5 | 0.921 / 0.920 / 0.987 (+7.2 %) | 1.390 / 1.290 / 1.401 (+0.8 %) | 5.207 / 4.950 / 5.481 |
| demo materials (focused, second region) | 5 + 5 + 5 | 0.909 / 0.950 / 0.991 (+9.0 %) | 1.235 / 1.251 / 1.381 (+11.8 %) | 5.464 / 6.007 / 5.297 |
| demo shadows | 3 + 3 + 3 | 0.839 / 0.853 / 0.871 (+3.8 %) | 1.149 / 1.252 / 1.207 (+5.0 %) | 3.676 / 4.952 / 4.752 |
| demo shadows (focused, two regions) | 5 + 5 + 5 | 0.957 / 0.949 / 0.971 (+1.5 %), 0.965 / 0.949 / 0.963 (-0.2 %) | 1.239 / 1.252 / 1.257 (+1.5 %), 1.203 / 1.231 / 1.202 (-0.1 %) | 3.677 / 3.390 / 3.552, 3.377 / 4.329 / 3.781 |
| demo sky | 3 + 3 + 3 | 0.820 / 0.837 / 0.814 (-0.7 %) | 1.094 / 1.116 / 1.122 (+2.6 %) | 5.527 / 4.294 / 5.322 |
| demo post | 3 + 3 + 3 | 0.857 / 0.899 / 0.914 (+6.7 %) | 1.220 / 1.211 / 1.221 (+0.1 %) | 3.217 / 4.863 / 5.613 |
| demo post (focused) | 5 + 5 + 5 | 0.887 / 0.865 / 0.891 (+0.5 %) | 1.158 / 1.194 / 1.199 (+3.5 %) | 5.475 / 5.049 / 5.418 |
| demo hdr | 3 + 3 + 3 | 0.834 / 0.763 / 0.825 (-1.1 %) | 1.254 / 1.060 / 1.238 (-1.3 %) | 2.301 / 2.785 / 2.797 |
| demo hdr (focused) | 5 + 5 + 5 | 0.897 / 0.828 / 0.847 (-5.6 %) | 1.210 / 1.157 / 1.135 (-6.2 %) | 2.754 / 2.938 / 4.182 |
| demo hdr_on | 3 + 3 + 3 | 0.857 / 0.835 / 0.878 (+2.5 %) | 1.259 / 1.201 / 1.224 (-2.8 %) | 2.954 / 2.950 / 2.999 |
| demo physics | 3 + 3 + 3 | 0.895 / 1.028 / 0.984 (+9.9 %) | 1.470 / 1.449 / 1.483 (+0.9 %) | 2.894 / 3.445 / 2.887 |
| demo physics (focused) | 5 + 5 + 5 | 1.007 / 0.956 / 1.006 (-0.1 %) | 1.402 / 1.360 / 1.396 (-0.4 %) | 4.902 / 3.884 / 4.900 |
| demo assets | 3 + 3 + 3 | 0.916 / 0.921 / 0.958 (+4.6 %) | 1.267 / 1.348 / 1.300 (+2.6 %) | 4.469 / 4.278 / 4.876 |
| demo audio | 3 + 3 + 3 | 0.885 / 0.916 / 0.918 (+3.7 %) | 1.234 / 1.221 / 1.234 (0.0 %) | 4.404 / 5.862 / 5.869 |
| demo animation (against stage 8) | 3 + 3 | 2.659 / 2.669 (+0.4 %) | 3.696 / 3.647 (-1.3 %) | 4.944 / 5.087 |

The complete runs alternated `5bace7f`, stage 8, merged in that order; the focused runs (one section per process,
05:14-05:32) rotated the order and started after one warm-up run. Every region more than 5 % worse in the complete
runs was measured again. Only materials stayed above 5 %: in every focused run of the merged tree its CPU p50 (0.972
to 1.032) is above every run of `5bace7f` (0.619 to 0.948), and stage 8 alone is within the noise of `5bace7f`. The
cost is the translucent shadows' three transmittance passes (one render pass per cascade, one draw each): in one
process of the merged tree, alternating the transmittance map and the dithered fallback (`set translucent_shadows`,
12 + 12 regions of 3 s, 05:27-05:28), CPU p50 1.129 against 1.030 ms (+9.6 %), CPU p99 1.488 against 1.353 ms, GPU
p50 5.93 against 5.57 ms; the map was dearer in 11 of the 12 pairs. About 0.1 ms per frame, about 33 µs per pass, not
broken down further; only scenes with translucent casters pay it, and the section stays within its 2.0 ms budget.
hello does not use `quench_render`: the merged tree equals stage 8 alone there (-0.5 % and -3.7 % in the 8 + 8 + 8
runs). Stage 8 alone read +5.1 % and +7.6 % against `5bace7f` in those runs (+0.4 % and -5.8 % in the session
below); the runs overlap (CPU p50 0.252 to 0.327 against 0.257 to 0.305), and the only change in hello's frame since
`5bace7f` is one more branch in the GPU layer's uniform push. The GPU p50 of one build swings between about 3.2 and
5.6 ms between runs, with the GPU clock under the irregular pacing; the benchmark does not time the shadow passes
alone.

Stage 8 A/B, 2026-11-06 04:18-04:37, Release, vsync, Mac, Apple M4 Pro, `--frame-size 1920x1080`,
`--audio-device null`: HEAD `5bace7f` (built from `git archive` in `/tmp/qs8head`) against the stage 8 working tree,
interleaved. In this session the display paced at about 120 Hz (hello 1129 to 1195 frames in 10 s, the demo 482 to
601 in 5 s), not the 60 Hz of the sessions below, and the demo's frame p95 was about 25 ms in most regions of both
builds (missed intervals; cause not checked); Xcode was open. Medians of the runs, HEAD and stage 8, in
milliseconds:

| Run | Runs | CPU p50 | CPU p99 | GPU p50 |
|---|---|---|---|---|
| hello | 5 + 5 | 0.267, 0.268 (+0.4 %) | 0.486, 0.458 (-5.8 %) | 0.549, 0.551 |
| demo materials (focused) | 5 + 5 | 0.912, 0.885 (-3.0 %) | 1.280, 1.256 (-1.9 %) | 5.684, 4.998 |
| demo shadows (focused) | 5 + 5 | 0.855, 0.839 (-1.9 %) | 1.195, 1.149 (-3.8 %) | 5.254, 3.332 |
| demo sky (focused) | 5 + 5 | 0.813, 0.773 (-4.9 %) | 1.106, 1.090 (-1.4 %) | 4.577, 4.084 |
| demo post (focused) | 5 + 5 | 0.875, 0.865 (-1.1 %) | 1.172, 1.259 (+7.4 %) | 5.297, 4.337 |
| demo post alone, first region | 6 + 6 | 0.826, 0.851 (+3.0 %) | 1.177, 1.201 (+2.0 %) | 3.485, 3.229 |
| demo post alone, second region | 6 + 6 | 0.835, 0.8575 (+2.7 %) | 1.1895, 1.1825 (-0.6 %) | 3.359, 3.263 |
| demo hdr_on (focused) | 5 + 5 | 0.827, 0.819 (-1.0 %) | 1.209, 1.214 (+0.4 %) | 3.083, 2.942 |
| demo physics (focused) | 5 + 5 | 1.010, 0.972 (-3.8 %) | 1.414, 1.422 (+0.6 %) | 2.863, 2.902 |

The complete demo benchmark, three runs per build (04:22-04:26), gave CPU p50 between -8.2 % (hdr) and +14.7 %
(hdr_on) per section and CPU p99 between -6.9 % (hdr) and +6.6 % (sky); HEAD's first run read lower than its other
two in most sections (materials 0.272 against 0.909 and 0.871). Every section above 5 % in either was measured
again on its own (the focused rows); post's p99 read +7.4 % there and +2.0 % and -0.6 % in 6 + 6 runs of post
alone. assets read +2.1 % and -2.9 %, audio +3.0 % and -0.6 % in the complete runs. The GPU p50 of one build swings
by up to 2 ms between runs in this session (shadows: 3.3 to 5.8 ms), with the GPU clock under the irregular pacing;
a frame without skinned instances records no skinning pass, so these sections' GPU work did not change.

The new `animation` region, three complete runs: 531 to 593 frames, CPU p50 2.663 / 2.797 / 2.757, CPU p99
3.613 / 3.768 / 3.741, GPU p50 5.495 / 4.808 / 4.968. Three runs of the section alone with crowds of 256, 512 and 0
(04:33-04:34) and its perf fields ([Engine demo](demo.md#benchmark): the update's mean and largest time over the
last 240 frames on 12 threads, 11 workers and the frame thread; the skinning pass's GPU time):

| Crowd (characters) | CPU p50 | CPU p99 | GPU p50 | Update mean | Update max | Skinning GPU |
|---|---|---|---|---|---|---|
| 256 (259) | 2.727 / 2.625 / 2.636 | 3.829 / 3.622 / 3.725 | 5.027 / 4.853 / 4.868 | 0.752 / 0.663 / 0.654 | 3.749 / 1.948 / 1.098 | 1.552 / 1.979 / 1.147 |
| 512 (515) | 2.474 / 2.567 / 2.587 | 3.716 / 3.756 / 3.808 | 4.957 / 4.864 / 5.092 | 0.495 / 0.512 / 0.533 | 1.236 / 1.208 / 1.505 | 1.279 / 1.346 / 1.539 |
| 0 (3) | 1.066 / 1.177 / 1.184 | 1.416 / 1.645 / 1.575 | 3.528 / 5.183 / 5.203 | 0.095 / 0.108 / 0.102 | 0.216 / 0.273 / 0.258 | 0.687 / 0.362 / 0.437 |

All values in milliseconds. The crowd of 256 adds about 1.5 ms of CPU p50 to the section with three characters:
about 0.6 ms in `AnimationSystem::update`, the rest in the frame's other work for 256 more skinned instances (each
its own batch and dispatch, 655 shadow draws instead of 21, the palette copies), not broken down further. The crowd
of 512 read faster than 256 in all three runs, in the update and in the frame; not explained (the CPU clock rising
with the load is a guess, not checked). The skinning time is the GPU time of the pass's own command buffer (palette
upload and dispatches) and has a fixed part: 0.36 to 0.69 ms for three characters.

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
