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
perf: hello: 600 frames; frame ms avg 16.667 p10 16.243 p50 16.657 p95 17.248 p99 17.433 max 17.830; cpu ms avg 0.246 p50 0.242 p95 0.314 p99 0.355; gpu ms avg 0.545 min 0.537 p10 0.540 p50 0.544 p95 0.549 (600 samples); thermal nominal
```

- **frame**: wall time between frames. With vsync it is the display interval; it shows missed frames, not cost.
- **cpu**: the frame's CPU work up to the submit, without the wait for a swapchain texture.
- **gpu**: the GPU time of the frame's command buffer (`gpu::Caps::gpuTiming`: Metal only, through SDL's internal
  fence on SDL GPU, directly on the native Metal backend), one sample per frame. Its minimum and 10th percentile are
  the frames least disturbed by other work on the machine.
- **thermal** (since 2026-11-06): the operating system's thermal state at the start of the region, `-> end` where
  it changed and `(max ...)` where it rose higher in between (`app::ThermalState`, read every 0.5 s; Apple platforms:
  `NSProcessInfo`'s `thermalState`, unknown elsewhere). From fair on the system lowers clocks: every GPU range grows
  by the same factor without a change in the work, so numbers taken in different states do not compare. The app logs
  each change (`app: thermal state nominal -> fair`); the demo shows the state in its Performance panel and About,
  and `benchmark.json` has it per section (`thermal_start`, `thermal_end`, `thermal_max`).
- **thermal reaction** (since 2026-11-07, only while a game turned it on): `; thermal reaction serious` with
  `-> end` and `(max ...)` as above and the limits at the end of the region (`(at most 60 Hz, render scale x0.80
  now)`, [below](#thermal-state)). A region with a reaction measures the reduced frame, not the game's settings;
  the demo's `benchmark.json` has `thermal_reaction_start`, `_end` and `_max` per section (`off` when it was off).

On the native Metal backend (`--gpu-backend metal`, since stage 9) a second line follows with the GPU time per pass
([below](#gpu-time-per-pass)).

Run it in Release, with vsync (the default), at a fixed render size, in a window on the screen on the Mac:

```sh
cmake --build --preset release
build/release/examples/hello/hello --onscreen --frame-size 1920x1080 --script examples/hello/scenarios/benchmark.txt \
    --audio-device null
```

`--onscreen` (since 2026-11-06): scripted runs are offscreen by default ([App shell](app.md#offscreen-runs)), where
frames are not presented, so a perf region there warns and measures no display pacing; the benchmark needs the
window. Offscreen regions run unpaced and keep their GPU times (also GPU-bound ones since 2026-11-06; before, a
frame that waited for the oldest frame in flight replaced it unread, and such regions had no GPU sample): enough
for an A/B of costs. Since the same day each frame gives one GPU sample, to the region it was in (before, every
frame repeated the last time read, which could be a frame of the previous region or a frame read twice). Offscreen,
three frames are in flight and a frame's GPU time runs from its command buffer's start to its end, so these spans
overlap: in a GPU-bound region the GPU p50 can exceed the frame interval (11.5 against 10.8 ms in the tunnel's
A/B). Compare GPU times with GPU times, and take the busy times per range as the costs. It is the one automated run
that shows a window: start it by hand, not from a test.
`--audio-device null` (since stage 7): miniaudio's null backend pulls the mix on its own thread like a real output,
without sound; the stage 7 runs below used it.

On the iPhone: `ios-xcode` build, copy the scenario into the app's user directory with `devicectl device copy to`,
launch with `--script benchmark.txt` and `--console` ([App shell](app.md#ios-runs)); the device renders at its
native size.

Why vsync: a window is paced to its display even with `--present-mode immediate` (60 Hz on the Mac's LG UltraFine 5K),
and then the measured GPU time grows to about 15.5 ms, probably because the drawable wait is counted inside the
command buffer (not verified). With vsync the GPU time matches the work (0.55 ms). On 2026-11-06 the same Mac ran
`immediate` unpaced (4286 frames in 10 s, GPU time 0.62 ms on average; cause not known): vsync stays the benchmark's
mode, its frame rate is the display's either way. The native backend was paced at 120 Hz with `immediate` the same
day (481 frames in 4 s) and unpaced at 60 Hz in another session; both backends set the layer's `displaySyncEnabled`
to false for `immediate` on macOS. A hypothesis, not checked: the window server holds the drawables to the display's
refresh when it composites the window (which display, full screen or not, the window's position), so `immediate` is
unpaced only sometimes. Each `perf:` line now says what the frames waited for: `wait ms p50 .. p95 ..` (the wait
for the swapchain texture, left out of the CPU time; offscreen, for the oldest frame in flight), the present mode in
use and the display's rate (`perf_wait_p50`, `perf_present_mode`, `perf_display_hz`). A paced run shows a wait of
about the display's interval less the CPU time (vsync at 60 Hz: 16.13 to 16.15 ms with a CPU p50 of 0.41 ms). On
2026-11-07 both backends ran `immediate` unpaced on the same display (one run each, [below](#on-the-screen-after-the-bug-packages)):
the frames then wait about 2.7 to 2.8 ms (p50) for a drawable, which holds the rate at 358 to 381 frames per second
while the GPU needs 0.55 ms. No paced `immediate` run has been seen since the wait is reported, so the hypothesis is still
not checked.

## Comparing runs

Timings drift between runs: compare before and after in the same session, three runs each, and compare the medians
of the runs. At today's sizes the CPU time of one run swings by up to two times between runs (CPU median 0.119 to
0.257 ms on the Mac, below), so the 5% rule cannot resolve CPU changes yet; the GPU median is stable to 0.001 ms in
normally paced runs (one run paced like a 120 Hz display read 0.587 ms, last table). Say in the devlog which numbers
a claim rests on.

### CPU time and the cores

The frame's CPU time (`cpu ms`) depends on where the scheduler runs the frame thread: on an efficiency core, or on a
performance core at a low clock, the same work takes several times longer. Since 2026-11-06 (after stage 12) the
`perf:` line adds the process's CPU time per frame (all threads) and the share of it on performance cores
(`perf_process_cpu`, `perf_cpu_performance_share`; Apple platforms, from `TASK_POWER_INFO_V2`). The ray tracing
section offscreen on the M4 Pro (native backend, 1920 x 1080, everything traced, MetalFX denoising at 0.67, the
same region twice; `proc_pid_rusage` read alongside in a temporary build): 8.6 and 8.5 million instructions per
frame in both, but 12% on performance cores with CPU p50 1.202 ms, then 97% at 3.44 GHz with CPU p50 0.293 ms.
Other regions of the same run read 4 to 50% with CPU p50 0.63 to 1.21 ms, and 90% at 1.55 GHz with 0.594 ms. The
earlier anomalies (CPU p50 0.45 ms in the MetalFX denoising row on the Mac, 0.247 ms against about 1.9 ms on the
iPhone 17 in the tunnel's MetalFX temporal row) fit this: frames that ran on performance cores, not less work
(the iPhone's not checked with the share yet). Compare CPU times only at a similar share; the scheduler, not the
engine, chooses it (the frame thread is the main thread, user-interactive).

## GPU time per pass

With `gpu::Caps::gpuPassTiming` (the native Metal backend; SDL GPU has no timestamp queries, so it reports false and
logs nothing of this) the frame's command buffer is timed per range of passes ([Architecture](architecture.md#gpu-timestamps-per-pass)):
the app's `game` (around `Game::render`), `ui` and `blit`, and inside `game` the renderer's `upload` (its per-frame
buffers), `accel` (acceleration structure builds, refits and compactions; inside it `blas`, the bottom levels, and
`tlas`, the top level), `prepass` (the depth prepass for the
rays, with the G-buffer, the motion vectors and the denoising upscaler's inputs where those are needed), `raytrace`
(with `rays` (sun, occlusion and local light shadows), `denoise`, `probes`, `reflections` and `indirect` inside; the
last two include their own filters), `path` (the path tracer), `cloud_noise` (once), `weather` (the weather and
cloud shadow maps), `clouds` (the volumetric march and its accumulation) and `sky_cube` (where clouds are drawn;
[Clouds](#clouds)), `shadows` (all cascades), `local_shadows` (the local
lights' shadow maps, where they are not traced), `hdr` (the main pass with
the sky), `taa`, `upscale` (MetalFX), `bloom` and `tonemap`; the ray tracing, MetalFX and cloud ranges only where
those run. A range's time is its busy
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

## Profiling with Tracy

[Tracy](https://github.com/wolfpld/tracy) (0.15.0, [Dependencies](dependencies.md#added-for-profiling)) records
zones (named spans of time on a thread), GPU zones, plots and messages while a Tracy server is connected to the
program. A build with `QUENCH_TRACY` (off by default; the `profile` preset is Release with it on) links Tracy's
client into `quench_core`. Without it nothing of Tracy is compiled or linked, and the profiling macros compile to
nothing without evaluating their arguments. The client records on demand (`TRACY_ON_DEMAND`): it listens on
localhost only (`TRACY_ONLY_LOCALHOST` on desktops: port 8086 or the first free one of the 19 after it, or the port
in `TRACY_PORT`) and records nothing until a server connects. Its cost while nobody records and while it records:
[below](#tracy-on-and-off).

```sh
cmake --preset profile && cmake --build --preset profile
# A scenario recorded offscreen, without the Tracy GUI: build/profile/tracy/raytracing.tracy and _zones.csv
python3 tools/tracy_capture.py --output build/profile/tracy/raytracing -- \
    build/profile/examples/demo/quench-demo --gpu-backend metal --frame-size 1920x1080 --audio-device null \
    --script examples/demo/scenarios/raytracing_profile.txt
# The statistics of a saved capture again
python3 tools/tracy_capture.py --stats build/profile/tracy/raytracing.tracy
```

`tools/tracy_capture.py` picks a free port, starts the program with `TRACY_PORT` set to it, records with
`tracy-capture` and exports with `tracy-csvexport` (Tracy's command line tools, built by the `profile` preset into
its build directory; `QUENCH_TRACY_TOOLS`, on with `QUENCH_TRACY` in host builds). `<output>_zones.csv` has a row per
CPU zone, GPU zone and plot, for the whole capture (`all`) and for every perf region of the script (found by the
`perf_begin <label>` and `perf_end <label>` messages the app sends): count, count and time per frame, mean, p50, p99
and max of single zones, total and self time (without child zones). The script prints the zones with the most time
per region; `--seconds N` stops recording after N seconds, `--expect NAME` and `--expect-gpu NAME` fail the run
without that zone (the `hello_tracy_capture` test). The program must leave by itself: `--script` with `quit`, or
`--quit-after`. Two limits of Tracy's: recording starts when the server connects, so the program's first moments
are missing (the script retries `tracy-capture` until the program listens: started earlier it fails at once on
macOS); and its statistics leave out zones shorter than one tick of its timer (about 42 ns on Apple silicon), so
the counts of tiny zones are low.

What a capture has:

- **Frames**: a frame mark per app frame; plots `app.frame_ms`, `app.cpu_ms`, `gpu.frame_ms` (the command buffer's
  GPU time, every backend with `gpu::Caps::gpuTiming`, also SDL GPU), `audio.mix_ms` and `audio.load` (the audio
  statistics the game thread reads every 0.5 s).
- **The app loop** (thread `main`): `app.frame`, `app.gpu_time`, `app.scenario`, `app.fixed_step` (each tick),
  `app.update`, `app.ui`, `app.render` with `app.wait_gpu` (offscreen: the wait for the oldest frame in flight),
  `app.acquire_swapchain`, `app.submit` and `app.capture`, and `render.frame` (`Renderer::render`).
- **Timed ranges**: a CPU zone `pass.<range>` around each range of [GPU time per pass](#gpu-time-per-pass)
  (`pass.game`, `pass.raytrace`, ...), on every backend: while a server is connected the app times the ranges every
  frame. On the native Metal backend the same ranges are also GPU zones, on a GPU timeline named after the device,
  made from the engine's own timestamps: each interval in which a range kept the GPU busy is a zone, so a range's
  time per frame is its busy time in the perf lines (the ray tracing section at 1920 x 1080: `raytrace` 4.781 ms
  per frame in the capture, 4.804 ms p50 in the perf line). Where passes overlap (an Apple GPU starts a pass's
  vertex stage while the previous one shades) a range has several zones per frame. The GPU timeline begins at the
  timestamps' calibration, about 1 s after the start. SDL GPU has no timestamp queries: only the plot there.
- **Jobs** (threads `jobs.worker N`): a zone per job, named where the job has a name (`JobSystem::run(name, ...)`,
  `parallelFor(name, ...)`; others are `job`): `assets.load`, `audio.stream_open`, `audio.stream_fill`,
  `animation.characters`, `navigation.tile`, `navigation.prepare`, `physics.jobs` (Jolt's jobs).
- **Modules**: `physics.step` with Jolt's own zones inside (`jolt.<name>`, through Jolt's external profile hooks),
  `animation.update`, `assets.update` with `assets.finish` per finished asset, `assets.read`, `assets.decode`,
  `assets.transcode_basis` and `assets.decode_bc7` (the asset's path as the zone's text), `gpu.upload_buffer`,
  `gpu.upload_texture`, `audio.update`, `audio.decode` (a stream's refill), `audio.mix_offline`,
  `navigation.update` (a navmesh's finished tiles) and `navigation.crowd` (a crowd's step).
- **Messages**: every log line while a server is connected.

Not recorded: the audio device's callback. It must not allocate, lock or wait, and Tracy's zone path may allocate
(a thread's first event, a new block of its event queue); its mix time comes through the `audio.mix_ms` plot.
Allocations are recorded only with `-DQUENCH_TRACY_MEMORY=ON` (with `QUENCH_TRACY`; off by default): it replaces
the global `operator new` and `operator delete` (not the aligned ones) with versions that report to Tracy while a
server is connected, and every allocation then pays for that check.

Games use the same macros (`#include <quench/core/Profile.h>`, all of them nothing without `QUENCH_TRACY`):

```cpp
QUENCH_PROFILE_ZONE("game.ai");             // a zone to the end of the scope; the name a string literal
QUENCH_PROFILE_ZONE_DYNAMIC(name);          // a zone with a name made at run time (copied while recording)
QUENCH_PROFILE_ZONE_TEXT(path);             // text on the current zone
QUENCH_PROFILE_PLOT("game.enemies", count); // a value over time; the name a string literal
QUENCH_PROFILE_MESSAGE(text);               // a message on the timeline
QUENCH_PROFILE_THREAD_NAME("loader");       // the calling thread's name
```

`QUENCH_PROFILE_FRAME()` marks the end of a frame; the app does it. `quench::profile::connected()` tells whether a
server records (for work that only feeds a profile).

**Opening a capture** (by the user; agents do not start the GUI, it opens a window): Tracy's profiler GUI of version
0.15.0 or newer opens a `.tracy` file (its welcome window, "Open saved trace"); an older one refuses it (Homebrew had
0.14.1 on 2026-11-07). It builds from the source the configure fetched:
`cmake -S build/profile/_deps/tracy-src/profiler -B build/tracy-profiler -DCMAKE_BUILD_TYPE=Release`, then
`cmake --build build/tracy-profiler` (upstream's CMake, which downloads its own libraries). Connected to a running
profile build instead of `tracy-capture`, it shows the frames live (the program in its list, or `127.0.0.1` and the
port). On 2026-11-08 the GUI was built this way for the user, who connected it to `127.0.0.1` (the list did not
show the demo) and profiled the profile build's demo in its Weather section in a window on the Mac: "everything
works great"; the user's screenshot showed the frames, the CPU and GPU plots and the native backend's GPU zones.

On iOS the client builds with `TRACY_NO_BROADCAST` (no discovery broadcast on the phone's network) and listens on
all addresses; a server would connect to the phone's address on the local network. Not run on a phone yet, and the
app would need iOS's local network permission (`NSLocalNetworkUsageDescription`, not in the engine's Info.plist).
Off in WebAssembly builds (the option is ignored there: Tracy's client needs threads and TCP sockets).

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

The iPhone baseline of 2026-11-06 (below; every section, both backends) stays within the iPhone budget in every
section but one: the ray tracing section on the native backend reads 8.07 ms GPU p50 (RT shadows with occlusion),
over the 7.5 ms. It is left over budget, as a known item for the ray tracing work; it still makes the 120 Hz
interval. The phone's GPU p50 does not go below about 6 ms (below), so the budget catches growth above that, not
below. Since stage 11 the section runs at 60 Hz on phones that trace, with lighter defaults
([Stage 11 on the iPhone](#stage-11-on-the-iphone)): its budget there is the 60 Hz interval, not the table's. The
iPhone 17 (A19, 2026-11-06, below) has no budget of its own: it holds 120 Hz in every other section, with CPU p50
about 1.5 times the iPhone 18 Pro Max's (up to 1.8 ms, within the 2.0 ms).

The demo's `animation` region (a crowd of 256, 259 characters; since stage 8) is a load test and has its own Mac
budget, at about twice the medians of the runs of 2026-11-06 04:22-04:34 below: CPU p50 5.5 ms, CPU p99 7.5 ms,
GPU p50 15 ms as for the other sections. It is above the sections' budget of 2.0 and 3.5 ms (they have a few dozen
instances). No iPhone budget for it yet: the crowd of 256 has not run on the phone. The section with its default
crowd (51 skinned characters, `--benchmark-all` on 2026-11-06, below) stays within the sections' iPhone budget there
(CPU p50 1.47 ms, CPU p99 1.77 ms).

## Thermal state

A phone under sustained GPU load lowers its clocks: on the iPhone 17 the ray tracing section's phone defaults took
twice their GPU time after the first 20 s and still held 60 Hz for five minutes, and the tunnel's phone defaults
missed intervals after about 8 minutes, a minute before the state read serious
([Ten minutes on the iPhone](#ten-minutes-on-the-iphone)). The engine reports the state and changes nothing by
itself; a game may opt in to a reaction (`App::thermalGovernor()`, [App shell](app.md#thermal-state)):

| State | Default limits (`ThermalGovernorDesc`) |
|---|---|
| nominal, unknown | none |
| fair | none |
| serious | at most 60 Hz, render scale x0.8 |
| critical | at most 30 Hz, render scale x0.6, effects reduced (the game's choice which) |

A higher state applies at once; the limits are lifted after 60 s at a lower state. The frame rate limit is never above
the player's (`App::setMaxFrameRate`), and the render scale is a factor on the player's. The defaults are not tuned
on a hot phone yet: in the iPhone 17 run above the misses came at fair, before serious, so with them the reaction
would come about a minute after the first missed intervals; a game that wants it sooner sets limits at fair. The
demo's Adapt to heat (Settings, off) uses them as they are, and its check mode has the run that would tell
(`tunnel.heat`: the tunnel's phone defaults for 10 minutes with it on; [Engine demo](demo.md#modes)).

## Measurements

### Clouds

2026-11-07 20:55-20:58, the Mac (Apple M4 Pro), offscreen, the native backend, `--window 1920x1080` and
`--window 1206x2622` (the iPhone 17's native size in portrait), `--audio-device null`,
`examples/demo/scenarios/weather_cost.txt`: the weather section in fair weather and in a storm with the weather
moving at 10 times its speed, 3 s per setting after a second, unpaced; the GPU busy time per range, p50, ms (the
sum of `weather`, `clouds` and `sky_cube` and the `hdr` range's growth over clouds off, whose `hdr` was 0.464 and
0.738 ms). The frame's `gpu ms` is not used: offscreen and unpaced, frames overlap on the GPU and it counts the
waits.

| Setting | 1920 x 1080: weather / clouds / sky_cube | added | 1206 x 2622: weather / clouds / sky_cube | added |
|---|---|---|---|---|
| 2D layer | 0.223 / - / 0.068 | 0.38 | 0.225 / - / 0.068 | 0.38 |
| Volumetric, half size, 64 steps, 2 cube faces | 0.223 / 1.472 / 0.280 | 1.97 | 0.179 / 1.566 / 0.292 | 2.01 |
| Volumetric, full size, 64 steps, 2 cube faces | 0.187 / 3.789 / 0.314 | 4.29 | 0.226 / 4.598 / 0.317 | 5.14 |
| Storm, half size | 0.223 / 1.072 / 0.187 | 1.48 | 0.222 / 1.049 / 0.193 | 1.46 |
| Storm, full size | 0.221 / 2.803 / 0.210 | 3.23 | 0.222 / 3.113 / 0.213 | 3.55 |
| Phone defaults: half size, 48 steps, 1 cube face | 0.223 / 1.108 / 0.147 | 1.48 | 0.224 / 1.066 / 0.153 | 1.44 |

Before two changes made in the same session (one run, 20:52), the weather range was 0.36 ms (the weather and shadow
maps drawn every frame; drawn in turns since) and the sky cube 0.41 ms at two faces per frame (32 steps with the
detail erosion; 24 steps without it since); the march the same. The Mac target of the roadmap, the volumetric
clouds within about 2 ms at half size at 1080p, holds (1.97). On the iPhone 17 the stage 11 effects cost about 3
times the M4 Pro's: by that ratio the phone defaults would take about 4.3 ms and the 2D layer about 1.1 ms there, over
the roadmap's 2 and 0.5 ms; an estimate, not a measurement, so the section's phone defaults (half size, 48 steps,
one cube face, 60 Hz) wait for a run on the phone. Not run on the screen.

Measured again on 2026-11-08 17:24-17:27 at 1080p, before and after the clouds' accumulation was changed against the
hatching in motion (the march's noise read at a new place every frame, the history read through a Catmull-Rom filter,
this frame through a 3x3 tent; [Weather](weather.md#clouds)), the two binaries alternated three times and the lower
p50 kept: the clouds range 1.471 and 1.506 ms at half size, 3.791 and 3.897 at full size, 1.070 and 1.094 and 2.795
and 2.895 in the storm, 1.107 and 1.134 with the phone defaults: 2 to 4 % more, the history's nine reads in place
of one. The weather and sky cube ranges did not change.

### Rain

2026-11-07 23:08-23:10, the Mac (Apple M4 Pro), offscreen, the native backend, `--window 1920x1080` and
`--window 1206x2622`, `--audio-device null`, `examples/demo/scenarios/rain_cost.txt`: the weather section's Rain here
preset from the ground and its storm preset from the air, half-size clouds, 3 s per setting after a second, unpaced;
the GPU busy time per range, p50, ms: `rain_map`, `rain_shafts`, `rain` and the `hdr` range's growth over rain off
(the wet surfaces), "added" their sum.

| Setting | 1920 x 1080: map / shafts / drops / hdr | added | 1206 x 2622: map / shafts / drops / hdr | added |
|---|---|---|---|---|
| Ground, Low | 0.020 / 0.177 / 0.084 / 0.070 | 0.35 | 0.021 / 0.255 / 0.118 / 0.107 | 0.50 |
| Ground, Medium | 0.023 / 0.437 / 0.138 / 0.084 | 0.68 | 0.023 / 0.592 / 0.173 / 0.111 | 0.90 |
| Ground, High | 0.021 / 0.604 / 0.456 / 0.087 | 1.17 | 0.022 / 0.813 / 0.491 / 0.132 | 1.46 |
| Phone defaults: Low without shafts | 0.020 / - / 0.085 / 0.068 | 0.17 | 0.021 / - / 0.116 / 0.109 | 0.25 |
| Storm from the air, Low | 0.020 / 0.225 / 0.085 / 0.043 | 0.37 | 0.020 / 0.338 / 0.128 / 0.054 | 0.54 |
| Storm from the air, Medium | 0.020 / 0.825 / 0.140 / 0.054 | 1.04 | 0.021 / 1.232 / 0.190 / 0.055 | 1.50 |
| Storm from the air, High | 0.019 / 1.197 / 0.446 / 0.072 | 1.73 | 0.020 / 1.837 / 0.481 / 0.078 | 2.42 |

In the first runs of the session (22:55) the shafts were marched at the render size: 0.66, 1.04 and 1.61 ms from
the ground and 1.64, 2.67 and 4.40 ms from the air at 1080p. Marched at a quarter (Low) or half (Medium, High) of the
size with a depth-aware upsample they cost 0.18 to 1.2 ms there; the upsample is most of Low's. By the stage 11
ratio of about 3 the phone defaults would take about 0.7 ms on the iPhone 17, within the roadmap's 1 ms for the
rain, and Low with the shafts about 1.5 ms: an estimate, not a measurement. Not run on the screen or on a phone.

Measured again on 2026-11-08 00:57 (another session), with part 3's splashes (1536 and 4096 at Medium and High
instead of 1024 and 3072) and the snow off: ground 0.38, 0.75 and 1.30 ms at 1080p (the drops 0.086, 0.148, 0.488;
the shafts 0.191, 0.486, 0.685), the phone defaults 0.20; the storm from the air 0.41, 1.10 and 1.88; at
1206 x 2622 0.53, 0.98, 1.60, the phone defaults 0.25, from the air 0.58, 1.64, 2.65
([Weather](weather.md#cost)). The drops cost 0.01 to 0.03 ms more; the shafts and the HDR pass 3 to 8 % more than
the evening before, not compared in one session. Since then the streaks, splashes and flakes are not drawn where
none of their kind falls near the camera (the drops' range was 0.04 to 0.46 ms in a clear sky with the rain on).

Measured again on 2026-11-08 17:08-17:15, before and after the changes of that afternoon (the streaks a sixtieth of
a second of the drops' path, thinner and lit by the overcast sky; the splashes smaller and shorter; the shafts'
march with its curtains' noise faded with the step, the clouds' shadow on short steps only and a new noise offset
every frame, then smoothed at its reduced size; 17 and 28 steps at Medium and High instead of 20 and 32), the two
binaries alternated twice and the lower p50 kept, since other GPU work ran on the Mac during the session. The
shafts from the ground 0.187, 0.481 and 0.677 ms before, 0.222, 0.533 and 0.742 after at 1080p (0.281, 0.686,
0.937 and 0.312, 0.726, 1.002 at 1206 x 2622); from the air 0.242, 0.867, 1.293 and 0.258, 0.803, 1.251 (0.361,
1.353, 2.030 and 0.407, 1.338, 2.078): up to 11 % more from the ground, about the same from the air. The smoothing
pass is about 0.02 ms of it at Low and 0.05 ms at Medium and High at 1080p. The drops (0.084, 0.145 and 0.496
before, 0.085, 0.142 and 0.452 after), the map and the phone defaults (no shafts) did not change.

### Snow

2026-11-08 01:03, the same way (`examples/demo/scenarios/snow_cost.txt`: the Snowfall preset from the ground and the
Snow showers preset from the sky view, the weather's clock stopped, the rain off but for the phone defaults; GPU
busy time p50, ms: `rain_map`, `rain_shafts`, `rain` (the copy of the image the flakes are drawn over), `snow` (the
flakes) and the `hdr` range's growth over snow off).

| Setting | 1920 x 1080: map / shafts / copy / flakes / hdr | added | 1206 x 2622: map / shafts / copy / flakes / hdr | added |
|---|---|---|---|---|
| Ground, Low | 0.020 / 0.198 / 0.035 / 0.035 / 0.059 | 0.35 | 0.020 / 0.265 / 0.063 / 0.039 / 0.098 | 0.48 |
| Ground, Medium | 0.022 / 0.577 / 0.036 / 0.066 / 0.062 | 0.76 | 0.021 / 0.712 / 0.062 / 0.071 / 0.098 | 0.96 |
| Ground, High | 0.021 / 0.782 / 0.036 / 0.112 / 0.065 | 1.02 | 0.021 / 0.978 / 0.062 / 0.119 / 0.105 | 1.28 |
| Phone defaults: rain and snow Low without shafts | 0.020 / - / 0.037 / 0.036 / 0.136 | 0.23 | 0.020 / - / 0.066 / 0.040 / 0.215 | 0.34 |
| Snow showers from the sky view, Low | 0.020 / 0.193 / 0.042 / 0.035 / 0.042 | 0.33 | 0.020 / 0.300 / 0.072 / 0.040 / 0.080 | 0.51 |
| Snow showers from the sky view, Medium | 0.020 / 0.537 / 0.042 / 0.065 / 0.042 | 0.71 | 0.021 / 0.936 / 0.071 / 0.073 / 0.080 | 1.18 |
| Snow showers from the sky view, High | 0.020 / 0.758 / 0.041 / 0.111 / 0.043 | 0.97 | 0.021 / 1.360 / 0.071 / 0.123 / 0.081 | 1.66 |

The snow's shafts start where the flakes end (6 m at Medium) instead of 20 m, so under a snowfall they march more of
the near air than the rain's. Their first version took 2.207 ms (Medium) and 3.714 ms (High) from the ground at
1080p: the snow share at each step was a function that took the weather field's uniform block (`WeatherUniforms`,
its 16 cells) by value, and the compiled shaders copied the whole block at every step; reading the block in place
took the shafts to 0.577 and 0.785 ms with the same image. By the stage 11 ratio of about 3 the phone defaults would
take about 1 ms on the iPhone 17: an estimate. Not run on the screen or on a phone.

With the shafts' changes of 2026-11-08 afternoon ([Rain](#rain), measured the same way at 17:11-17:21): from the
ground 0.195, 0.572 and 0.774 ms before, 0.236, 0.641 and 0.870 after at 1080p (0.260, 0.701, 0.960 and 0.317,
0.807, 1.095 at 1206 x 2622, three runs each); the snow showers from the air 0.189 and 0.536 before, 0.226 and
0.595 after at Low and Medium at 1080p (High 0.834 after; its run before was disturbed), 0.295, 0.925, 1.341 and
0.349, 1.025, 1.479 at 1206 x 2622: 10 to 15 % more, the smoothing pass most of it. The flakes, the copy and the
phone defaults (no shafts) did not change.

### Wind

2026-11-08 02:30, the same way (`examples/demo/scenarios/wind_cost.txt`: the weather section's meadow in the Windy
storm preset, 79 swaying instances, the weather's clock stopped, the rain and the snow at Medium; GPU busy time p50,
ms).

| Range | 1920 x 1080 | 1206 x 2622 |
|---|---|---|
| `hdr`, the plants still (no wind) | 1.52 | 1.42 |
| `hdr`, the plants swaying in the gale | 1.52 | 1.46 |
| `rain` (the drops), steady wind / with gusts and eddies | 0.18 / 0.38 | 0.22 / 0.51 |
| `snow` (the flakes), steady wind / with gusts and eddies | 0.09 / 0.14 | 0.10 / 0.14 |
| `rain` at Low with gusts (the phone defaults) | 0.13 | 0.16 |

The sway costs nothing measurable; the gusts cost in the drops and flakes, each evaluating the field at its place.
The eddies' first noise (three value noises per instance) took the drops with gusts to 0.61 ms at 1080p, one hash
split into three values to 0.44 (the run above: 0.38). The lightning's query takes 0.001 to 0.002 ms of CPU time. The
meadow's first build, one instance per tuft (2131), took the frame's CPU median over the rain cost's nine regions to
1.17 ms against 0.97 without the meadow; as 64 patches of grass 0.95 ms. Not run on the screen or on a phone.

With the streaks of 2026-11-08 afternoon, whose tails read the wind field an exposure earlier ([Rain](#rain);
17:32-17:35, before and after alternated twice, the lower p50 kept, 1080p): the drops 0.174 and 0.167 ms in the
steady wind, 0.306 and 0.330 with gusts and eddies, 0.129 and 0.125 at the phone defaults; the flakes and the sway
unchanged; the `hdr` range 0.02 to 0.07 ms more (1.502 and 1.518 with the plants still, 1.655 and 1.724 in the
gusting rain), of which only the clouds' shadow's fade at its map's edge changed in that pass (two runs, with other
GPU work on the Mac: not settled).

### Particles

2026-11-08 15:55, offscreen on the native Metal backend (`examples/demo/scenarios/particles_cost.txt`: the demo's
Particles section at dusk, 11 systems in 23488 slots with about 7000 particles alive, 20 trails of about 8500 strip
vertices together; 3 s per setting after 1 s to settle; GPU busy time p50, ms).

| Range | 1920 x 1080 | 1206 x 2622 |
|---|---|---|
| `particles_sim` (the compute step), everything | 0.119 | 0.125 |
| `particles` (the draws), everything on the GPU / on the CPU | 0.389 / 0.343 | 0.350 / 0.311 |
| `particles`, density 0.5 / 0.25 | 0.340 / 0.308 | 0.315 / 0.308 |
| `particles_sim` / `particles`, the streams alone (6 systems, 20 trails) | 0.069 / 0.191 | 0.071 / 0.218 |
| `particles_sim` / `particles`, the sparkle field alone (8192 slots) | 0.024 / 0.121 | 0.043 / 0.139 |
| `particles_sim` / `particles`, the campfire alone (3 systems) | 0.066 / 0.093 | 0.064 / 0.105 |
| `hdr` with and without the particles | 0.227 / 0.270 | 0.349 / 0.356 |

The cost follows the slots (every slot is stepped and runs the vertex shader) more than the pixels or the density.
The frame's CPU time p50 at 1080p: 0.48 ms without the particles, 0.47 with them on the GPU (p95 0.54 and 1.13),
0.87 with the CPU simulation (p95 2.15); 7 to 78 % of the process's time on the performance cores, so these vary
between runs. The bloom range moved between 1.1 and 1.7 ms in the same runs with and without particles (offscreen,
the GPU's clock is not held up by presenting). Not run on the screen or on a phone. [Particles](particles.md#cost).

### Tracy on and off

2026-11-07 18:58-19:19, the Mac (Apple M4 Pro), offscreen, the native backend (the default), `--frame-size 480x270
--audio-device null`, the demo's `benchmark.txt` (every section, 5 s each, unpaced) and hello's `benchmark.txt`
(10 s, unpaced, about 20,000 frames per second); runs of the builds alternated, the medians of the runs' CPU p50
and p99, ms. Off: Release before the change (`c981cc6`) against Release with it (Tracy off), three runs of the demo
and eight of hello. On: Release against the `profile` preset (Tracy compiled in), without a server and recorded by
`tracy-capture` (`tools/tracy_capture.py`), two runs each.

| Build | Sum of the demo sections' CPU p50 / p99 | hello CPU p50 / p99 |
|---|---|---|
| Release before the change | 2.842 / 5.974 | 0.022 / 0.0595 |
| Release, Tracy off | 2.839 / 5.920 (-0.1 % / -0.9 %) | 0.022 / 0.0625 (0 / +5.0 %) |
| Release (the second session) | 2.801 / 6.528 | 0.022 / 0.059 |
| `profile`, no server | 2.814 / 5.777 (+0.5 % / -11.5 %) | 0.022 / 0.059 (0 / 0) |
| `profile`, recorded | 2.814 / 7.144 (+0.5 % / +9.4 %) | 0.023 / 0.068 (+4.5 % / +14 %) |

**Off**, every section's CPU p50 is within -3.6 % to +4.6 % of the build before (the ray tracing section +4.6 %:
0.456 to 0.477, its runs 0.41 to 0.52 in both builds). A section's p99 moves by up to 2 times between runs of the
same binary, so the per-section p99 says nothing at this size. hello's p99 median is 3 µs higher (0.057 to 0.078
in the runs before, 0.046 to 0.067 after; the mean of the runs' p99 0.0615 before, 0.0603 after); its frame has
no code of the change but the macros, which compile to nothing, so this is the spread of the runs. The demo's
stripped program is 208 bytes larger (the job names; [Dependencies](dependencies.md#added-for-profiling)).
**Compiled in, no server**: each zone costs a check of the connection, a load of one flag; the sections' p50 within
-8 % to +13.5 % (the ray tracing section 0.474 and 0.490 against 0.414 and 0.435, inside the spread of the runs
above), their sum +0.5 %. **Recorded**: the sections' p50 +0 to +2.4 % (the sum +0.5 %), the first section more
(+19.5 %: the server connects during it, 8375 frames in the first run against 11,684); hello +4.5 % p50 and +14 %
p99, and one of its two recorded runs made 149,110 frames instead of about 200,000: at 20,000 frames per second
Tracy's thread has about 20 events a frame to send (zones, GPU zones, plots). The recorded demo benchmark made a capture of 91 MB
(162,000 frames, 65 s), hello's 17 MB (10 s); `tracy-csvexport` took about two minutes over the demo's.

One run of hello before the change and one recorded run showed `gpu ms avg` of 3,110 and 3,045 ms with p95 under
0.06 ms: one command buffer's GPU time read as 7.2 days on the native backend, about the host clock since the Mac
started, a fault older than Tracy; the frame times and the ranges' busy times are not affected. Fixed since
(2026-11-07): a command buffer's status can read completed before Metal sets its start time, so the backends now
wait for the completed buffer (which returns at once) before reading its times, and a time with a start of 0 or an
end not after the start is no sample (`gpu_device.gpu_timing_frames_in_flight`).

### The ray tracing section by frame size

2026-11-07 18:53-18:57, the demo's ray tracing section on the Mac (Apple M4 Pro), offscreen on the native backend,
Release (Tracy off), `--audio-device null`, one run per size, 5 s per region (`scenarios/raytracing_profile.txt`;
the render scales in a scenario of the same form, not committed, 4 s per region). The open question: the user saw
57 ms of GPU time per frame in that section (the 0.10.0 package, the `raytrace` range 43.9 ms, `rays` 15 ms,
`denoise` 4.8 ms) at render scale 1.0 with 8 shadow rays, 4 occlusion rays, traced reflections, per-pixel indirect
light, the temporal denoiser and MetalFX temporal, likely in a large window on a Retina display. Mac defaults: render
scale 0.67 with MetalFX temporal, ray-traced sun shadows (2 rays), occlusion (2 rays), the 2 lamps' shadows and
reflections, the probe grid, the temporal denoiser. GPU busy time p50 per range, ms (the perf lines; the Tracy
capture at 1920 x 1080 reads the same within 0.03 ms):

| Output (rendered) | Frame p50 | `game` | `raytrace` | `rays` | `denoise` | `probes` | `reflections` | `hdr` | `upscale` | `accel` |
|---|---|---|---|---|---|---|---|---|---|---|
| 1280 x 720 (858 x 482) | 4.28 | 4.65 | 3.09 | 1.20 | 0.39 | 1.19 | 0.31 | 0.40 | 0.65 | 0.35 |
| 1920 x 1080 (1286 x 724) | 7.17 | 7.75 | 4.80 | 2.33 | 0.78 | 1.20 | 0.52 | 0.78 | 1.29 | 0.55 |
| 2560 x 1440 (1715 x 965) | 11.40 | 12.12 | 7.29 | 3.97 | 1.36 | 1.20 | 0.75 | 1.30 | 2.32 | 0.59 |
| 3456 x 2160 (2316 x 1447) | 21.57 | 22.26 | 13.29 | 7.86 | 2.76 | 1.20 | 1.46 | 2.52 | 4.76 | 0.58 |

The user's settings (`heavy` in the scenario: render scale 1.0, the rest as above), ms:

| Output | Frame p50 | GPU p50 | `raytrace` | `rays` | `indirect` | `denoise` | `probes` | `reflections` | `upscale` |
|---|---|---|---|---|---|---|---|---|---|
| 1280 x 720 | 9.24 | 9.65 | 8.06 | 3.65 | 1.95 | 0.77 | 1.19 | 0.50 | 0.79 |
| 1920 x 1080 | 18.54 | 19.34 | 15.93 | 7.81 | 4.32 | 1.68 | 1.19 | 0.95 | 1.79 |
| 2560 x 1440 | 31.94 | 33.44 | 27.36 | 13.70 | 7.85 | 3.02 | 1.20 | 1.60 | 3.27 |
| 3456 x 2160 | 64.66 | 67.82 | 55.49 | 27.88 | 16.75 | 6.49 | 1.20 | 3.13 | 6.63 |

**Where the time goes.** Everything but the probe grid (1.2 ms at every size: its 2048 probes do not depend on the
frame) and the acceleration structures (0.35 to 0.6 ms) grows with the pixel count. The traced passes and the HDR
pass grow with the rendered pixels (`raytrace` without the probes about 3.6 ms per million rendered pixels at the
defaults from 1080p on, more at 720p); MetalFX's temporal upscaler, bloom and tone mapping with the output pixels
(`upscale` 0.63 ms per million output pixels at every size). No range grows faster than its pixels. The user's
settings cost 8.7 to 9.0 ms of frame time per million pixels from 1080p on, over 40 % of it in the sun and
occlusion rays and a quarter in the per-pixel indirect light. The user's 57 ms fall between 2560 x 1440 (33.4 ms of
GPU time) and 3456 x 2160 (67.8 ms), at about 6.3 million pixels by the GPU time per pixel here: a large window on a
Retina display. Their split differs: at their `denoise` time (about 5.6 million pixels here) `rays` would take
about 21 ms against their 15, and the rest of `raytrace` (indirect light, reflections, probes) about 16 ms against
their 24; the view (the sun turns, the character walks) changes the split, the total follows the pixel count. The
section's defaults are fixed while the window is not.

**Render scales** (Mac defaults otherwise; frame p50 / `game` busy / `upscale` busy, ms):

| Output | 0.67 temporal (default) | 0.5 temporal | 0.4 temporal | 0.5 spatial | 0.67 spatial |
|---|---|---|---|---|---|
| 1920 x 1080 | 7.17 / 7.75 / 1.29 | 5.26 / 5.78 / 1.14 | 4.22 / 4.70 / 0.91 | 4.38 / 4.99 / 0.27 | 6.11 / 6.74 / 0.27 |
| 2560 x 1440 | 11.40 / 12.12 / 2.32 | 7.94 / 8.51 / 1.95 | 6.11 / 6.71 / 1.61 | 6.52 / 7.15 / 0.49 | 9.62 / 10.08 / 0.49 |
| 3456 x 2160 | 21.57 / 22.26 / 4.76 | 14.46 / 14.98 / 3.99 | 10.76 / 11.29 / 3.26 | 11.47 / 12.00 / 1.01 | 17.82 / 18.27 / 1.03 |

The spatial columns' busy times include TAA (0.08 to 0.49 ms), which runs before MetalFX's spatial upscaler. Their
command buffers' GPU time reads 2.8 to 2.9 times the frame time (`gpu ms` p50 12.2 against a frame of 4.4 ms at 1920 x
1080, 0.5): not possible for a GPU-bound frame, so that number is wrong with MetalFX spatial on the native backend
(cause not checked); the frame time and the ranges' busy times agree with each other.

**Proposal (not applied: the section's defaults are unchanged).** Scale the Mac defaults' render scale with the
output's pixel count, so that the rendered pixels stay near the 0.93 million of 1920 x 1080 at 0.67: scale =
clamp(sqrt(0.93 million / output pixels), 0.5, 0.67), 0.5 being the panel's lowest. By the table: 0.67 up to
1920 x 1080 (unchanged), 0.5 at 2560 x 1440 (7.9 instead of 11.4 ms: within 120 Hz's 8.33 ms, just) and at
3456 x 2160 (14.5 instead of 21.6 ms: 60 Hz instead of 46 frames per second). Chosen when the section starts and when
the window's size changes, and only while the player has not set a scale (the settings are saved). Beyond that, the
upscaler's own cost grows with the output (4.0 ms of the 14.5 at 3456 x 2160); MetalFX spatial would save 3 ms there
(11.5 ms), and a lower bound of 0.4 would give 10.8 ms. Ray counts by pixel count were not measured: at the
defaults `rays` is about a third of the frame, while the render scale acts on all of it but the probe grid, the
acceleration structures and the passes at the output's size. The user's 8 and 4 rays with per-pixel indirect light
at scale 1.0 do not hold 60 Hz even at 1920 x 1080 (18.5 ms). Not looked at: what 0.5 or spatial upscaling look
like in this scene (a check by eye on the Mac's display), and whether a game wants this in the engine (a target
rendered pixel count in `RenderSettings`) or in its own settings, as the section would do it.

### The denoiser in a still scene

2026-11-07 15:39-16:19, the per-pixel indirect light's temporal filter before and after its change of that day
([Render](render.md#denoising)), on the Mac (Apple M4 Pro), offscreen on the native backend, `--frame-size
1920x1080 --audio-device null`, Release; before: the code of `aa75d46` with the demo's `spin` option added (the
scenarios need it), after: the commit "Keep a per-pixel deviation in the indirect light's temporal filter". The
cause of a user report from the iPhone 17 (2026-11-07 15:30: with Indirect
light "Ray traced" at the section's phone defaults the unlit objects and the shadows are grainy all the time, also
when nothing moves).

**Noise** (`scenarios/raytracing_noise.txt`, one run each): luma (0 to 255) of the captures below the horizon (rows
250 to 1079), without the pixels within 3 of an edge (edges move with TAA's jitter): flicker, the mean difference
of two frames in a row; grain, the median difference of a frame from its 5 x 5 mean; residual, the mean difference
of the captures 300 frames apart (120 apart where the region ends at 300: the noise left, which moves slowly).
At the phone's 60 Hz, 60 frames are 1 s.

| Configuration (frames) | Before: flicker / grain / residual | After |
|---|---|---|
| phone defaults with per-pixel indirect light (600; 300 -> 600) | 0.84 / 0.42 / 1.37 | 0.26 / 0.25 / 0.43 |
| the same at full size, bilinear (300; 120 -> 300) | 0.66 / 0.38 / 1.27 | 0.05 / 0.14 / 0.26 |
| half size, bilinear | 0.59 / 0.25 / 1.20 | 0.06 / 0.15 / 0.26 |
| half size, MetalFX temporal | 1.10 / 0.27 / 1.54 | 0.03 / 0.16 / 0.23 |
| half size, MetalFX denoising (its own denoiser, unchanged) | 0.05 / 0.09 / 0.21 | 0.05 / 0.09 / 0.21 |
| phone defaults, the probe grid alone (unchanged) | 0.25 / 0.20 / 0.32 | 0.25 / 0.20 / 0.32 |
| phone defaults with per-pixel indirect light, the spatial denoiser | 2.53 / 0.52 / 3.04 | 1.58 / 0.37 / 1.77 |

Before, the phone defaults' flicker stayed at 0.88, 0.85, 0.85 and 0.84 after 60, 120, 300 and 600 frames: the
light never settled. After: 0.27, 0.26, 0.27, 0.26, the level of the probe grid alone, whose floor at half size
is MetalFX spatial over TAA's jitter. The mean luma of the scene went from 190.3 to 192.7 (the probe grid's 194.2):
the old filter's mean was darker. How the cause was found, at the phone defaults (flicker / grain / residual): no
clamp and no anti-lag in the indirect light's filter alone, 0.34 / 0.32 / 0.64 (in every filter: the same within
0.02); the deviation floor instead, 0.34 / 0.33 / 0.63 (the clamp and anti-lag kept, the noise as without them);
with 64 frames of history 0.28 / 0.30 / 0.52; with a third a-trous pass 0.22 / 0.23 / 0.38 at 64 frames and
0.26 / 0.25 / 0.43 at 32. 32 frames were kept: as clean as the probe grid within 60 frames, and they follow a
sudden change faster (below: mean 0.94 against 1.25 32 frames after the sun's jump, 0.63 against 0.90 after 64).
The deviation floor on the sun and lamp shadows too changed nothing in a still scene and let the moving sun shadow
trail more (99th percentile error at the wall's shadow 19.8 against 12.3 in motion), so they keep their filter.

**In motion** (`scenarios/raytracing_noise_motion.txt`): the frame after 2 s of motion against the same instant
settled for 600 frames, luma: mean difference 2.27 before, 1.57 after (99th percentile 16.4 and 16.2); away from
the moving sun-shadow edges 2.00 and 1.24; at those edges (62251 pixels where either trails) -8.35 and -9.26 (the
shadow behind its new place). In a run with the pan stepped every frame (not committed) the probe grid alone read
-10.74 there against -10.62 before and -11.82 after: a clean image trails at a moving edge as much (TAA keeps more of
its history where the noise does not cut it), the new filter adds about one level. **After the sun jumps by 60
degrees**, mean / 99th percentile against the settled frame, 1, 2, 4, 8, 16, 32 and 64 frames after: before 2.36 /
15, 2.19 / 13, 1.99 / 11, 1.78 / 9, 1.61 / 8, 1.54 / 8, 1.51 / 8; after 2.38 / 17, 2.25 / 16, 2.05 / 14, 1.75 /
12, 1.36 / 9, 0.94 / 6, 0.63 / 3. The old filter followed the change sooner in the first frames because its random
resets also made it noisy; the new one is behind there by a few levels at the sunlit sides the floor's rays see,
and better from 16 frames on.

**Cost** (`perf.txt` regions of 4 s, unpaced, the phone defaults with and without per-pixel indirect light, three
runs of each build alternated, 16:17-16:19, another agent's builds on the machine; render size 960 x 540), GPU busy
time p50, ms, medians over the runs: the indirect light's range 1.052 before, 1.169 after (relative to the probe
grid's range in the same region, which did not change: 0.893 and 0.981, +0.10 ms); the frame (`game`) with
per-pixel indirect light 5.67 and 5.83 (+2.8 %); without it 3.39 and 3.40 (that filter does not run; the sun and
lamp filters are the same code). On the iPhone 17 the phone defaults' frame took 7.67 ms ([below](#stage-11-on-the-iphone),
the display rate), 2.3 times the Mac's: by that ratio the change costs about 0.25 to 0.4 ms there, and the phone
defaults with per-pixel indirect light take about 13 ms (18 ms by the 3.1 times of everything at the device's
size): not measured on the phone, and near the 16.7 ms of 60 Hz by the second estimate.

### The native default on the screen and on the iPhone 17

2026-11-07 14:49-15:12, the code of `c42b2c5` (the native Metal backend as the default on Apple platforms), Release,
no backend flag unless named.

**hello on the Mac's screen** (14:49-14:50, Apple M4 Pro, the display at 60 Hz, `--onscreen --frame-size 1920x1080
--audio-device null`, vsync, thermal nominal), the backends alternated; started once the 1-minute load average,
raised by another agent's builds, had fallen below 4 (3.77 at the start, 3.00 at the end):

| Run | Backend | Frames | Frame p50 / p99 | CPU p50 / p99 | On performance cores | GPU p50 / p95 |
|---|---|---|---|---|---|---|
| 1 | native (default) | 600 | 16.621 / 17.799 | 0.230 / 0.496 | 44 % | 0.549 / 0.575 |
| 2 | SDL GPU | 601 | 16.637 / 17.737 | 0.235 / 0.507 | 37 % | 0.548 / 0.557 |
| 3 | native (default) | 600 | 16.583 / 17.835 | 0.366 / 0.554 | 10 % | 0.549 / 0.556 |
| 4 | SDL GPU | 601 | 16.607 / 17.827 | 0.361 / 0.517 | 7 % | 0.549 / 0.557 |

The same GPU time on both backends, as in the earlier on-screen runs (0.550 ms on both at 120 Hz on 2026-11-06,
0.551 to 0.552 ms on SDL GPU at 60 Hz, [below](#on-the-screen-after-the-bug-packages)). The CPU p50 of the two
backends is within 2 % in each pair and follows the share on performance cores, not the backend (0.23 ms at 37 to
44 %, 0.36 ms at 7 to 10 %); means of the two runs: p50 0.298 ms on both, p99 0.525 against 0.512 ms (+2.5 %). Two
runs per backend, not the three of [Comparing runs](#comparing-runs). The native backend's ranges: `game` 0.342,
`ui` 0.041, `blit` 0.203 ms. Within the Mac budget.

**The iPhone 17** (A19, iOS 27.2, 1206 x 2622 in portrait, 120 Hz, `--audio-device null`, launched through
`devicectl` on its cable, no validation layer in the demo; nobody looked at the screen during the runs; the device
tests in [Platforms](platforms.md#native-metal-backend)). The benchmark of every section (14:58-15:00, `mode
benchmark 5` in a copy of `benchmark_all.txt`, one run) against 2026-11-06 17:15-17:19 (`5aea212`, the 0.8.0 demo,
`--benchmark-all` with the native backend by flag; [The iPhone 17 baseline](#the-iphone-17-baseline-stage-9-and-10-code)):

| Section | Frames / 5 s | Frame p50 / p99 | CPU p50 / p99 | GPU p50 | 2026-11-06 CPU p50 / p99, GPU p50 |
|---|---|---|---|---|---|
| materials | 600 | 8.315 / 9.112 | 1.333 / 1.693 | 5.796 | 1.294 / 1.668, 5.825 |
| shadows | 601 | 8.334 / 8.428 | 1.124 / 1.476 | 5.710 | 1.134 / 1.484, 5.693 |
| sky | 601 | 8.333 / 8.956 | 1.097 / 1.397 | 5.748 | 1.036 / 1.444, 5.632 |
| post | 601 | 8.335 / 8.422 | 1.150 / 1.427 | 5.711 | 1.188 / 1.532, 5.683 |
| hdr | 601 | 8.334 / 8.448 | 1.130 / 1.398 | 5.698 | 1.126 / 1.444, 5.788 |
| physics | 601 | 8.334 / 8.418 | 1.175 / 1.462 | 5.700 | 1.166 / 1.484, 5.803 |
| input | 601 | 8.334 / 8.414 | 1.140 / 1.451 | 5.647 | 1.127 / 1.502, 5.761 |
| assets | 601 | 8.335 / 8.441 | 1.244 / 1.531 | 5.749 | 1.280 / 1.696, 5.738 |
| audio | 601 | 8.334 / 8.438 | 1.124 / 1.432 | 5.730 | 1.154 / 1.511, 5.795 |
| animation | 601 | 8.334 / 8.437 | 1.913 / 2.206 | 5.773 | 1.783 / 2.890, 5.786 |
| raytracing (phone defaults, 60 Hz) | 300 | 16.669 / 16.761 | 2.580 / 2.840 | 7.709 | full size then, not compared |
| tunnel (phone defaults, 60 Hz; fair) | 300 | 16.670 / 16.760 | 2.168 / 2.502 | 8.670 | not run then |

Thermal nominal in every section but the tunnel (fair from the ray tracing section's end). The two runs are two
stages of code apart on the same backend, so they show the default holding the phone's numbers, not a same-build
A/B; the animation section's CPU p50 +7.3 % (p99 lower) was not examined. `benchmark_all.txt` as it is (0.1 s per
section, the form of the report) passed too.

The cost of each effect (`raytracing_phone.txt`, 15:00-15:05, 5 passed, 0 failed) against 2026-11-06 18:42
([Stage 11 on the iPhone](#stage-11-on-the-iphone), the native backend by flag): every region's `game` within 3 %
but the first (`rt_shadows_ao` 10.971 against 11.604 ms, 376 frames in 4 s against 352), for example everything
traced 34.952 against 34.370 ms, MetalFX spatial at 0.5 13.458 against 13.432 ms, the denoising upscaler at 0.67
27.256 against 28.008 ms.

The tunnel (`tunnel_phone.txt`, 15:05-15:12, 11 passed, 1 failed) against 2026-11-06 21:43
([The tunnel in motion](#the-tunnel-in-motion)), GPU busy time in ms:

| Region | Thermal | Frames | Frame p50 / p99 | CPU p50 / p99 | game | 2026-11-06 frames, game |
|---|---|---|---|---|---|---|
| the lamps unshadowed, MetalFX spatial at 0.5 | nominal | 469 / 4 s | 8.331 / 14.729 | 1.275 / 1.527 | 7.242 | 481, 5.463 |
| two lamp shadows | nominal | 473 / 4 s | 8.328 / 13.888 | 1.763 / 2.019 | 7.969 | 457, 8.863 |
| four lamp shadows | nominal | 419 / 4 s | 8.344 / 17.755 | 1.684 / 1.976 | 9.603 | 399, 10.250 |
| everything traced | nominal | 171 / 4 s | 24.998 / 25.093 | 2.029 / 2.320 | 23.809 | 169, 23.961 |
| the defaults at 60 Hz, 0-20 s | nominal | 1159 / 20 s | 16.669 / 16.763 | 2.108 / 2.433 | 7.986 | 1159, 7.986 |
| the defaults at 60 Hz, 20-40 s | nominal | 1200 / 20 s | 16.668 / 16.753 | 1.756 / 2.009 | 15.167 | 1200, 15.165 |
| the defaults in motion, 40-60 s | nominal -> fair | 1200 / 20 s | 16.669 / 16.761 | 1.741 / 2.006 | 15.259 | 1200, 15.253 |

The minute at 60 Hz reads as on 2026-11-06, and every frame made its interval (max 16.820 ms), but the phone turned
fair 56 s into it, and the scenario's check of the state after the motion failed (nominal throughout on 2026-11-06).
This run came after about 15 minutes of other load on the phone and started at fair (2.5 minutes to nominal at 20
frames per second); not repeated from a cold phone. The first two regions at about the display's rate read the
clock, not costs (every range of the first 1.3 to 1.4 times the earlier run's); the slower ones are 0.6 to 6.3 %
below 2026-11-06's. The captures (the defaults still and in motion, the path tracer) were copied from the phone and
looked at.

### SDL GPU and the native backend, before it became the default

2026-11-07 13:41-14:03, Release, Mac (Apple M4 Pro), **offscreen** (`--audio-device null --script`, frames not
paced), thermal state nominal; another app's window rendered on the same GPU during the runs (about 20 % of a core).
The demo's `benchmark.txt` (a region per section) three times per backend and hello's `benchmark.txt` five times,
in a rotating order; medians over the runs. Three configurations: SDL GPU (`--gpu-backend sdl`), the native backend
(`--gpu-backend metal`; per-pass timestamps on, as in every perf region) and the native backend with the timestamps
off (a temporary switch, not committed). At 480 x 270 the frame thread is CPU-bound and runs 88 to 100 % on
performance cores, so its CPU times compare; at 1920 x 1080 the frames are GPU-bound and it runs 3 to 11 % on them
(more for the animation section), so only the frame counts compare there.

| 480 x 270 | SDL GPU frames, CPU p50, p99 | native | native, no timestamps |
|---|---|---|---|
| materials | 8993, 0.145, 0.493 | 8756, 0.156, 0.225 | 8769, 0.144, 0.181 |
| shadows | 12905, 0.125, 0.350 | 12884, 0.132, 0.178 | 12864, 0.124, 0.164 |
| sky | 12143, 0.123, 0.178 | 11660, 0.126, 0.164 | 11670, 0.117, 0.157 |
| post | 13324, 0.124, 0.208 | 13089, 0.132, 0.194 | 13192, 0.123, 0.165 |
| hdr | 14665, 0.117, 0.324 | 14852, 0.127, 0.208 | 15094, 0.118, 0.183 |
| hdr_on | 14057, 0.122, 0.177 | 13983, 0.132, 0.371 | 14524, 0.124, 0.198 |
| physics | 13770, 0.122, 0.343 | 13677, 0.131, 0.245 | 13693, 0.120, 0.213 |
| assets | 13409, 0.134, 0.209 | 13067, 0.142, 0.331 | 13272, 0.132, 0.251 |
| audio | 13541, 0.128, 0.338 | 12990, 0.136, 0.296 | 13244, 0.127, 0.171 |
| animation (crowd 256) | 4133, 0.673, 0.984 | 4000, 0.646, 1.204 | 4277, 0.598, 0.857 |
| raytracing (traced on native) | 10133, 0.182, 0.551 | 1851, 0.680, 1.083 | 1848, 0.502, 0.966 |
| tunnel (traced on native) | 10873, 0.145, 0.209 | 1546, 0.614, 1.138 | 1544, 0.463, 0.997 |

Frames in a 5 s region, CPU times of the frame thread in milliseconds. Without timestamps the native backend's CPU
p50 is within -11 to +2 % of SDL GPU's in the sections that do the same work (the animation section's -11 % the
largest), and its p99 lower in all of them but hdr_on (0.198 against 0.177) and assets (0.251 against 0.209);
single runs' p99 vary by up to 0.3 ms in the light sections and 0.8 ms in the animation section, on both backends.
The timestamps cost 8 to 12 microseconds of CPU time per frame (+2 to +8.5 % over SDL GPU's p50 in the light
sections) and, in the animation section, 6.5 % of the frames: Metal samples them at the stage boundaries of every
pass, and with them the GPU overlaps that section's many passes less; its frame thread then waits longer and runs
slower (CPU p50 0.646 ms, still below SDL GPU's 0.673; p99 1.204 against 0.984). They are on only in perf regions
and when a game asks for them (`App::requestPassTiming`, the demo's Performance panel); SDL GPU has no per-pass
times at all. At 1920 x 1080 the native backend ran 0.3 % more to 3.1 % fewer frames than SDL GPU in the same
sections (animation 1552 against 1602, the largest gap; hdr_on 3142 against 3134), within 1.5 % of its frames
without timestamps. hello at 1920 x 1080 (CPU-bound): 100757, 100472 and 100801 frames, CPU p50 0.022 ms on all
three, p99 0.033, 0.044 and 0.034 ms (single runs 0.031 to 0.062 on each), GPU p50 0.145 to 0.146 ms. The ray tracing section and the tunnel trace on the native backend (ray-traced
shadows, reflections, area light shadows) and show their fallbacks on SDL GPU: different work, not compared.

An earlier session the same day (13:00-13:24, the same runs) found the native backend's animation section at 3232
frames and CPU p50 0.882 ms against SDL GPU's 4156 and 0.647 (+36 %), with or without timestamps. The cause: GPU
buffers written with `cycle` were not cycled on the native backend, so each frame's skinning palette upload and
skinning pass waited for the previous frame's draws; the frame thread waited longer (wait p50 0.38 against
0.09 ms) and every part of its work ran slower, the draw lists' culling (no GPU calls) 33 % slower included. SDL GPU
with those two writes not cycled reproduced it (3855 frames, CPU p50 0.91 ms). The native backend cycles GPU
buffers since ([Architecture](architecture.md#native-metal-backend)); after it, 4925 to 4933 frames and CPU p50
0.662 to 0.684 ms against SDL GPU's 4965 to 4979 and 0.684 to 0.718 in that section's region alone. The memory
footprint in the animation section (256 characters, 1920 x 1080): 504 to 505 MB on the native backend, 761 to
763 MB on SDL GPU.

On the screen and on the iPhone 17 with the native backend as the default: [above](#the-native-default-on-the-screen-and-on-the-iphone-17).

### Animation LOD

2026-11-07 03:38-03:47, Release, Mac (Apple M4 Pro), the demo's Animation section with a crowd of 256 (260
characters), **offscreen** (`--frame-size 1920x1080 --audio-device null --script`, frames not paced: 1.5 to 2.4 ms
per frame, so the update times are lower than on the screen at vsync), perf regions of 5 s after a second each, from
the hero's view and from 45 m (`camera 2 22 45`); the commit before animation LOD (`01834f7`) built and run in the
same session between two runs of the new build. Means over the last 240 frames:

| Camera, LOD | Sampled per frame | Update, 12 threads | Update, 1 thread | Skinning GPU | CPU avg |
|---|---|---|---|---|---|
| hero's view, `01834f7` | 260 | 0.153 | 0.343 | 2.13 | 1.030 |
| hero's view, off | 260 | 0.159, 0.164 | 0.334 | 2.21, 2.25 | 1.041, 1.035 |
| hero's view, interpolated | 124 to 129 | 0.150, 0.152 | 0.329 | 2.18, 2.24 | 1.050, 1.036 |
| hero's view, held | 143 | 0.119, 0.132 | 0.240 | 1.13, 1.18 | 1.017, 1.029 |
| 45 m, `01834f7` | 260 | 0.152 | 0.334 | 1.72 | 0.911 |
| 45 m, off | 260 | 0.148, 0.153 | 0.309 | 1.75, 1.77 | 0.959, 0.953 |
| 45 m, interpolated | 65 | 0.129, 0.136 | 0.256 | 1.58, 1.61 | 0.959, 0.922 |
| 45 m, held | 65 | 0.088, 0.093 | 0.139 | 0.46, 0.47 | 0.817, 0.809 |

All in milliseconds; the 1-thread column is one run with `--worker-threads 0`. With LOD off the update is within
the runs' spread of the commit before. Interpolation saves little on this character (20 joints, a three-clip
blend): it still blends, poses and writes palettes every frame. Holding halves the update and cuts the skinning
pass to a quarter at 45 m, where 195 of the 260 skins are copied instead of skinned. Not measured on the screen or
on the iPhone.

### Dual quaternion skinning

2026-11-07 04:02-04:14, Release, Mac (Apple M4 Pro), the demo's Animation section with a crowd of 256 (262
characters with the skinning pair), **offscreen** as above, perf regions of 3 s after a second each, the hero's view.
CPU p50 of the frame, the skinning pass's GPU time (mean over 240 frames), in milliseconds:

| Build, skinning of the crowd | CPU p50 | Skinning GPU |
|---|---|---|
| `85012e6` (260 characters), 6 regions | 0.985 to 1.039 (mean 1.016) | 2.16 to 2.28 |
| this change, linear, 6 regions alternating with `85012e6` | 1.037 to 1.114 (mean 1.075) | 2.26 to 2.35 |
| this change, without the skinning pair (260), 4 regions | 1.006 to 1.069 (mean 1.050) | 2.22 to 2.31 |
| this change, linear, 4 regions in one run with the next row | 1.045 to 1.075 | 2.27 to 2.32 |
| this change, dual quaternion, 4 regions | 1.097 to 1.135 | 2.24 to 2.42 |

The default (linear) costs 0.06 ms more than the commit before, 5.8%: 0.026 ms of it are the demo's two new
characters (the same binary with and without them; they stand out of view but in the shadow cascades, so they are
skinned and drawn), the rest is within the spread of one binary's regions (about 0.06 ms) and the linear path has
the same code apart from one branch per instance. Dual quaternions cost 0.06 ms of the frame thread for 5240 joints
(about 11 ns each: the palette's matrices become dual quaternions on the CPU; 0.11 ms before the conversion skipped
the scale of unscaled joints) and nothing measurable on the GPU. Not measured on the screen or on the iPhone.

### Blend shapes

2026-11-07 05:20-05:30, Release, Mac (Apple M4 Pro), the demo's Animation section with a crowd of 256 (265
characters with the blob, which is not drawn in the hero's view; 264 skinned instances in both builds), **offscreen**
as above, perf regions of 3 s after a second each, the hero's view; the commit before blend shapes (`cc8e263`) built
in a second tree and run alternating with this change. CPU p50 of the frame, the skinning pass's GPU time (mean over
240 frames), in milliseconds:

| Build | CPU p50 | CPU p99 | Skinning GPU |
|---|---|---|---|
| `cc8e263`, 8 regions | 1.054 to 1.080 (mean 1.065) | 1.36 to 1.77 | 2.32 to 2.40 (mean 2.35) |
| blend shapes in `skin_cs` itself, 6 regions | 1.140 to 1.180 | 1.50 to 1.63 | 2.55 to 2.59 |
| this change (`skin_cs` and `skin_morph_cs`), 8 regions | 1.067 to 1.120 (mean 1.087) | 1.38 to 1.78 | 2.37 to 2.47 (mean 2.41) |

With the blend shapes inside the one skinning entry, the skinned meshes without them were 10% slower on the GPU
(the morph code and its buffer, though no weight was set), so the shader has two entries: `skin_cs` for skinned
meshes without weights (the code it had before) and `skin_morph_cs` for instances with weights and for meshes
without joints. What is left, +2% of CPU and +2.5% of skinning GPU time, is within a few hundredths of a millisecond
of the spread of one binary's regions. The cost of the blend shapes themselves (one dispatch of 1094 vertices with
four targets in the demo) was not measured. Not measured on the screen or on the iPhone.

Weights from clips (2026-11-07 05:48, the same scene and method, the blend shape commit `c2b1fee` against the change,
4 regions each; the crowd's characters have no channels, the blob's four are sampled in every update): CPU p50 1.074
to 1.151 before and 1.062 to 1.115 after, p99 1.47 to 1.73 and 1.39 to 1.64, `AnimationSystem::update` 0.151 to
0.168 and 0.157 to 0.162 ms, skinning GPU 2.35 to 2.47 and 2.34 to 2.41 ms: no difference beyond the regions'
spread.

### Normal maps in ray hits

2026-11-07 06:08-06:26, Release, Mac (Apple M4 Pro), the native Metal backend, **offscreen** (`--frame-size
1920x1080 --gpu-backend metal --audio-device null --script`, unpaced), full size with bilinear scaling, perf regions
of 3 s after a second each; the commit before (`c5bd5fd`) and this change run alternating. GPU busy time p50 of the
range, in milliseconds (two pairs; in a third pair one binary ran about 10% slower in every range for part of its
run, at the nominal thermal state, so it is left out):

| Region | Range | Before | After |
|---|---|---|---|
| ray tracing section (no normal maps), reflections | `reflections` | 0.901, 0.952 | 0.903, 0.903 |
| the same, probe grid | `probes` | 1.150, 1.243 | 1.150, 1.151 |
| the same, shadows, lamps, reflections and probes | `reflections`, `probes` | 0.933 / 1.152, 0.973 / 1.186 | 0.935 / 1.154, 0.937 / 1.163 |
| the same, path tracer | `path` | 12.70, 12.74 | 12.72, 12.71 |
| tunnel (four normal maps: panels, asphalt, plastic, rubber), reflections | `reflections` | 6.06, 6.29 | 6.39, 6.39 |
| the same, with the probe grid | `reflections`, `probes` | 7.89 / 1.18, 8.05 / 1.23 | 8.24 / 1.19, 8.26 / 1.20 |
| the same, path tracer | `path` | 29.29, 29.22 | 30.50, 30.45 |

A frame without normal maps costs what it did: it runs the tracing entries without them. One entry for both cost
the path tracer 0.35 to 0.5 ms (3 to 4 %) and the tunnel's reflections 0.2 ms more in frames whose hits read no
map (the tangent frame's registers, though the branch was not taken), and a separate `[branch]` for the map still
0.2 ms of the path tracer, so the tracing entries have `_maps` variants used only while an instance in the top
level has a normal map. In the tunnel the maps cost reflections 0.1 to 0.33 ms (2 to 5 %) and the path tracer
1.2 ms (4 %); its hit arrays hold 10 textures instead of 6 (ray memory 4.10 MB instead of 3.10). Not measured on
the screen or on the iPhone.

### Surface functions in ray hits

`quench_add_ray_hits()` ([Render module](render.md#ray-hits-of-surfaces)). 2026-11-07 16:33-16:37, Release, Mac
(Apple M4 Pro), the native Metal backend, **offscreen** at 1920 x 1080 as above, unpaced, full size with bilinear
scaling, 3 s regions after a second each; the commit before (`c35c8b1`) and this change alternating, five pairs
(two with every region, three with indirect light and the path tracer only). Other agents' builds and runs loaded
the Mac throughout (load average 40 to 85); two more pairs, in which one run was 15 to 60 % slower in its ranges,
are left out. GPU busy time p50 of the range, ms, the lowest and the median of the runs:

| Region | Range | Before | After |
|---|---|---|---|
| ray tracing section (the checker ground's hits run its function after), reflections | `reflections` | 0.925, 0.944 | 0.972, 0.983 |
| the same, probe grid | `probes` | 1.243, 1.249 | 1.268, 1.269 |
| the same, per-pixel indirect light | `indirect` | 3.976, 4.222 | 4.086, 4.366 |
| the same, path tracer | `path` | 11.74, 12.14 | 12.73, 12.85 |
| tunnel (no surface with hits), reflections | `reflections` | 3.815, 3.835 | 3.824, 3.829 |

A frame without such a surface runs the engine's own entries and costs what it did (the tunnel). With the checker's
hits the traced passes cost 2 to 5 % more and the path tracer 6 to 8 % (0.7 to 1.0 ms); in an earlier set of five
pairs (16:26-16:31, the same load), with the checker's box-filtered form in its hits (a filter width of 0 there,
since replaced by the plain square waves), the path tracer was unchanged (12.11 against 12.02 ms, the lowest) and
reflections and indirect light 0.06 and 0.3 ms more: about a tenth of a millisecond either way is within what the
compiler makes of a different function in the big tracing shaders. The instances' hit table grew from 144 to 256
bytes each. The first frame that traces such a surface compiles its pipelines (the path tracer's about 0.9 s the
first time on the Mac, 0.02 s from Metal's cache). Not measured on the screen or on the iPhone.

### Probe grid in turns

`RenderSettings::probeUpdatePeriod` ([Render module](render.md#ray-traced-lighting)). 2026-11-07 06:52-06:57,
Release, Mac (Apple M4 Pro), the native Metal backend, **offscreen** at 1920 x 1080 as above, unpaced, full size
with bilinear scaling, 3 s regions after a second each, thermal state nominal throughout; the commit before
(`69b1955`) and this change alternating, three pairs. GPU busy time p50 of the range, ms (the three runs):

| Region | Probe updates | `probes` | `raytrace` | `game` (the frame) |
|---|---|---|---|---|
| ray tracing section, probe grid (sun shadows, occlusion, probes) | before | 1.156, 1.156, 1.153 | 3.858, 3.860, 3.864 | 6.726, 6.708, 6.717 |
| | every frame | 1.162, 1.164, 1.163 | 3.871, 3.866, 3.873 | 6.701, 6.680, 6.693 |
| | 1/2 | 0.681, 0.680, 0.683 | 3.360, 3.361, 3.364 | 6.001, 5.997, 6.056 |
| | 1/4 | 0.426, 0.427, 0.429 | 2.896, 2.900, 2.901 | 5.453, 5.464, 5.488 |
| | 1/8 | 0.324, 0.324, 0.325 | 2.783, 2.787, 2.786 | 5.379, 5.338, 5.321 |
| tunnel, everything traced (4 lamp shadows, reflections, probes) | before | 1.190, 1.190, 1.188 | 14.951, 14.960, 14.956 | 19.637, 19.641, 19.641 |
| | every frame | 1.203, 1.203, 1.204 | 14.815, 14.841, 14.818 | 19.504, 19.531, 19.505 |
| | 1/2 | 0.671, 0.672, 0.671 | 14.287, 14.309, 14.326 | 18.958, 18.979, 18.990 |
| | 1/4 | 0.410, 0.410, 0.409 | 14.019, 14.048, 14.016 | 18.707, 18.734, 18.701 |
| | 1/8 | 0.330, 0.333, 0.331 | 13.978, 13.980, 13.958 | 18.665, 18.690, 18.651 |

The probe rays scale with the fraction; the irradiance and distance passes over all 2048 probes stay, about
0.2 ms here, so 1/8 saves 0.83 to 0.87 ms of the 1.16 to 1.20. Every frame, the `probes` range reads 0.008 and
0.014 ms (0.7 and 1.2 %) above the commit before: the irradiance pass reads the previous value before it knows the
probe's state, and the turn's bookkeeping (the distance pass reads the validity only after a frame in turns). The
frames are not slower: `game` 0.02 ms and, in the tunnel, 0.13 ms lower, the latter in `reflections` (8.09 against
8.23 ms, every pair), whose hits read the grid through `Probes.hlsli`; not examined further. The captures with every
probe per frame are the same as before (mean difference 0.0000).

How far the grid lags behind a change of light (test `renderer_probe_grid_updates_in_turns`, a tile lit only by
the grid at 1 m spacing, after the sun doubles or halves; one run, plus one with a count to within 10 % that is not
part of the test): halfway in 18, 38, 68 and 156 frames with 1, 2, 4 and 8 turns, within 10 % of the new light in
70, 166, 276 and 629 frames. At 60 Hz that is 1.2, 2.8, 4.6 and 10.5 s to within 10 %; at 120 Hz half of it.

On a phone (not run yet): the iPhone 17 measured the probe grid at 3.3 ms of `probes` at any render scale, and
4.2 ms more in the main pass at the native size from the per-pixel sampling, which turns do not change
([Stage 11 on the iPhone](#stage-11-on-the-iphone)). By the Mac's ratios 1/4 would take the probes to about 1.2 ms
and 1/2 to about 1.9 ms; that is an estimate. Proposed for the ray tracing section's phone defaults, not set: the
probe grid on in 4 turns at half size, at 60 Hz, if `scenarios/raytracing_phone_probes.txt` shows it fits the
16.7 ms frame with room (light then settles within 10 % in about 4.6 s); else 2 turns or off as now.

### Probe cascades, relocation and classification

`RenderSettings::probeCascades`, `probeRelocation`, `probeClassification` ([Render module](render.md#ray-traced-lighting)).
2026-11-07 17:23-17:32, Release, Mac (Apple M4 Pro), the native Metal backend, **offscreen** at 1920 x 1080,
unpaced, the sections' Mac defaults otherwise (MetalFX temporal at 0.67: 1286 x 724), 4 s regions after a second
each, two runs of each; other agents' builds and runs loaded the Mac (load average 10 to 47). GPU busy time p50 of
the range, ms (the two runs); the tunnel from its overview with the drive stopped (`reset_camera`), since what the
drive passes changes the other ranges:

| Region | `probes` | `hdr` | `reflections` | frame GPU p50 |
|---|---|---|---|---|
| ray tracing section, its grid (2 m, 1 cascade) | 1.195, 1.202 | 0.779, 0.778 | 0.518, 0.522 | 7.90, 8.32 |
| the same, classification | 0.714, 0.722 | 0.775, 0.779 | 0.501, 0.505 | 7.31, 7.75 |
| the same, relocation | 1.271, 1.272 | 0.808, 0.809 | 0.510, 0.512 | 7.95, 8.14 |
| the same, 2 cascades | 2.204, 2.210 | 0.891, 0.891 | 0.518, 0.515 | 8.92, 9.10 |
| the same, 3 cascades | 3.154, 3.160 | 0.894, 0.890 | 0.512, 0.513 | 9.84, 9.84 |
| 1 m, 3 cascades | 3.352, 3.339 | 0.964, 0.962 | 0.532, 0.536 | 10.19, 10.23 |
| 1 m, 3 cascades, relocation and classification (the demo's `raytracing_probes`) | 1.824, 1.821 | 0.998, 1.003 | 0.538, 0.542 | 8.70, 8.70 |
| 1 m, 2 cascades, relocation and classification | 1.365, 1.355 | 0.941, 0.939 | 0.549, 0.550 | 8.50, 8.31 |
| 2 m, 4 turns | 0.442, 0.442 | 0.776, 0.777 | 0.501, 0.503 | 7.07, 7.11 |
| 2 m, 4 turns, classification | 0.333, 0.332 | 0.777, 0.777 | 0.500, 0.492 | 6.98, 6.92 |
| 1 m, 2 cascades, relocation and classification, 4 turns | 0.667, 0.667 | 0.939, 0.939 | 0.517, 0.515 | 7.50, 7.49 |
| tunnel, its grid (2 m, 1 cascade) | 1.212, 1.213 | 1.852, 1.840 | 4.186, 4.060 | 12.85, 12.36 |
| the same, classification | 0.667, 0.669 | 1.892, 1.891 | 3.986, 3.939 | 11.71, 11.52 |
| the same, relocation | 1.269, 1.266 | 1.938, 1.938 | 4.184, 4.123 | 12.71, 12.43 |
| the same, relocation and classification | 0.656, 0.655 | 1.860, 1.859 | 4.075, 4.033 | 11.80, 11.61 |
| 1 m, 2 cascades | 2.350, 2.343 | 2.124, 2.114 | 4.925, 4.817 | 14.55, 14.30 |
| 1 m, 2 cascades, relocation and classification | 1.288, 1.293 | 2.106, 2.103 | 4.982, 4.978 | 13.45, 13.41 |

A cascade costs what a grid does (about 1 ms of probes each), and sampling more than one adds 0.1 to 0.3 ms in the
main pass and, in the tunnel, about 0.8 ms in the reflections (their hits read the grid), where points near a
cascade's edge read two. Classification takes 40 % off the ray tracing section's probes and 45 % off the tunnel's
(open air above the scene and around the tunnel; the irradiance and distance passes over every probe stay): 0.48
and 0.55 ms less, and with 1 m probes in 3 cascades it takes the probes from 3.35 to 1.82 ms with relocation.
Relocation adds 0.06 to 0.07 ms to the probes (the data pass) and 0.03 to 0.09 ms to the main pass (a probe's
offset read per sample). With 4 turns classification still
saves a quarter (0.442 to 0.333 ms). Against the path tracer's 512 samples (the same frames, one run, mean
difference of the 1280 x 720 captures, levels; pixels more than 16 levels off): the ray tracing section's 2 m grid
5.24 (10.1 %), a single 0.5 m grid 13.10 (57.5 %: it covers 8 m, beyond it the sky's ambient light), 0.5 m in
3 cascades 4.66 (8.0 %), 1 m in 3 cascades 4.64 (5.1 %), with relocation and classification 4.62 (5.2 %): the
ground beyond the 2 m grid's 16 m is grey in the one-grid frame and lit as in the reference with the cascades. In the
tunnel every configuration is 10.3 to 10.5 from the reference (its indirect light is a small part of the frame);
relocation changes the frame by a mean of 0.79 levels, mostly the crates' sides by the magenta panel.

The defaults (one cascade, neither option) against the commit before (`3d530ee`), the sections' defaults, four
pairs alternating (17:23-17:27): `probes` 1.184, 1.184, 1.189, 1.188 before and 1.205, 1.206, 1.200, 1.198 after in
the ray tracing section (+1 to 2 %), 1.206, 1.208, 1.210, 1.243 and 1.219, 1.220, 1.215, 1.213 in the tunnel; `hdr`
0.769, 0.770, 0.771, 0.770 and, with one cascade's own path in the shading (the last two pairs), 0.775, 0.778
(0.806 and 0.808 in the first two pairs, before that path); the tunnel's `hdr` 1.995, 1.991, 2.005, 2.065 and 2.042,
2.056 with it. CPU p50 is the same within the runs' noise (0.46 to 0.51 ms in the ray tracing section and the
tunnel in the first two pairs, both binaries; the last two pairs ran under a load average of 47).
The captures of every other part of the batches are unchanged (their golden images pass).

On a phone (not run yet): the iPhone 17's probes cost about 2.85 times the M4 Pro's (3.3 ms against 1.16;
[Stage 11 on the iPhone](#stage-11-on-the-iphone)), so by the Mac's ratios classification would take the ray tracing
section's grid to about 2.1 ms, 4 turns with classification to about 0.95 ms, and 1 m in two cascades with
relocation, classification and 4 turns to about 1.9 ms plus about 0.35 ms more in the main pass at half size (the
phone's per-pixel sampling cost about 2.5 times the Mac's per pixel); those are estimates. Proposed for the ray
tracing section's phone defaults, not set: the probe grid on with classification in 4 turns, one 2 m cascade, at
half size and 60 Hz, if `scenarios/raytracing_phone_probes.txt` (its `probes_4_class` and `probes_4_class_60hz`
regions) shows it fits the 16.7 ms frame with room; the same for the tunnel's (its grid classifies 45 % away, not
measured on a phone either); relocation where a scene's walls hold probes (about 0.2 ms more by the same ratio); no
second cascade on phones until a run shows the reflections' and the main pass's sampling cost there.

### Local light shadow maps

`RenderSettings::localShadowMapSize` ([Render module](render.md#local-light-shadow-maps)). 2026-11-07 07:28-07:33,
Release, Mac (Apple M4 Pro), **offscreen** at 1920 x 1080 (`--frame-size 1920x1080 --audio-device null --script`,
unpaced), 4 s regions after a second each, thermal state nominal.

**Frames without maps** (the 5% rule and "no cost where unused"): the commit before (`fbb8fb4`) and this change
alternating, two pairs per backend; the demo's materials and shadows sections, the ray tracing section and the
tunnel at their defaults (on the native backend traced, so no maps anywhere). Native backend, GPU busy time p50 of
`hdr`, ms; SDL GPU, frame p50 and CPU p50, ms:

| Section | native `hdr` before | after | SDL GPU frame before | after | SDL GPU CPU before | after |
|---|---|---|---|---|---|---|
| materials | 0.932, 0.931 | 0.932, 0.933 | 1.648, 1.648 | 1.671, 1.651 | 0.466, 0.470 | 0.465, 0.449 |
| shadows | 0.644, 0.643 | 0.643, 0.643 | 1.325, 1.313 | 1.331, 1.318 | 0.425, 0.443 | 0.291, 0.430 |
| ray tracing | 0.765, 0.765 | 0.765, 0.765 | 1.585, 1.599 | 1.582, 1.581 | 0.544, 0.586 | 0.571, 0.592 |
| tunnel | 1.995, 1.995 | 1.990, 1.990 | 0.956, 0.977 | 0.963, 0.993 | 0.596, 0.581 | 0.606, 0.582 |

(The SDL GPU tunnel's frame p50 is bimodal, p10 0.53 against p50 0.96, and says little.) Two earlier versions of
the lookup cost frames without maps: inside the light loop of `surface_fs` 3.4 % of the materials section's `hdr`
(0.962 against 0.930), and outside the loop but in `surface_fs` 3.2 % of the tunnel's (2.057 against 1.992), so the
maps have their own variant, `surface_local_shadows_fs`.

**Frames with maps** (SDL GPU's Metal driver, the ray tracing section at full size with bilinear scaling, its point
lamp (6 maps) and spot lamp (1 map); the tunnel's two spot lamps, 1 map each, before area lights took maps (since
then the tunnel's four nearest lamps: [below](#area-light-shadow-maps)); two runs):

| Region | frame p50 | CPU p50 | SDL GPU's GPU time p50 |
|---|---|---|---|
| ray tracing section, lamps without shadows | 2.018, 2.071 | 0.623, 0.632 | 5.437, 5.604 |
| the same, maps of 256 | 2.444, 2.408 | 0.749, 0.779 | 6.629, 6.458 |
| the same, maps of 512 | 2.405, 2.382 | 0.765, 0.731 | 6.475, 6.429 |
| the same, maps of 1024 | 2.476, 2.425 | 0.772, 0.786 | 6.704, 6.480 |
| tunnel, lamps without shadows | (bimodal) | 0.675, 0.654 | 11.961, 11.396 |
| the same, maps of 512 | (bimodal) | 0.717, 0.660 | 12.711, 12.634 |

Seven maps add 0.3 to 0.4 ms to the frame and 0.1 to 0.15 ms of CPU time (seven depth passes with their culling and
draws); the size matters little at these counts of casters. The native backend's timed range `local_shadows`
splits it per pass where it runs, but on the Mac the native backend traces these shadows instead. Not measured on
a phone (the reason to have the maps: phones on SDL GPU, the default until 2026-11-07, and devices without ray
tracing).

### Area light shadow maps

`localShadowMapSize` with area lights, read in a screen-space pass ([Render module](render.md#local-light-shadow-maps)).
2026-11-07 15:30-15:55, Release, Mac (Apple M4 Pro), **offscreen** at 1920 x 1080 (`--frame-size 1920x1080
--audio-device null --script`, unpaced), regions of 3 s after a second. The demo's tunnel from its overview with
`set local_shadow_maps 512`: the four nearest lamps (two floor strips, the ceiling square, the orange tube), 21 maps.
On the Mac the native backend traces these shadows, so the timings per pass come from a local build whose renderer
ignored `gpu::Caps::rayTracing` (not committed): the fallback path on the native backend, with its timed ranges.
GPU busy time p50, ms, two runs:

| Render size | `game` without maps | with | `local_shadows` | `prepass` | `local_filter` | `hdr` without | with |
|---|---|---|---|---|---|---|---|
| 1286 x 724 (the section's 0.67, MetalFX temporal) | 2.925, 2.949 | 3.843, 3.844 | 0.230, 0.231 | 0.023 | 0.696, 0.695 | 1.278, 1.282 | 1.272, 1.270 |
| 1920 x 1080 (bilinear) | 3.539, 3.511 | 5.308, 5.167 | 0.368, 0.360 | 0.033 | 1.380, 1.377 | 2.777, 2.751 | 2.671, 2.647 |

So 0.92 ms more at the section's size and 1.7 ms at the full size. CPU p50 0.37 to 0.41 ms without the maps and
0.49 to 1.09 with them at 0.67, 0.66 to 0.70 and 1.16 to 1.42 at the full size (21 depth passes with their culling
and draws; the runs spread widely). How the cost came down:

- **The soft filter in the main pass** (per surface fragment, 8 search and 16 filter taps; SDL GPU's Metal driver at
  1080p): 355 frames in 4 s against 1137 without maps, about 7.8 ms more per frame. With the lookup cut out (the
  depth passes alone) 842 frames in 3 s, as without maps: the filter was the cost, the 21 passes are cheap.
- **In a screen pass at half resolution** (8 and 8 taps): `local_filter` 1.68 ms at 1080p, of which 0.68 ms without
  any tap and about 0.55 ms for a 4 x 4 blur; the 3 x 3 blur of now: 1.38 ms. With the 4 x 4 blur at the section's
  size the map size changed `local_filter` little (0.78, 0.80 and 0.98 ms at 256, 512 and 1024; `local_shadows`
  0.22, 0.23 and 0.36 ms).

**Against the traced shadows** (the same frame at full size with the sky's reflections and ambient light, lamps
standing): mean difference 1.05 for SDL GPU's maps and 1.00 for the native backend's, looked at: the crates', the
plinth's and the pillars' shadows and the orange tube's light cut by its bay agree; the traced frame shows blotches
of its denoiser on the near walls that the maps do not.

**Frames without maps** (the defaults): the binary of `f915958` and this change, one pair per backend (a second pair
ran while another agent's demo used the GPU and slowed both binaries alike; left out). Native backend, `hdr` GPU p50
before and after: materials 0.934 and 0.936, shadows 0.646 and 0.649, ray tracing 0.767 and 0.769, tunnel 1.969 and
1.995 ms; SDL GPU frames in 4 s: 2331 and 2408, 2950 and 2983, 2472 and 2474, 1188 and 1193.

**On a phone:** not measured. The iPhone 17 took about 3 times the M4 Pro's time per effect (stage 11); the tunnel's
phone fallback runs at three quarters of its size (905 x 1966, 0.86 times 1080p's pixels) at 60 Hz in frames of
about 12 ms ([The tunnel on the iPhone](#the-tunnel-on-the-iphone)), so four lamps would add about 4.5 ms (3.6 ms
screen pass, 0.7 to 1.1 ms maps) and two lamps (11 maps) somewhat more than half of that: within the 60 Hz interval
on paper, to be measured by `scenarios/lamp_maps_phone.txt` on SDL GPU (`--gpu-backend sdl`: the tunnel's four
lamps at 256 and 512, two at 256, then two for 20 s at 60 Hz).

### Screen-space ambient occlusion

`AmbientOcclusion::ScreenSpace`, GTAO ([Render module](render.md#screen-space-ambient-occlusion)). 2026-11-07
07:58-08:15, Release, Mac (Apple M4 Pro), **offscreen** at 1920 x 1080 (`--frame-size 1920x1080 --audio-device null
--script`, unpaced), 4 s regions after a second each, thermal state nominal.

**Frames without GTAO** (the 5% rule and "no cost where unused"): the commit before (`b94e6ec`) and this change
alternating, two pairs per backend, each section at its defaults (on the native backend the ray tracing section and
the tunnel traced). Measured with the first version of the change; what changed after it runs only in frames with
GTAO. Native backend, GPU busy time p50 of the frame's `game` range, ms; SDL GPU, frame p50 and CPU p50, ms:

| Section | native `game` before | after | SDL GPU frame before | after | SDL GPU CPU before | after |
|---|---|---|---|---|---|---|
| materials | 1.815, 1.809 | 1.815, 1.815 | 1.665, 1.647 | 1.646, 1.647 | 0.506, 0.479 | 0.482, 0.480 |
| shadows | 1.446, 1.452 | 1.445, 1.453 | 1.334, 1.320 | 1.326, 1.298 | 0.394, 0.430 | 0.424, 0.435 |
| ray tracing | 7.658, 7.649 | 7.668, 7.655 | 1.582, 1.579 | 1.604, 1.603 | 0.561, 0.569 | 0.569, 0.554 |
| tunnel | 11.170, 11.182 | 11.227, 11.178 | 1.000, 1.023 | 1.287, 1.224 | 0.596, 0.600 | 0.583, 0.583 |

(The SDL GPU tunnel's frame p50 is bimodal and says little.)

**Frames with GTAO** (`set ao screen` against none, or against the traced occlusion in the native ray tracing
section; two runs each):

| Region | native `game` | native `gtao` (`prepass`) | SDL GPU frame p50 | SDL GPU CPU p50 | SDL GPU's GPU time p50 |
|---|---|---|---|---|---|
| shadows section, no occlusion | 1.498, 1.498 | | 1.327, 1.324 | 0.418, 0.428 | 3.504, 3.381 |
| the same, GTAO | 1.979, 1.976 | 0.413, 0.414 (0.029) | 1.784, 1.802 | 0.487, 0.362 | 4.845, 4.986 |
| ray tracing section, its default (native: traced occlusion; SDL GPU: none) | 7.631, 7.628 | | 1.585, 1.581 | 0.547, 0.566 | 4.254, 4.236 |
| the same, GTAO | 7.179, 7.183 | 0.174, 0.175 | 1.997, 1.997 | 0.658, 0.700 | 5.390, 5.353 |
| tunnel, its default | 11.128, 11.144 | | (bimodal) | 0.603, 0.594 | 9.013, 9.057 |
| the same, GTAO (the camera drives on: another view) | 11.320, 11.326 | 0.251, 0.251 | (bimodal) | 0.644, 0.648 | 11.129, 11.030 |

On the native ray tracing section GTAO replaces the occlusion rays: `rays` 2.30 to 1.70 ms, the frame 0.45 ms
cheaper. The first version ran GTAO at full resolution with world positions from the inverse view-projection per
sample and a 5 x 5 blur over the center's plane: `gtao` 1.482 ms in the shadows section (0.93 the horizon search,
0.55 the blur), 0.651 in the ray tracing section, 0.807 in the tunnel. Half resolution, positions along camera rays
and a 4 x 4 blur of the term with its depth beside it (`Rg16Float`) brought that to 0.41, 0.17 and 0.25 ms; the
contact gets softer (the test's 64 x 64 image: 0.940 against 0.885 next to the box). In the first version the
radius cap (20 % of the render height) did not change the cost in these views (0.05 to 0.2 of the height: the
same), and each step to either side of a slice cost about 0.1 ms.

Not measured on a phone. `scenarios/gtao_phone.txt` measures it there on SDL GPU (the three sections without
occlusion and with GTAO, from the nominal thermal state, and a capture); on the Mac offscreen at 1170 x 2532 it
passed 8 of 8 (SDL GPU's GPU time p50 4.42 to 5.96 ms in the shadows section, 5.09 to 6.52 in the ray tracing
section).

### Screen-space reflections

`Reflections::ScreenSpace`, SSR ([Render module](render.md#screen-space-reflections)). 2026-11-07 08:43-09:02,
Release, Mac (Apple M4 Pro), **offscreen** at 1920 x 1080 (`--frame-size 1920x1080 --audio-device null --script`,
unpaced), 4 s regions after a second each, thermal state nominal.

**Frames without SSR** (the 5% rule and "no cost where unused"): the commit before (`3abc380`) and this change
alternating, two pairs on the native backend and four on SDL GPU, each section at its defaults (on the native
backend the ray tracing section and the tunnel traced). Measured with an earlier version of the change; what
changed after it runs only in frames with SSR. Native backend, GPU busy time p50 of the frame's `game` range, ms;
SDL GPU, frame average (the tunnel's p50 is bimodal) and the means of the CPU p50 and p99, ms:

| Section | native `game` before | after | SDL GPU frame before | after | CPU p50 before | after | CPU p99 before | after |
|---|---|---|---|---|---|---|---|---|
| materials | 1.822, 1.832 | 1.823, 1.813 | 1.659 | 1.663 | 0.473 | 0.477 | 0.866 | 0.878 |
| shadows | 1.455, 1.451 | 1.443, 1.447 | 1.341 | 1.341 | 0.417 | 0.424 | 0.893 | 0.915 |
| ray tracing | 7.652, 7.664 | 7.666, 7.654 | 1.603 | 1.604 | 0.575 | 0.571 | 0.956 | 0.962 |
| tunnel | 11.197, 11.202 | 11.187, 11.137 | 3.345 | 3.338 | 0.578 | 0.595 | 1.395 | 1.408 |

The largest change is the tunnel's CPU p50 (+2.9 %), within the 5 % and within the spread of the four runs
(0.556 to 0.605 before, 0.583 to 0.602 after).

**Frames with SSR** (`set reflections screen` against each section's default: the sky's reflection, on the native
backend the traced ones in the ray tracing section and the tunnel; two runs each):

| Region | native `game` | native `ssr` (`prepass`, `ssr history`) | SDL GPU frame average | SDL GPU CPU p50 |
|---|---|---|---|---|
| materials section, its default | 1.818, 1.828 | | 1.658, 1.655 | 0.476, 0.486 |
| the same, SSR | 2.096, 2.093 | 0.116, 0.116 (0.091, 0.134) | 1.903, 1.908 | 0.552, 0.551 |
| shadows section, its default | 1.456, 1.459 | | 1.342, 1.340 | 0.439, 0.421 |
| the same, SSR | 1.783, 1.773 | 0.183, 0.184 (0.076, 0.177 and 0.084) | 1.672, 1.637 | 0.483, 0.484 |
| ray tracing section, its default (native: traced reflections, `reflections` 0.51) | 7.649, 7.633 | | 1.600, 1.614 | 0.585, 0.602 |
| the same, SSR | 7.193, 7.184 | 0.090, 0.089 (0.107, 0.018) | 1.845, 1.844 | 0.633, 0.659 |
| tunnel, its default (native: traced reflections, `reflections` 3.65) | 11.139, 11.129 | | 3.328, 3.342 | 0.591, 0.572 |
| the same, SSR | 8.440, 8.448 | 0.695, 0.697 (0.160, 0.020) | 5.230, 5.236 | 0.777, 0.779 |

The native tunnel renders at 1286 x 724 (temporal upscaling) and its ray tracing section's G-buffer prepass is there
anyway; SDL GPU renders at 1080p and draws the G-buffer prepass for SSR. In the SDL GPU tunnel, leaving out passes
one at a time (one run each, frame average): the prepass and the copy 0.37 ms, the resolve about 0.4 ms, the march
about 1.2 ms (half of 1080p: 518,400 rays of up to 48 steps along the tunnel's long floor and walls). On the native
tunnel the march is 0.54 of `ssr`'s 0.70 ms and the resolve 0.16. Stepping linearly in clip space instead of
projecting every step changed nothing (0.694 to 0.685 ms: the depth loads, not the arithmetic); 32 steps growing
1.15 or 1.18 times cost 0.56 ms, 24 steps growing 1.25 times 0.50 ms, so 48 steps growing 1.1 times stay. The
resolve's Karis weights cost about 0.05 ms in the tunnel and keep single bright rays from showing as squares (looked
at in a capture of the tunnel).

Not measured on a phone. `scenarios/ssr_phone.txt` measures it there on SDL GPU (the materials, ray tracing and
tunnel sections with the sky's reflection and with SSR, from the nominal thermal state, and a capture); on the Mac
offscreen at 1170 x 2532 it passed 8 of 8 (SDL GPU's GPU time average 11.77 to 15.80 ms in the tunnel, 5.10 to
5.77 in the ray tracing section, 5.37 to 5.42 in the materials section; the capture looked at).

### On the screen after the bug packages

2026-11-07 00:00-00:06, Release, Mac (Mac mini, Apple M4 Pro), its one display (the LG UltraFine 5K at 60 Hz; no
second display to compare), `--onscreen --frame-size 1920x1080 --audio-device null`, nothing else running.

**`--present-mode immediate`**, hello's `benchmark.txt`, one run per backend: unpaced on both. SDL GPU (Metal driver):
3806 frames in 10 s, frame p50 3.067 ms, wait p50 2.805 / p95 3.711 ms, CPU p50 0.231 ms, GPU p50 0.547 ms. Native
Metal: 3583 frames, frame p50 2.912 ms (p99 16.692, max 23.584), wait p50 2.676 / p95 3.863 ms, CPU p50 0.227 ms,
GPU p50 0.549 ms.

**Vsync, before and after the bug packages of 2026-11-06** (`1ae3117`, the 0.9.0 release, against `1ef1f1c`), hello's
`benchmark.txt` on SDL GPU, runs in the order before, after, after, before:

| Build | Frames | Frame p50 | Frame max | CPU p50 | CPU p99 | GPU p50 | GPU p95 |
|---|---|---|---|---|---|---|---|
| `1ae3117` | 601, 600 | 16.661, 16.650 | 17.848, 18.357 | 0.405, 0.402 | 0.567, 0.575 | 0.551, 0.552 | 0.945, 0.945 |
| `1ef1f1c` | 600, 600 | 16.663, 16.666 | 32.914, 17.541 | 0.407, 0.408 | 0.533, 0.515 | 0.552, 0.551 | 0.945, 0.946 |

CPU p50 +1.0% and p99 -8.2% (means of the two runs), GPU unchanged; one interval missed in the first run after (the
32.9 ms frame). The new wait: p50 16.148 and 16.131 ms, p95 16.529 and 16.461 ms.

**MetalFX denoising** in `raytracing_benchmark.txt` on the native backend with `--present-mode immediate`, one run
(the whole table's conditions of [Stage 11 on the Mac](#stage-11-on-the-mac-and-earlier-runs)): the anomaly of
2026-11-06 came back, in the other region. At 0.67 the ranges after the upscaler read as elsewhere (`upscale` 6.387,
`bloom` 0.240 ms); at 0.5 `bloom` read 2.454 ms, `tonemap` 0.389, `ui` 0.229 and `blit` 0.276 (0.07 to 0.22 ms in
every other region), `probes` 1.683 (1.14 to 1.15 ms elsewhere, 1.240 at 0.67), and the region's `game` 12.705 ms was
longer than the 0.67 region's 11.884. No other upscaler shows it. A hypothesis, not checked: part of MetalFX's
denoising work runs outside the `upscale` range (its end marker waits only for the output texture) and overlaps
the passes after it and the next frame's first ones, so it lands in their ranges; which region shows it changes from
run to run. Not an ordering error: `bloom` reads the output texture, which Metal's hazard tracking waits for. Read
the denoising upscaler's cost from the frame interval (unpaced): 11.317 ms at 0.67 and 9.225 ms at 0.5, against
6.912 ms with MetalFX temporal at 0.67. Its CPU p50 at 0.67 was low again (0.496 ms; 0.446 on 2026-11-06) with 26%
of the process's CPU time on performance cores, not explained by the share alone.

**MetalFX denoising offscreen, and the hypothesis tested** (2026-11-07 05:58-06:00, Release, offscreen, native
backend, 1920 x 1080, the ray tracing section with everything traced, 4 s regions). `bloom` at render scale 0.5,
0.6, 0.67 and 0.75: 0.269, 0.242, 0.266 and 0.245 ms (`upscale` 4.307, 4.885, 6.744, 7.769 ms; frame p50 9.01,
9.35, 12.91, 14.03 ms); MetalFX temporal at 0.5 and 0.67: 0.221 ms. So the anomaly does not show offscreen at any of
the four scales. To test the hypothesis that MetalFX runs work after its output is written, a temporary build put a
full barrier (a command buffer event signalled and waited for) between MetalFX's passes and the `upscale` end
marker, so the marker could not start before every earlier command had finished. Measured in one process each, with
one frame's ranges logged every 97 frames: `upscale` 4.312 against 4.300 ms with the barrier at 0.5, 6.825 against
6.933 ms at 0.67; `bloom` 0.269 against 0.230 and 0.266 against 0.252 ms, the same drop as with MetalFX temporal
(0.221 against 0.192 ms), where it comes from `bloom`'s vertex stage no longer starting before the upscaler's end
(without the barrier `bloom` starts about 0.5 ms before `upscale` ends with both scalers; with it, at its end). So
MetalFX's denoising upscaler leaves no measurable work behind the marker, the markers attribute it correctly, and the
engine's timing was not changed (the barrier was removed). What is left happens only in a window on the screen, in
one GPU-bound denoising region per run, and moves between runs; a guess, not checked: in a GPU-bound frame paced to
the display, other GPU work of the system (the window server compositing the window) runs at the same point of
every frame and lands in whichever range runs then. A Metal System Trace in Instruments on the screen would show it
(a profiler the user has not allowed yet). Read MetalFX denoising's cost from offscreen regions.

### The tunnel

Stage 12 (area lights and anisotropic GGX, the demo's tunnel section), 2026-11-06 20:30-20:54, Release, the code of
`9b024a4` (the tunnel's phone defaults of `0704ce5` for the iPhone's 60 Hz runs). Its iPhone numbers are
[below](#the-tunnel-on-the-iphone).

**The tunnel on the M4 Pro.** `raytracing_benchmark.txt` on the native backend at 1920 x 1080, on the screen,
`--present-mode immediate`, one run (20:48-20:51). The display ran at 120 Hz in this session and paced the window
also without display sync (481 frames in 4 s), so the region without rays comes from the same scenario part run
offscreen (unpaced, 1359 frames in 4 s); the other regions were slower than the display's interval (GPU-bound, the
clock at its full rate) and their offscreen frame times agree within 2%. GPU busy time per range, medians, ms:

| Region | Render size | Frames / 4 s | Frame p50 | CPU p50 | game | rays | denoise | probes | reflections | hdr | taa | upscale | path |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| the lamps unshadowed (the fallback's frame; offscreen) | 1920 x 1080 | 1359 | 2.93 | 0.714 | 3.053 | | | | | 2.338 | 0.367 | | |
| four traced lamp shadows | 1920 x 1080 | 477 | 8.40 | 1.237 | 8.586 | 3.503 | 1.939 | | | 2.285 | 0.318 | | |
| + traced reflections | 1920 x 1080 | 275 | 14.59 | 1.610 | 14.790 | 3.505 | 1.945 | | 6.030 | 2.299 | 0.318 | | |
| + the probe grid (everything traced) | 1920 x 1080 | 213 | 18.84 | 1.723 | 19.109 | 3.508 | 1.943 | 1.179 | 7.752 | 3.675 | 0.318 | | |
| the desktop default: everything, MetalFX temporal at 0.67 | 1286 x 724 | 369 | 10.80 | 1.297 | 10.876 | 1.651 | 0.886 | 1.177 | 3.694 | 1.749 | | 1.287 | |
| path tracer | 1920 x 1080 | 139 | 28.43 | 1.173 | 29.051 | | | | | | | | 28.109 |

Eight area lights and two spots cost 2.3 ms in the main pass at 1080p (the ray tracing section's shadow-map frame:
1.1 ms). Four traced lamp shadows add 5.5 ms (rays 3.5, their filter 1.9). Traced reflections add 6.2 ms, seven
times their cost in the ray tracing section (0.86 ms): almost every pixel of the tunnel is metal or wet below the
roughness cutoff, and every hit is shaded with the area lights. The probe grid adds 4.3 ms (1.2 for the probes, the
rest where the main pass and the reflection hits sample it). The desktop default runs at 92 frames per second
(10.9 ms). The path tracer takes 28.1 ms per sample at 1080p (the ray tracing section in the same run: 12.4 ms): at every
hit it samples one lamp at random, an area light at a point of its shape weighted against the BRDF rays that hit
the shape (MIS), and the glossy metal walls carry the paths on.

**The cost in frames without area lights.** The area light code and the anisotropic lobe are in the standard
surface shader whether a frame uses them or not (branches on uniforms), and they cost there. Offscreen A/B against
`1ae3117` (the code before stage 12, built in a separate worktree), unpaced, 1920 x 1080, frames in 4 s on the
native backend (two runs each; the runs of a build agree within 0.5%):

| Section | before (`1ae3117`) | stage 12 (`613f442`) | the area light loop apart (`9b024a4`) | experiment: no anisotropy | experiment: no anisotropy, no area lights |
|---|---|---|---|---|---|
| materials | 2376 | 2170 (-8.7%) | 2196 (-7.6%) | 2288 | 2335 |
| sky | 2913 | 2722 (-6.6%) | 2755 (-5.4%) | 2860 | 2911 |
| post | 3040 | 2838 (-6.6%) | 2870 (-5.6%) | 2968 | 3028 |

The experiments compiled the paths out (anisotropy fixed at 0, the area light block never taken); they were not
kept. About half of the remaining cost is the anisotropic lobe, the rest the area light block; with neither, the
frame is back to before within 0.4% (sky, post) and 1.7% (materials). The same holds on SDL GPU: the demo
benchmark offscreen, two runs each, alternated, 6 to 8% fewer frames in every section (before `9b024a4`). At 1080p
this is 0.08 to 0.14 ms per frame on the M4 Pro; with vsync the frame times are the display's in both builds.

**Surface shader variants** (2026-11-06, after stage 12): `surface_basic_fs`, the surface shader without the area
lights, the anisotropic lobe and the area light shapes, draws every material without anisotropy in frames without
area lights ([Render module](render.md#surface-shaders)). Frames in 4 s, unpaced offscreen at 1920 x 1080
(`--present-mode immediate`), means of three rounds with the builds alternated: first `1ae3117` (before stage 12)
against stage 12 without the variants (22:50), then `1ae3117` against the variants (23:00):

| Section | before | without variants | before | with variants |
|---|---|---|---|---|
| SDL GPU: materials | 2435 | 2250 (-7.6%) | 2433 | 2421 (-0.5%) |
| SDL GPU: sky | 2943 | 2779 (-5.6%) | 2946 | 2927 (-0.6%) |
| SDL GPU: post | 3103 | 2931 (-5.6%) | 3104 | 3106 (+0.0%) |
| SDL GPU: shadows | 2997 | 2808 (-6.3%) | 2994 | 2993 (-0.0%) |
| native: materials | 2337 | 2151 (-8.0%) | 2375 | 2370 (-0.2%) |
| native: sky | 2774 | 2670 (-3.8%) | 2914 | 2909 (-0.2%) |
| native: post | 2940 | 2786 (-5.2%) | 3034 | 3040 (+0.2%) |
| native: shadows | 2735 | 2611 (-4.5%) | 2942 | 2942 (-0.0%) |

The native rounds vary more than SDL GPU's (the first run's "before" rounds by up to 4%). The native means with
variants are of two rounds: the third ran 3 to 6% slower in every section, the shadows section too, whose shading
the variants do not change, and the "before" round just ahead of it was 3% slower in shadows (with it: materials
-2.3%, shadows -1.1%). A frame without area lights and anisotropy is back to before within 0.6%.

**The CPU (the 5% rule).** The demo benchmark's CPU times on SDL GPU, same offscreen A/B (11 sections, the sum of
the sections' values; per section the runs of one build differ by up to 2 times): CPU p50 5.20 ms before, 5.25 ms
after (+1.0%); CPU p99 10.90 and 11.22 ms (+2.9%). On the screen with vsync (one pair at 60 Hz, 20:33-20:35) the
sums read 11.06 and 14.47 ms, but runs of the old build alone ranged from 10.56 to 13.34 ms that day (60 and 120 Hz):
on the screen the CPU clock follows the light load and the CPU times do not resolve 5%. The hello benchmark on the
screen at 120 Hz (one run each): CPU p50 0.298 ms before, 0.234 ms after; p99 0.654 and 0.400 ms. The frame's added
CPU work is the area light count in the light list, one more texture per pass and 16 more bytes per material.

### The tunnel on the iPhone

iPhone 17 (A19 GPU, iOS 27.0.1), 2026-11-06 20:30-20:54: Release (`ios-xcode`), the native size 1206 x 2622 in
portrait, 120 Hz, `--audio-device null`, launched through `devicectl`, no validation layer. Nobody looked at the
screen; the captures were copied from the device and looked at.

**The cost of each option** (`scenarios/tunnel_phone.txt`, 20:30-20:35, the native backend, one run; a first run at
20:28 went to the background after two regions, possibly the user handling the phone, as they did during other runs
that day (not confirmed for this one), and its two regions agree within 4%): 4 s regions after
a cool-down to nominal, at the display's rate. As in [stage 11](#stage-11-on-the-iphone), frames slower than the
display's 8.33 ms run the GPU at its full clock (their busy times are costs); at 481 frames the clock follows the
load. GPU busy time per range, medians, ms:

| Region | Render size | Thermal | Frames / 4 s | Frame p50 / p99 | CPU p50 | game | rays | denoise | reflections | probes | hdr | taa | upscale |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| the lamps unshadowed, MetalFX spatial at 0.5 | 603 x 1311 | nominal | 481 | 8.32 / 9.17 | 1.298 | 5.687 | | | | | 2.786 | 0.516 | 1.295 |
| two lamp shadows | 603 x 1311 | nominal | 481 | 8.33 / 8.57 | 1.833 | 7.714 | 1.191 | 2.050 | | | 2.265 | 0.344 | 1.080 |
| four lamp shadows | 603 x 1311 | nominal | 418 | 8.35 / 17.78 | 1.734 | 9.679 | 2.124 | 2.303 | | | 2.441 | 0.386 | 1.157 |
| two lamp shadows + reflections | 603 x 1311 | nominal | 274 | 13.86 / 22.32 | 1.881 | 14.903 | 1.469 | 2.469 | 5.285 | | 2.462 | 0.397 | 1.175 |
| four lamp shadows + reflections | 603 x 1311 | nominal | 253 | 16.67 / 22.31 | 1.885 | 16.136 | 2.309 | 2.514 | 5.449 | | 2.512 | 0.406 | 1.232 |
| everything traced (+ probe grid) | 603 x 1311 | nominal | 174 | 24.98 / 25.12 | 1.992 | 23.354 | 2.302 | 2.518 | 7.493 | 3.517 | 4.330 | 0.419 | 1.197 |
| two lamp shadows + reflections, MetalFX temporal at 0.67 | 808 x 1757 | fair | 141 | 26.10 / 32.37 | 0.247 | 28.674 | 2.580 | 4.476 | 10.255 | | 4.914 | | 4.686 |
| the lamps unshadowed at full size, bilinear | 1206 x 2622 | fair | 284 | 13.16 / 18.48 | 0.920 | 14.333 | | | | | 10.838 | 2.024 | |

On SDL GPU (`scenarios/tunnel_phone_fallback.txt`, 20:53, the fallback frame at the full size, no timings per
pass): 660 frames in 8 s at the display's rate (frame p50 11.92 ms, p99 20.57 ms, CPU p50 0.838 ms, thermal
nominal).

Traced reflections cost about 5.3 ms at half size (7.5 ms with the probe grid, whose light the hits sample), more
than the two lamp shadows' rays and filter (3.2 ms); the probe grid 3.5 ms as in the ray tracing section. Two thirds
of the size with MetalFX temporal costs about twice half size with spatial (28.7 against 14.9 ms with the same
effects; the former in the fair state, where every range grows). The fallback frame at the full size does not fit 120 Hz on either backend (14.3 ms native in the fair state,
frames of about 12 ms on SDL GPU at nominal): its main pass with eight area lights over 3.2 million pixels took
10.8 ms (fair).

**The phone defaults** (`TunnelSection.cpp`, since `0704ce5`): every phone runs the section at 60 Hz; phones that
trace use two lamp shadows, no traced reflections, no probe grid, half size with MetalFX spatial. Held at 60 Hz for
40 s from nominal, after a cool-down (two 20 s regions; GPU p50 is the span):

| Run | Thermal | Frames / 20 s | Frame p50 / p99 | CPU p50 / p99 | GPU p50 | game | rays | denoise | reflections | hdr | upscale |
|---|---|---|---|---|---|---|---|---|---|---|---|
| native, the defaults, 0-20 s | nominal | 1158 | 16.669 / 16.775 | 2.012 / 2.272 | 7.994 | 7.934 | 1.210 | 2.061 | | 2.252 | 1.058 |
| native, the defaults, 20-40 s | nominal | 1200 | 16.669 / 16.756 | 1.683 / 1.901 | 15.273 | 15.164 | 2.282 | 3.964 | | 4.555 | 1.852 |
| native, the defaults with traced reflections, 0-20 s | nominal | 1159 | 16.669 / 16.768 | 1.868 / 2.240 | 12.739 | 12.678 | 1.210 | 2.058 | 4.570 | 2.162 | 1.064 |
| native, the defaults with traced reflections, 20-40 s | nominal -> fair | 1200 | 16.666 / 16.779 | 1.857 / 2.180 | 15.336 | 15.236 | 1.484 | 2.508 | 5.492 | 2.871 | 1.326 |
| SDL GPU, the fallback at the full size, 0-20 s | nominal -> fair | 1200 | 16.669 / 16.749 | 1.013 / 1.184 | 12.056 | | | | | | |

The first region of each run has one long frame (708 and 725 ms), the PNG encoding of the capture just before it:
a region counted the time from the frame before its first one (fixed after stage 12, 2026-11-06; on the M4 Pro, a
1206 x 2622 capture just before a region: max 609.9 ms in it before, 10.3 ms after).
Every frame made its 16.7 ms interval. In the second region of the defaults the GPU lowered its clock to fill the
interval (every range about 1.9 times longer at the same work, the state nominal): time at that clock, not a cost.
With traced reflections the first 20 s ran at 12.7 ms and the phone turned fair within 40 s; without them 7.9 ms and
nominal throughout, which is why the default leaves them out (the panel turns them on). The SDL GPU run came right
after 8 s at the display's rate. The iPhone 18 Pro Max (A20 Pro) has not run the tunnel.

### The tunnel in motion

The lamps and the camera's drive (2026-11-06 21:36-21:57, Release; the Mac on the code of `5cc53a6`, the iPhone on
`61a17ba`). The regions above stay as they were: their scenarios now set the lamps to their time 0 and stop them.

**The M4 Pro**, offscreen A/B on the native backend at 1920 x 1080, the section's desktop default (everything
traced, MetalFX temporal at 0.67: 1286 x 724), 4 s regions alternated three times: the lamps still at the overview,
the lamps moving, the lamps moving and the camera driving. Medians of the three runs; GPU busy time per range, ms
(the frame's GPU p50 reads 11.45 to 11.58 ms over a frame of 10.8, as offscreen frames overlap):

| State | Frames / 4 s | Frame p50 | CPU p50 / p99 | game | accel | rays | reflections | hdr |
|---|---|---|---|---|---|---|---|---|
| still | 370 | 10.841 | 1.243 / 1.897 | 10.939 | 0.072 | 1.676 | 3.682 | 1.767 |
| the lamps moving | 368 | 10.885 | 1.302 / 1.956 | 10.989 | 0.075 | 1.652 | 3.729 | 1.779 |
| the lamps moving, the camera driving | 371 | 10.797 | 1.252 / 1.975 | 10.893 | 0.075 | 1.563 | 3.499 | 1.966 |

The top level is refitted in every frame anyway (a full build every 30th), so moving instances add nothing to
`accel`; the moving lamps add 0.05 ms to the frame, and the drive changes the view (less in the reflections, more in
the main pass) rather than the cost. CPU p50 +0.06 ms (+4.7 %) with the lamps moving, +0.01 ms with the drive; p99
+3.1 % and +4.1 %: within the 5 % rule, and the runs of the still state alone spread from 1.230 to 1.264 ms.

**The iPhone 17**, `scenarios/tunnel_phone.txt` (21:43-21:46, the native backend, the phone defaults at 60 Hz):
40 s with the lamps still as before, then 20 s in motion; `tunnel_phone_fallback.txt` on SDL GPU (21:47 and 21:53):
8 s at the display's rate, 20 s at 60 Hz still and 20 s in motion. GPU busy time per range, ms (SDL GPU has none):

| Run | Thermal | Frames / 20 s | Frame p50 / p99 | CPU p50 / p99 | game | accel | rays | denoise | hdr | upscale |
|---|---|---|---|---|---|---|---|---|---|---|
| native, still, 0-20 s | nominal | 1159 | 16.669 / 16.764 | 2.066 / 2.245 | 7.986 | 0.157 | 1.221 | 2.065 | 2.295 | 1.060 |
| native, still, 20-40 s | nominal | 1200 | 16.669 / 16.760 | 1.736 / 1.922 | 15.165 | 0.059 | 2.283 | 3.943 | 4.536 | 1.847 |
| native, in motion, 40-60 s | nominal | 1200 | 16.669 / 16.749 | 1.703 / 1.903 | 15.253 | 0.059 | 2.220 | 3.724 | 5.290 | 1.804 |
| SDL GPU, the full size, still | nominal -> fair | 1200 | 16.670 / 16.753 | 1.035 / 1.176 | | | | | | |
| SDL GPU, the full size, in motion | fair | 1146 | 16.673 / 23.307 | 0.869 / 0.987 | | | | | | |
| SDL GPU, 0.75, still | nominal | 1200 | 16.669 / 16.782 | 0.968 / 1.090 | | | | | | |
| SDL GPU, 0.75, in motion | nominal | 1200 | 16.670 / 16.750 | 1.018 / 1.208 | | | | | | |

On the native backend the phone held 60 Hz in motion at the nominal thermal state after 40 s of load (the scenario
checks the state after the region). From 20 s on the GPU runs at a lowered clock that fills the interval (as above),
where the motion reads like the still frame (game 15.25 against 15.17 ms). On SDL GPU at the full size the phone
turned fair while still, as in the run of 20:53, and missed intervals in motion (p95 23.3 ms). So phones without ray
tracing render the section at three quarters of the size since `61a17ba` (905 x 1967 on the iPhone 17, bilinear;
MetalFX spatial where it runs): 916 frames in 8 s at the display's rate (frame p50 8.34 ms, p95 11.92 ms), then 60 Hz
still and in motion at nominal throughout. The captures (the defaults still and in motion, SDL GPU at 0.75, the
path tracer) were copied from the phone and looked at; nobody looked at the screen.

**The temporal denoiser in motion** (the Mac, native, offscreen; a frame in motion against the same moment held
still for 1.5 s, mean difference in levels of 255; two still frames 1.5 s apart differ by 0.46): the lamps moving
0.61 at full size with TAA and 0.75 at the default (MetalFX temporal at 0.67), the drive 1.13 and 1.11, both 1.18
and 1.16 (1280 x 720). The drive's difference sits on texture edges (TAA is softer in motion), not on the shadows or
the puddle; the crates under the turning amber spot differ by 0.58. A lamp that takes a shadow channel from another
(`RenderStats::localShadowChanges`, 4 to 10 times in 34 s of the drive or the overview) leaves no trace beyond that:
at the frame of the change 0.74 levels, against 0.85 at a frame without one (960 x 540). Nothing was tuned. What to
look at in motion is the check mode's item "Lamps and camera in motion" ([Engine demo](demo.md#check-mode-on-the-iphone)).

### Ten minutes on the iPhone

iPhone 17, 2026-11-06 23:37-23:47 (`scenarios/phone_sustained.txt`, the code of `6a822b7`; Release, `ios-xcode`,
the native Metal backend, 1206 x 2622, launched through `devicectl` on its cable; nobody looked at the screen): the
ray tracing section's phone defaults (half size, MetalFX spatial, no probe grid) for five minutes, then the tunnel's
phone defaults in motion for five more, all at 60 Hz in 20 s regions, from the nominal thermal state, without a pause.

| Regions | Thermal | Frames per region | Frame p99 / max ms | CPU p50 ms | GPU p50 / p95 ms |
|---|---|---|---|---|---|
| Ray tracing 1 | nominal | 1200 | 16.779 / 20.408 | 2.540 | 7.652 / 13.575 |
| Ray tracing 2-15 | nominal, fair from region 9 (about 3 min) | 1200 each | at most 16.776 / 16.869 | 2.221-2.245 | 15.318-15.334 / at most 15.617 |
| Tunnel 1-8 | fair | 1200 each | at most 16.763 / 16.921 | 1.727-1.780 | 15.297-15.350 / at most 15.571 |
| Tunnel 9 | fair | 1200 | 16.753 / 16.834 | 1.799 | 13.601 / 26.919 |
| Tunnel 10-15 | fair, serious from region 12 (about 9 min) | 1179, 1196, 1195, 1196, 1197, 1196 | 25.021 in region 10, else at most 16.818 / 25.016-25.159 | 1.755-1.792 | 15.347-16.451 / 28.860-31.387 |

The ray tracing defaults held 60 Hz for five minutes with no missed interval. From the second region on, every
range takes twice its first-region time (`raytrace` 4.80 against 9.70 to 9.72 ms): the GPU's clock lowered to fill the
interval, the work is the same. The tunnel held 60 Hz for its first 160 s; after about 8 minutes of load some of its
frames took two intervals (21 of 1200 in region 10, then 3 to 5 per region; GPU p95 near 29 ms), and the phone
turned serious at about 9 minutes. The process's share on performance cores read 0% in all 30 regions (process CPU
4.7 ms per frame with ray tracing, 3.8 ms in the tunnel); whether iOS counts performance-core time in
`TASK_POWER_INFO_V2` is not known, since no phone region has read anything else yet. The run had no capture before a
region, so it does not show the fix of the frame before a region on the phone (shown on the Mac only).

### Stage 11 on the iPhone

iPhone 17 (A19 GPU, iOS 27.0.1), 2026-11-06 18:42-18:56: Release (`ios-xcode`), the native size 1206 x 2622 in
portrait, 120 Hz, the native Metal backend with vsync, `--audio-device null`, launched through `devicectl`, no
validation layer, on its cable (probably charging). The iPhone 18 Pro Max was not available: nothing of stage 11 has
run on it. Nobody looked at the screen.

**The cost of each effect** (`scenarios/raytracing_phone.txt`, 18:42-18:45, one run, the code of `91a6c2f`): 4 s
regions in four groups, each group started at the nominal thermal state (the run waited for it at 20 frames per
second on the cheapest frame); the state of each region is in the table. Frames above the 8.33 ms display interval
run the GPU at its full clock, so their busy times are costs; at 120 frames per second (481 in 4 s) the clock follows
the load and the ranges read longer than they cost (the A19's plateau is about 5.6 to 5.9 ms, see the baseline
below). GPU busy time per range, medians, ms; `game` is the renderer's whole frame:

| Region | Render size | Thermal | Frames / 4 s | Frame p50 | CPU p50 | game | rays | denoise | probes | reflections | indirect | hdr | taa | upscale |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| RT shadows + AO (2 rays each): the base | 1206 x 2622 | nominal | 352 | 11.17 | 1.236 | 11.604 | 4.319 | 2.688 | | | | 1.463 | 1.314 | |
| base + 2 lamp shadows | 1206 x 2622 | nominal | 199 | 22.25 | 1.742 | 20.737 | 10.540 | 5.338 | | | | 1.472 | 1.376 | |
| base + reflections | 1206 x 2622 | nominal | 254 | 16.62 | 1.805 | 16.231 | 4.084 | 2.926 | | 3.976 | | 1.686 | 1.384 | |
| base + probe grid | 1206 x 2622 | nominal | 202 | 22.31 | 1.728 | 20.514 | 4.627 | 3.031 | 3.325 | | | 5.695 | 1.690 | |
| base + per-pixel indirect | 1206 x 2622 | nominal | 150 | 25.01 | 1.918 | 27.250 | 4.698 | 3.256 | 3.408 | | 10.496 | 1.711 | 1.500 | |
| all (lamps, reflections, probes), temporal denoiser | 1206 x 2622 | nominal -> fair | 118 | 33.34 | 2.077 | 34.370 | 11.127 | 6.034 | 3.525 | 4.517 | | 5.557 | 1.478 | |
| shadow maps (3 cascades), nothing traced | 1206 x 2622 | fair | 481 | 8.34 | 1.434 | 6.199 | | | | | | 2.866 | 1.750 | |
| all, MetalFX temporal at 0.67 | 808 x 1757 | nominal | 201 | 19.11 | 0.290 | 18.859 | 4.311 | 2.425 | 2.948 | 1.618 | | 2.154 | | 4.035 |
| all, MetalFX denoising at 0.67 | 808 x 1757 | nominal | 145 | 25.92 | 0.262 | 28.008 | 4.712 | | 3.045 | 1.257 | | 2.263 | 0.129 | 14.957 |
| all, MetalFX temporal at 0.5 | 603 x 1311 | nominal | 269 | 13.89 | 2.085 | 15.287 | 2.887 | 1.531 | 3.189 | 1.030 | | 1.424 | | 3.611 |
| all, MetalFX spatial at 0.5 | 603 x 1311 | nominal | 305 | 11.40 | 2.175 | 13.432 | 2.982 | 1.546 | 3.374 | 1.054 | | 1.422 | 0.380 | 1.108 |
| no lamp shadows, MetalFX spatial at 0.5 | 603 x 1311 | nominal -> fair | 364 | 11.15 | 2.076 | 11.467 | 1.351 | 0.822 | 3.494 | 1.122 | | 1.533 | 0.383 | 1.121 |
| no lamp shadows, MetalFX temporal at 0.5 | 603 x 1311 | fair | 307 | 11.30 | 1.965 | 13.465 | 1.298 | 0.826 | 3.466 | 1.051 | | 1.534 | | 3.630 |
| base, MetalFX spatial at 0.5 | 603 x 1311 | fair | 481 | 8.33 | 1.771 | 5.838 | 1.336 | 0.903 | | | | 0.613 | 0.442 | 1.285 |
| base, bilinear at 0.5 | 603 x 1311 | fair | 481 | 8.34 | 1.706 | 5.807 | 1.832 | 1.232 | | | | 0.868 | 0.602 | |

What each effect adds to the base at the device's size (the `game` difference): two lamp shadows +9.1 ms (their
rays +6.2, their filter +2.7); reflections +4.6 ms; the probe grid +8.9 ms (3.3 ms for the probes, 4.2 ms more in
the main pass, which samples the probe atlases per pixel); per-pixel indirect light +15.6 ms (10.5 ms of rays and
filter over the probes). Everything the desktop default turns on costs 34.4 ms at the device's size (29 frames per
second), 18.9 ms at 0.67 with MetalFX temporal and 13.4 ms at 0.5 with MetalFX spatial. Against the M4 Pro at 1080p
(below, unpaced) the phone has 1.5 times the pixels and the base costs 2.8 times as much, everything 3.1 times. The
probe grid costs 2.9 to 3.5 ms at every render scale (2048 probes of 64 rays every frame), so at half size it is the
largest part of the frame (about 4.4 ms of the 11.5 ms without lamp shadows, with its sampling). MetalFX on the A19:
temporal 3.6 ms at 0.5 and 4.0 ms at 0.67, spatial 1.1 ms at 0.5, the denoising upscaler 15.0 ms at 0.67 (it
replaces the engine's denoiser, 2.4 ms at that scale, and TAA). CPU p50 reads 2.0 to 2.2 ms in the half-size regions against 1.2 to
1.9 ms at the device's size, above the iPhone budget's 2.0 ms (set on the iPhone 18 Pro Max); not examined.

**The phone defaults** (since 2026-11-06, `RayTracingSection.cpp`; phones and tablets that trace: `App::mobile()` and
`gpu::Caps::rayTracing`): ray-traced sun shadows and occlusion, two lamp shadows and reflections, no probe grid (the
sky's ambient light; the panel turns it on), render scale 0.5 with MetalFX spatial upscaling, and 60 frames per second
while the section is current (the panel's "60 Hz" box, `App::setMaxFrameRate`). From the table: the probe grid is the
one effect whose cost does not shrink with the render scale; MetalFX temporal costs three times spatial here while
the engine's temporal denoiser already accumulates over frames; MetalFX denoising alone takes nearly a 60 Hz frame;
at 0.5 everything but the probes fits a 60 Hz frame with room for the clocks to drop. The desktop default (everything
at 0.67 with MetalFX temporal) is unchanged.

**The defaults held for a minute** (`scenarios/raytracing_phone_sustained.txt`, 18:50-18:56, the code of the phone
defaults): three 20 s regions at the display's rate from nominal, then, after a cool-down to nominal, three at 60 Hz.
GPU p50 is the span, `game` the busy time; ms:

| Region | Thermal | Frames / 20 s | Frame p50 / p99 | CPU p50 / p99 | GPU p50 | game | rays | denoise | reflections | upscale |
|---|---|---|---|---|---|---|---|---|---|---|
| display rate, 0-20 s | nominal | 2338 (117 per s) | 8.339 / 13.863 | 2.123 / 2.380 | 7.744 | 7.668 | 2.521 | 1.378 | 0.896 | 0.977 |
| display rate, 20-40 s | nominal -> fair | 2264 (113 per s) | 8.344 / 13.947 | 2.077 / 2.305 | 10.002 | 8.903 | 2.897 | 1.599 | 1.080 | 1.101 |
| display rate, 40-60 s | fair | 2160 (108 per s) | 8.362 / 14.346 | 2.047 / 2.320 | 10.832 | 9.351 | 3.068 | 1.659 | 1.099 | 1.178 |
| 60 Hz, 0-20 s | nominal | 1200 | 16.669 / 16.764 | 2.161 / 2.444 | 15.328 | 15.252 | 5.220 | 2.682 | 1.821 | 1.959 |
| 60 Hz, 20-40 s | nominal | 1200 | 16.669 / 16.751 | 2.168 / 2.475 | 15.323 | 15.247 | 5.211 | 2.675 | 1.827 | 1.958 |
| 60 Hz, 40-60 s | nominal | 1200 | 16.670 / 16.756 | 2.176 / 2.454 | 15.320 | 15.246 | 5.199 | 2.690 | 1.823 | 1.974 |

At the display's rate the defaults run at 117 frames per second cold (7.7 ms busy, just inside the 8.33 ms
interval); the phone turned fair after 34 s, and by the third region every range had grown by about 1.2 times (108
frames per second). At 60 Hz every frame made its interval (p99 16.76 ms), the three regions read the same and the
state stayed nominal for the whole minute, which ran second, from a warmer start. The GPU there lowers its clock to
fill the 16.7 ms: its 15.3 ms busy are time at that clock, not a cost. So ray-traced effects on phones target 60 Hz:
at 120 Hz even this reduced set holds the rate only while the phone is cool (the baseline's ten-minute run below
shows where it ends: about 1.6 times every range), at 60 Hz the A19 keeps its frame and its thermal state with the
same picture. The 60 Hz minute is one minute; a run of ten minutes at 60 Hz is open. The iPhone 18 Pro Max (A20 Pro)
has not run these defaults.

### The iPhone 17 baseline (stage 9 and 10 code)

2026-11-06 16:49-18:01, `5aea212` (the stage 10 code with the ARC fix), Release, `ios-xcode`, the iPhone 17 (A19 GPU,
GPU family Apple10, iOS 27.0.1, 6 CPU cores, 7672 MB), 1206 x 2622 in portrait, 120 Hz, `--audio-device null`,
launched through `devicectl`. The device tests (Metal API validation layer): SDL GPU 198 cases, 188 passed, 0
failed, 10 skipped (the 8 ray tracing and MetalFX cases and the two skipped everywhere on the phone); native 198
cases, 196 passed, 0 failed, 2 skipped. `gpu::Caps` as on the iPhone 18 Pro Max; the denoised scaler is supported.

The ray tracing benchmark on the native backend (`raytracing_benchmark.txt` of stage 10, 5 s regions, its second run,
from nominal; GPU span p50 / busy (`game`) in ms; the iPhone 18 Pro Max at 1320 x 2868 from the run of 14:59 below):

| Region | iPhone 17 frames / 5 s | iPhone 17 span / busy | iPhone 17 rays / denoise / upscale | iPhone 18 Pro Max frames / span |
|---|---|---|---|---|
| rt_shadows_ao | 499 | 13.242 / 10.239 | 4.262 / 1.906 / - | 601 / 8.015 |
| rt_shadows (no AO) | 601 | 8.291 / 8.081 | 3.120 / 0.791 / - | 601 / 6.215 |
| rt_hard_shadows | 601 | 6.701 / 6.662 | 2.739 / - / - | 601 / 6.128 |
| shadow_maps | 601 | 5.952 / 5.845 | - | 600 / 6.131 |
| temporal_1 | 311 | 18.354 / 16.495 | 4.629 / 2.252 / 6.825 | 493 / 10.817 |
| temporal_067 | 506 | 11.107 / 10.246 | 2.315 / 1.057 / 4.705 | 601 / 6.231 |
| temporal_05 | 597 | 7.252 / 7.208 | 1.333 / 0.573 / 3.696 | 601 / 6.104 |
| spatial_05 | 601 | 5.671 / 5.618 | 1.339 / 0.630 / 1.281 | 600 / 6.125 |
| bilinear_05 | 601 | 5.702 / 5.613 | 1.814 / 0.863 / - | 601 / 6.052 |

- Below the interval the GPU time settles on a plateau that follows the load: about 5.6 to 5.9 ms on the A19, 6.0 to
  6.2 ms on the A20 Pro; only the regions above it compare costs. The span exceeds the busy sum where the GPU is the
  limit (command buffers wait behind the previous frame): the busy sum is the measure of the work.
- RT shadows with occlusion at the native size: 120 Hz on the A20 Pro, about 100 frames per second cold on the A19.
  The rays cost 1.19 times the A20 Pro's per frame, about 1.4 times per pixel; MetalFX temporal 6.8 ms at scale 1
  (1.95 times) and 3.7 ms at 0.5, where it costs more than the rays and denoising it saves.
- The benchmark of every section (`--benchmark-all`, 5 s each, both backends, each run from nominal to fair): every
  section holds 120 Hz on both backends (GPU p50 5.6 to 5.9 ms) except the ray tracing section on the native
  backend, 419 frames in 5 s at GPU p50 18.5 ms (it ran last, warm). CPU p50 is about 1.5 times the iPhone 18 Pro
  Max's (0.9 to 1.3 ms against 0.63 to 0.91 ms; animation 1.8 against 1.45 ms).
- **Throttling** (ten 60 s regions of RT shadows and occlusion at the native size, from nominal, 17:49-18:00): minute
  1 at 87 frames per second with every range 1.18 times its cold time; from minute 2 about 65 frames per second at
  1.56 times, 1.65 times and 62 frames per second by minute 10. Every range (rays, denoise, hdr, taa, bloom) grows by
  the same factor and the CPU time does not: the clock, not the work. A last minute at 0.5 with MetalFX temporal ran
  at 90 frames per second, 1.63 times its cold time.
- **Thermal state** (read by a probe app then; the engine reads it since the same day): serious after one 65 s ray
  tracing benchmark, 5 to 11 minutes back to nominal while idle; fair before any demo had run, and once rising from
  fair to serious with no Quench app running (cause not known: background work of a freshly connected phone, or
  charging; nothing on the phone can be seen from here).

### Stage 11 on the Mac, and earlier runs

Stage 11 (ray-traced lighting), 2026-11-06 17:20-18:00, Release, Mac, Apple M4 Pro, `--frame-size 1920x1080`,
`--audio-device null`, on the screen. The display ran at 60 Hz in this session. Its iPhone numbers are
[above](#stage-11-on-the-iphone).

**The cost of each effect.** `raytracing_benchmark.txt` on the native backend with `--present-mode immediate`
(17:47-17:49, one run; the build before the hard-shadow change below): unpaced, the GPU at its full clock. With vsync
at 60 Hz the GPU clock follows the load and the ranges of light regions read up to four times longer (`rays` of
`rt_shadows_ao`: 8.02 ms at 60 Hz, 1.88 ms unpaced), so the 60 Hz runs (17:28-17:29, stage 10 and stage 11, one each)
give no costs. Offscreen perf regions were tried first and dropped: their GPU timings lagged behind the region or
were missing (9 of 19 regions without a sample in a per-process run; explained later the same day: a GPU-bound
offscreen frame replaced the oldest frame's fence unread, and every frame repeated the last time read, so a region
could start with the previous region's times; both fixed, see [The benchmark](#the-benchmark)). GPU busy time
per range, medians over 4 s regions, in milliseconds; `game` is the renderer's whole frame, "frame" the interval
between frames (unpaced: the throughput):

| Region | Render size | Frames | Frame p50 | CPU p50 | game | rays | denoise | probes | reflections | indirect | hdr | taa | upscale |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Shadow maps (3 cascades) | 1920 x 1080 | 1371 | 3.24 | 0.606 | 2.106 | | | | | | 1.144 | 0.469 | |
| RT hard shadows (1 ray) | 1920 x 1080 | 1182 | 3.54 | 1.091 | 2.580 | 0.625 | 0.817 | | | | 0.481 | 0.298 | |
| RT shadows (2 rays, soft) | 1920 x 1080 | 1159 | 3.71 | 1.078 | 3.026 | 0.994 | 0.825 | | | | 0.436 | 0.299 | |
| RT shadows + AO (2 rays each): the base below | 1920 x 1080 | 1052 | 3.80 | 1.052 | 4.122 | 1.877 | 0.833 | | | | 0.423 | 0.300 | |
| base + 2 lamp shadows | 1920 x 1080 | 540 | 7.20 | 1.215 | 7.652 | 4.444 | 1.666 | | | | 0.431 | 0.298 | |
| base + reflections | 1920 x 1080 | 901 | 4.48 | 1.156 | 4.803 | 1.670 | 0.822 | | 0.861 | | 0.427 | 0.298 | |
| base + probe grid | 1920 x 1080 | 671 | 5.87 | 1.166 | 6.587 | 1.700 | 0.826 | 1.110 | | | 1.565 | 0.302 | |
| base + per-pixel indirect | 1920 x 1080 | 472 | 8.52 | 1.539 | 8.905 | 1.885 | 0.831 | 1.117 | | 3.584 | 0.464 | 0.310 | |
| all (lamps, reflections, probes), temporal denoiser | 1920 x 1080 | 380 | 10.57 | 1.624 | 11.038 | 4.726 | 1.666 | 1.110 | 0.910 | | 1.578 | 0.300 | |
| all, spatial denoiser | 1920 x 1080 | 416 | 9.70 | 1.499 | 10.360 | 4.494 | 1.220 | 1.112 | 0.697 | | 1.573 | 0.354 | |
| all, MetalFX temporal at 0.67 | 1286 x 724 | 591 | 6.80 | 1.135 | 7.335 | 2.111 | 0.774 | 1.118 | 0.471 | | 0.766 | | 1.268 |
| all, MetalFX denoising at 0.67 | 1286 x 724 | 355 | 10.91 | 0.446 | 14.529 | 2.096 | | 1.212 | 0.346 | | 0.797 | 0.044 | 6.232 |
| all, MetalFX denoising at 0.5 | 960 x 540 | 447 | 8.96 | 1.201 | 9.468 | 1.696 | | 1.484 | 0.268 | | 0.616 | 0.040 | 4.324 |
| base, MetalFX temporal at 1 | 1920 x 1080 | 789 | 4.99 | 0.910 | 5.559 | 1.654 | 0.818 | | | | 0.407 | | 1.757 |
| base, MetalFX temporal at 0.67 | 1286 x 724 | 1179 | 3.21 | 0.557 | 3.609 | 0.940 | 0.385 | | | | 0.206 | | 1.272 |
| base, MetalFX temporal at 0.5 | 960 x 540 | 1195 | 3.46 | 0.600 | 2.629 | 0.603 | 0.226 | | | | 0.140 | | 1.247 |
| base, MetalFX spatial at 0.5 | 960 x 540 | 1170 | 3.50 | 1.149 | 2.381 | 0.770 | 0.322 | | | | 0.225 | 0.118 | 0.392 |
| base, bilinear at 0.5 | 960 x 540 | 1265 | 3.15 | 1.003 | 1.847 | 0.766 | 0.318 | | | | 0.243 | 0.113 | |
| Path tracer (`path` 10.919) | 1920 x 1080 | 355 | 11.30 | 1.265 | 11.982 | | | | | | | | |

What each effect adds to the base at 1080p (the `game` difference): two lamp shadows +3.5 ms (their rays +2.6 ms,
their filter +0.8 ms); reflections +0.7 ms (0.86 ms in their range with the filter); the probe grid +2.5 ms (1.1 ms
for 2048 probes of 64 rays, and 1.1 ms more in the main pass, which samples the probe atlases per pixel); per-pixel
indirect light +4.8 ms (3.6 ms of rays and filter over the probes). Everything the demo turns on by default costs
11.0 ms at full size (about 95 frames per second), 7.3 ms at render scale 0.67 with MetalFX temporal and the
engine's denoiser. MetalFX's denoising upscaler costs 6.2 ms at 0.67 and 4.3 ms at 0.5 (it replaces the engine's
denoiser, 1.7 ms here, and TAA): on the M4 Pro it is the dearest way to the same frame, so the demo's default is
MetalFX temporal with the engine's denoiser (the MetalFX denoising row at 0.67 also read 2.9 ms of `bloom` against
0.2 ms elsewhere and 0.45 ms of CPU p50, the row at 0.5 clean. Offscreen after stage 12, three regions of MetalFX
denoising at 0.67 read `bloom` 0.231 and 0.317 ms, `upscale` 6.29 to 7.50 ms: not reproduced there. In a window
again on 2026-11-07 the region at 0.5 read it instead, and offscreen at four scales it did not, also not when a
barrier made the upscaler's end marker wait for all its work: [On the screen after the bug
packages](#on-the-screen-after-the-bug-packages)). MetalFX temporal costs 1.25 to 1.76 ms at a 1080p output, spatial 0.39 ms. The path tracer takes 10.9 ms
per frame. The temporal denoiser also filtered hard shadows without occlusion (0.82 ms in `rt_hard_shadows`); it no
longer does (`19d9a6f`, not measured again). The frame's GPU time (its command buffer's start to end) is not in the
table: unpaced, it is longer than the busy ranges in the heavier regions (6.34 against 4.21 ms in the base, 19.4
against 11.1 ms with everything). The frame interval there was 10.57 ms (380 frames in 4 s), shorter than the span of
19.4 ms: the GPU starts a command buffer while the one before it still runs, so a span includes time the GPU spent
on the frame before (offscreen, three frames in flight, the same: [The benchmark](#the-benchmark)). The busy times
per range are the costs.

**SDL GPU before and after** (the 5 % rule): `db5f4cd` (stage 10's code with the quick-tier change, its Release
binaries in `/tmp/quench-base`) against the stage 11 tree, interleaved, vsync at 60 Hz, medians of the runs:

| Run | Runs | CPU p50 | CPU p99 | GPU p50 |
|---|---|---|---|---|
| hello | 10 + 10 | 0.398 / 0.511 (+28.2 %) | 0.681 / 0.681 (-0.1 %) | 0.547 / 0.548 |
| hello, the first five pairs (`db5f4cd` first) | 5 + 5 | 0.355 / 0.443 (+24.8 %) | 0.667 / 0.679 (+1.8 %) | 0.547 / 0.547 |
| hello, the last five pairs (stage 11 first) | 5 + 5 | 0.529 / 0.525 (-0.8 %) | 0.683 / 0.703 (+2.9 %) | 0.548 / 0.548 |
| demo hdr (focused) | 5 + 5 | 1.199 / 1.159 (-3.3 %) | 1.580 / 1.552 (-1.8 %) | 10.754 / 10.913 |
| demo assets (focused) | 5 + 5 | 1.321 / 1.306 (-1.1 %) | 1.760 / 1.840 (+4.5 %) | 12.228 / 12.554 |
| demo post (focused) | 5 + 5 | 1.169 / 1.207 (+3.3 %) | 1.642 / 1.648 (+0.4 %) | 11.721 / 11.948 |

hello's CPU p50 is bimodal per run in this session in both builds (0.153 to 0.535 ms; `db5f4cd` 0.275 to 0.535,
stage 11 0.153 to 0.534) and rose through the session; its p99 is steady (0.654 to 0.739) and the same in both
builds. With the order reversed the two builds read the same; hello does not use `quench_render`, the SDL GPU
backend did not change, and the GPU layer's front end changed only in functions hello does not call (refits,
compaction, the denoised upscaler). The complete demo benchmark, 2 + 2 runs per order (17:24-17:38),
gave CPU p50 between -12.2 % (materials) and +21.1 % (post) per section, with sections flipping sign with the order
(shadows +15.3 % and -1.9 %, animation +14.6 % and -3.1 %); hdr, assets and post read above 5 % in both orders and
were measured again on their own (one section per process, five pairs, order alternating, 17:56-18:00): the focused
rows, within 5 %. The ray tracing section's SDL GPU region (its fallbacks) read +4.8 % and +8.8 %; its scene has
about ten more objects and two lamps since stage 11, so that region measures different content. The GPU p50 at 60 Hz
reads 1.5 to 2.7 % higher in the new build in the focused rows (and in most sections of the complete runs): the
64-byte vertex and the standard surface's normal and emissive maps are the likely part, at a clock that follows the
load; not measured at full clock.

The iPhone baseline of the stage 9 and 10 code, 2026-11-06 14:55-15:05: `48964e1` (0.8.0 with the demo's Graphics
panel) built by `ios-xcode` with ARC forced on (`-DCMAKE_XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC=YES`; without it the
native backend crashed there, [Platforms](platforms.md#native-metal-backend)), Release, vsync, iPhone 18 Pro Max
(A20 Pro GPU, iOS 27), native size 1320 x 2868 (portrait), 120 Hz, `--audio-device null`, launched through
`devicectl`, no validation layer. The benchmark of every section (`--benchmark-all`, 5 s each), one run per backend:

| Section | Frames SDL GPU / native | CPU p50 SDL GPU / native | CPU p99 SDL GPU / native | GPU p50 SDL GPU / native |
|---|---|---|---|---|
| materials | 601 / 600 | 0.872 / 0.909 | 1.354 / 1.224 | 6.063 / 6.034 |
| shadows | 601 / 601 | 0.697 / 0.719 | 0.988 / 0.965 | 6.095 / 6.037 |
| sky | 601 / 601 | 0.662 / 0.665 | 0.908 / 0.859 | 6.099 / 6.085 |
| post | 600 / 600 | 0.672 / 0.708 | 0.956 / 0.994 | 6.031 / 6.000 |
| hdr | 601 / 600 | 0.633 / 0.781 | 0.854 / 1.073 | 6.018 / 6.048 |
| physics | 600 / 600 | 0.688 / 0.771 | 0.938 / 1.146 | 6.013 / 6.047 |
| input | 600 / 600 | 0.672 / 0.671 | 0.919 / 0.829 | 6.068 / 6.024 |
| assets | 600 / 601 | 0.802 / 0.815 | 1.129 / 1.196 | 6.041 / 5.997 |
| audio | 601 / 601 | 0.680 / 0.705 | 0.951 / 0.896 | 6.042 / 6.063 |
| animation (51 skinned characters) | 601 / 600 | 1.465 / 1.435 | 1.773 / 1.710 | 5.999 / 6.045 |
| raytracing | 601 / 600 | 1.038 / 1.011 | 1.374 / 1.365 | 6.135 / 8.069 |

All values in milliseconds. Frame p50 8.33 ms in every section, frame p99 at most 9.02 ms (SDL GPU animation),
frame max at most 13.66 ms (native hdr). On SDL GPU the ray tracing section runs its fallbacks (shadow maps). Below
about 6 ms the phone's GPU time does not go down: every section reads 6.0 to 6.2 ms whatever its work (also at
render scale 0.5 below), probably because the GPU clock follows the load (not verified), so the phone's GPU times
compare only above that floor.

The ray tracing section on the native backend (`scenarios/raytracing_benchmark.txt`, 5 s regions). The first four
rows come from the first run (14:59); from MetalFX temporal on, that run switched sections (probably real touches;
the build had no input isolation), so those rows come from a second run of the same regions (15:02-15:03) that
checked the section after each one. GPU busy time per range:

| Region | Render size | Frames | CPU p50 | GPU p50 | accel | rays | denoise | shadows | hdr | taa | upscale |
|---|---|---|---|---|---|---|---|---|---|---|---|
| RT shadows (2 rays, softness 1) + RT AO (2 rays) | 1320 x 2868 | 601 | 0.524 | 8.015 | 0.180 | 3.574 | 1.381 | | 0.811 | 1.252 | |
| RT shadows, no AO | 1320 x 2868 | 601 | 0.502 | 6.215 | 0.245 | 2.525 | 0.555 | | 0.817 | 1.254 | |
| RT hard shadows (1 ray, softness 0) | 1320 x 2868 | 601 | 0.521 | 6.128 | 0.285 | 2.595 | | | 0.999 | 1.351 | |
| Shadow maps (3 cascades) | 1320 x 2868 | 600 | 0.565 | 6.131 | | | | 0.453 | 2.587 | 1.820 | |
| RT + AO, MetalFX temporal, scale 1 | 1320 x 2868 | 493 | 1.283 | 10.817 | 0.294 | 3.578 | 1.383 | | 0.836 | 0.187 | 3.503 |
| RT + AO, MetalFX temporal, scale 0.67 | 884 x 1922 | 601 | 1.294 | 6.231 | 0.186 | 1.549 | 0.638 | | 0.376 | 0.088 | 2.761 |
| RT + AO, MetalFX temporal, scale 0.5 | 660 x 1434 | 601 | 1.356 | 6.104 | 0.278 | 1.228 | 0.548 | | 0.333 | 0.092 | 2.836 |
| RT + AO, MetalFX spatial, scale 0.5 | 660 x 1434 | 600 | 1.319 | 6.125 | 0.358 | 1.573 | 0.696 | | 0.428 | 0.624 | 1.309 |
| RT + AO, bilinear, scale 0.5 | 660 x 1434 | 601 | 1.072 | 6.052 | 0.513 | 2.358 | 0.865 | | 0.537 | 0.729 | |
| RT + AO again, at the end of the second run | 1320 x 2868 | 601 | 1.005 | 7.870 | 0.178 | 3.425 | 1.384 | | 0.810 | 1.266 | |

All values in milliseconds. Ray-traced shadows with occlusion cost 8.0 ms of GPU time at the native size: over the
phone's budget of 7.5 ms (below), but within the 8.33 ms of the 120 Hz display (601 frames, frame p99 8.66 ms).
Without occlusion, with hard shadows and with shadow maps the frame sits at the 6 ms floor, so their differences
cannot be read there. MetalFX's temporal scaler costs about 3.5 ms at the phone's output size; at scale 1 the frame
grows to 10.8 ms and the section drops to about 99 fps (493 frames in 5 s, frame p99 19.19 ms); at 0.67 and 0.5 it
stays at 120 Hz. The spatial scaler costs 1.3 ms. The last row, about 4 minutes after the first, shows no thermal
drift (7.870 against 8.015 ms). The CPU time differs between the two runs (0.50 to 0.57 ms in the first, 1.0 to
1.36 ms in the second); not looked into, both within the budget. A few frames of each path under the Metal API
validation layer (15:06): no validation message.

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
