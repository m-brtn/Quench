# Quench, the next release (draft)

Not released: no version or tag yet (the version in `project()` stays 0.9.0). Stage 12 of the
[roadmap](../roadmap.md): area lights, anisotropic GGX and the demo's tunnel. This file becomes
`v<version>.md` when the user asks for the release; what was verified is in the devlog of 2026-11-06 and in
[Performance](../performance.md#the-tunnel).

## What changed

- Area lights in `quench_render`: rect, disk and tube lights, shaded by linearly transformed cosines in the raster
  and the ray hits, with soft ray-traced shadows, sampled by the path tracer, their shapes drawn in the scene
  ([Render module](../render.md#area-lights)). The fitted LTC table and integrals come from selfshadow/ltc_code
  (BSD-2-Clause; `THIRD_PARTY_LICENSES`, [Dependencies](../dependencies.md)).
- Anisotropic GGX in the standard surface, the ray hits and the path tracer
  ([Render module](../render.md#anisotropic-ggx)).
- The demo's section 12, "Tunnel" ([Engine demo](../demo.md#sections)), and since the user's request after stage
  12 the tunnel in motion: moving and turning lamps with their fixtures, a camera that drives a loop through the
  tunnel, a pause and a free camera; phones without ray tracing render the tunnel at three quarters of the size
  ([Performance](../performance.md#the-tunnel-in-motion)).
- Surface shader variants: `surface_basic_fs`, the surface shader without the area light and anisotropy paths,
  draws materials without anisotropy in frames without area lights. Without it the standard surface was 5 to 8 %
  slower in such frames than before stage 12 (unpaced, M4 Pro); with it within 0.6 %
  ([Performance](../performance.md#the-tunnel)).
- `RenderStats::localShadowChanges`: the lights that took another light's ray-traced shadow channel this frame.
- Perf regions report the process's CPU time per frame and the share of it on performance cores (Apple platforms;
  `perf_process_cpu`, `perf_cpu_performance_share`, on the `perf:` line and in the demo's `benchmark.json`): the
  frame's CPU time read 0.29 or 1.2 ms for the same work on the M4 Pro, by where the scheduler ran it
  ([Performance](../performance.md#cpu-time-and-the-cores)).
- The renderer's `accel` timing range has two ranges inside it: `blas` (the bottom levels' builds, refits and
  compactions; absent in a frame without them) and `tlas` (the top level's build or refit).
- Offscreen perf regions also time GPU-bound frames on the GPU (before, they had no GPU sample and no times per
  range); unpaced offscreen A/B runs of GPU costs need no window now.
- The demo's switch cycle checks the memory to below 64 MB of growth (was 32): the footprint moves within about
  60 MB from round to round on the native backend.

## Fixes

- **Memory growth on SDL GPU's Vulkan driver:** a fence released after it signalled could keep its command buffer,
  and every texture and buffer it used, in flight for good (a reference counting issue in SDL's Vulkan driver), so
  cycled render targets were made anew every frame: the demo's assets section reached 4.5 GB on Vulkan through
  MoltenVK. The GPU layer now waits on a signalled fence (which does not block) before it releases it: 676 MB, flat.
  The demo's switch cycle now also runs on Vulkan (`batch.txt`; `batch_metal.txt` is gone): +34 MB in its third
  round, below the bound of 64 MB, where it grew by 459 MB before.
- **Perf regions count each frame's GPU time once, in its own region:** before, every frame repeated the last GPU
  time read (a frame read late, twice, or one of the previous region's). New values `perf_gpu_samples` and
  `perf_gpu_lost`; hello's batches check that no timed frame is lost (`perf.txt`, `hello_perf_<backend>`).
- **Perf regions count each frame's time in its own region:** a frame's time (from its start to the next frame's
  start) went into the region of the next frame, so a region's first sample was the frame before it: a capture
  just before `perf_begin` put its PNG encoding into the region (708 ms on the iPhone 17, 610 ms on the M4 Pro at
  1206 x 2622), and the region's last frame was not counted. The capture still stalls its own frame.

## Changes for games

What a game on 0.9.0 changes or checks when it moves to this release. Neither game uses `quench_render` yet.

- **`render::LightType`** has `Rect`, `Disk` and `Tube` after `Spot`: a `switch` over it without a `default` warns
  (`-Wswitch`) until it handles them.
- **`render::LightDesc`** has new members after `sourceRadius`: `size` (`glm::vec2`), `tangent` (`glm::vec3`),
  `twoSided`. Designated initializers in the old order compile unchanged. For an area light `color` times
  `intensity` is the radiance of its surface, not an irradiance; `direction` is a rect's or disk's normal and
  `range` is measured from the shape.
- **Area lights draw their shape:** the scene adds an instance, a material and a mesh for each one, outside the
  scene's handles and counts; `RenderStats::draws` includes them. At most 8 of a frame's 16 lights are area lights
  (the nearest).
- **`render::MaterialDesc`** has `anisotropy` and `anisotropyRotation` after `name` (default 0: isotropic, as
  before). Anisotropy needs vertex tangents (`computeTangents` or cooked models).
- **`render::RenderStats::areaLights`** and **`localShadowChanges`** are new (`localShadowChanges` before
  `renderWidth`: designated initializers in the old order still compile; a positional aggregate initialization
  would fill the members after it one off); `diskMesh` and `cylinderMesh` are new in `MeshBuilder.h`.
- **Shaders built on `quench/Surface.hlsli`:** `MaterialUniforms` gained `float4 shading` at its end (x the
  anisotropy, yz its rotation, w the area light shapes); `LocalLight` gained `float4 shape`, its `cone` and
  `direction` carry area light values, and `LightUniforms::count.y` counts the area lights. The fragment sampler 12
  holds the area lights' table: a custom surface must not use it. `QuenchSurface` has two new fields,
  `anisotropy` and `anisotropyDirection`: a custom surface that fills a `QuenchSurface` itself sets `anisotropy` to
  0 (or its own value) and `anisotropyDirection` to its tangent. A custom surface gets the area lights without
  changes. It also builds `surface_basic_fs:fragment` and passes it as **`SurfaceDesc::basicFragmentShader`**
  (new, after `fragmentShader`) to skip those paths where a frame does not need them; `QUENCH_SURFACE_FULL` is 0
  in that entry, for code of its own around area lights or anisotropy. Without it nothing changes.
- **`app::PerfReport::gpuSamples`** (and the perf line's sample count) is the number of frames whose GPU time was
  read, a few less than `frames` (the frames in flight at the end), where it equalled `frames` before; GPU times of
  a region come only from its own frames. **`PerfReport::processCpu`** and **`cpuPerformanceShare`** are new, at its
  end. The `perf:` line has `, process <ms> per frame (<n>% on performance cores)` after the CPU percentiles on
  Apple platforms: a parser of the line that expects `; gpu ms` right after `p99 <ms>` needs to allow it.
- No new dependency library: the LTC data is code and a table in `quench_render` (BSD-2-Clause, with the paper
  reference its licence asks for).
