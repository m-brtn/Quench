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
- Perf regions report the frames' wait for the swapchain texture (offscreen, for the oldest frame in flight), the
  present mode in use and the display's rate, so a run with `--present-mode immediate` shows whether it was paced
  ([Performance](../performance.md#the-benchmark)).
- The renderer's `accel` timing range has two ranges inside it: `blas` (the bottom levels' builds, refits and
  compactions; absent in a frame without them) and `tlas` (the top level's build or refit).
- Offscreen perf regions also time GPU-bound frames on the GPU (before, they had no GPU sample and no times per
  range); unpaced offscreen A/B runs of GPU costs need no window now.
- The demo's switch cycle checks the memory to below 64 MB of growth (was 32): the footprint moves within about
  60 MB from round to round on the native backend.
- ASTC 4x4 textures (stage 6 remaining work): `quench_cook texture --format astc` encodes them through astc-encoder
  5.7.1 (Apache-2.0, in the cook only), `quench_cook_assets(TEXTURE_FORMAT auto)` cooks ASTC in iOS builds and BC7
  elsewhere, and `SceneAssets` samples them where `gpu::Caps::astcTextures` is set
  ([Assets](../assets.md#cooked-formats)). PSNR 54 to 57 dB on the cook's test images (BC7: 44 to 51 dB); the
  demo's Assets section ran with them in the iOS Simulator and on the iPhone 17.
- Asset hot reload on a device (stage 6 remaining work): development builds read assets from `asset_overlay/` in
  the user directory first (`fs::makeOverlayBackend`; CMake option `QUENCH_ASSET_OVERLAY`, on without `NDEBUG`),
  and `tools/push_assets.sh` copies files whose content changed into it with `devicectl`
  ([Assets](../assets.md#hot-reload-on-a-device)). On the iPhone 17 a pushed texture was reloaded about a second
  later. The demo's Assets section turns hot reload on with `--hot-reload` (before, only its switch did).
- Shader hot reload on desktops (stage 6 remaining work): with `--hot-reload` an edit of an HLSL source or of a
  file it includes is compiled again with the build's commands (`SDL_CreateProcess`, a manifest per set from
  `quench_add_shaders()`), and the pipelines that use the set are created again behind the same handles; after a
  compile error the old code stays ([App shell](../app.md#shader-hot-reload)). `gpu::Device::enableShaderHotReload`,
  `pollShaderHotReload` and `shaderReloadStatus`; scenario values `shader_reloads`, `shader_reload_failures`,
  `shader_reload_pipelines`. Checked on the Mac's three backends; the renderer's `render_frame` set (15 entries)
  compiled again in 3.7 to 4.2 s while the demo kept rendering.
- Ogg Vorbis sounds and streams (stage 7 remaining work): `quench_audio` decodes `.ogg` files through stb_vorbis 1.22
  from miniaudio's archive (MIT or public domain; no new download), as sounds in memory, streams and music, and
  `quench_cook sound` and `quench_cook_assets(SOUNDS)` accept them ([Audio](../audio.md#streams-and-music)). A
  streamed Vorbis loop needs no length (miniaudio reports none for it there) and gives exactly the frames of the file
  decoded in memory every time round; the test loop decodes to its 48000 frames at 38 to 39 dB signal to error
  against its source; also under Node.js.
- Reverb buses with sends (stage 7 remaining work): `BusDesc::reverb` makes a bus a Freeverb reverb (verblib 0.5
  from miniaudio's archive, MIT-0 or public domain; no new download) whose output replaces its input; sounds send
  to it with `PlayParams::sendBus` and `sendLevel` or `AudioSystem::setSend`, and `setReverb` changes the room size,
  damping and width ([Audio](../audio.md#reverb)). Rendered on the audio thread without allocating (counted: 0 in
  600 renders), bit for bit the dry mix when nothing reaches it, idle under -100 dB; RT60 0.67, 1.08 and 2.43 s at
  room sizes 0.2, 0.5 and 0.8; about 1 µs per millisecond of audio for one reverb on the M4 Pro. The demo's audio
  section has a reverb switch and a room size slider, a mix check (`audio_reverb.txt`) and a check mode item for
  listening.
- HRTF for headphones (stage 7 remaining work): with `AudioSettings::hrtf` or `AudioSystem::setHrtf(true)` spatial
  sounds are filtered by head-related impulse responses instead of panned, at most `AudioSettings::hrtfVoices` (8)
  at once, the others panned; switching crossfades over 10 ms ([Audio](../audio.md#hrtf)). The responses are
  SADIE's KU100 set as Resonance Audio distributes it (Apache-2.0; `THIRD_PARTY_LICENSES`), reduced by
  `tools/make_hrtf_table.py` to 386 directions of 128 taps with per-ear delays (`src/audio/HrtfTable.inc`, 198 KB in
  the binary), interpolated between directions and convolved directly on the audio thread without allocating. Noise
  2 m to the side: 15.4 dB between the ears and 39 frames of delay, as in the source data; bit for bit the panned
  mix when off; about 1.2 µs per HRTF voice and millisecond of audio on the M4 Pro (32 voices through it: 4% of a
  core). The demo's audio section has an HRTF switch, a mix check (`audio_hrtf.txt`) and a check mode item for
  listening in headphones. Nobody has listened to it.
- 2D blend spaces (stage 8 remaining work): a graph state's `blend2d` blends up to 16 clips placed at points of two
  float parameters, weighted by gradient band interpolation (freeform Cartesian), the clips in phase with a per-point
  `phase` offset; also in 1D trees ([Animation](../animation.md#graphs-state-machines)). On a grid of the demo's
  six points the weights stay in 0..1, sum to 1 within 1.2e-7, change by at most 0.0018 per mm and are mirror
  symmetric. The demo's mannequin has walk back and strafe clips, and its Animation section a view "2D blend space"
  with a character steered by its velocity while facing one way (`animation_blend2d.txt`: its root motion within
  0.3 to 11 degrees of the asked direction; a golden capture; a check mode item). Nobody has looked at it in
  motion.
- Animation LOD (stage 8 remaining work): `AnimationSystem::setLod` and `setLodView` sample the clips of characters
  small on the screen every 2nd or 4th frame (thresholds of screen size with hysteresis, staggered over the frames),
  interpolated between updates (computed a period ahead, so not late) or held, with foot placement and look-at off
  below a size, root motion spread over the frames ([Animation](../animation.md#animation-lod)). Off by default; with
  it off the update is bit for bit what it was. The renderer copies the skinned vertices of an instance whose palette
  did not change instead of skinning it again (`RenderStats::skinnedReused`). On the demo's crowd of 256 (M4 Pro,
  offscreen) from 45 m: 65 of 260 characters sampled per frame; the update 0.31 ms on one thread, 0.26 interpolated,
  0.14 held; the skinning pass 1.75 ms GPU, 0.46 held. The demo's Animation section has the option, a scripted check
  (`animation_lod.txt`) and a check mode item. Nobody has looked at it in motion.
- Dual quaternion skinning (stage 8 remaining work): `render::Skinning::DualQuaternion` per instance
  (`InstanceDesc::skinning`, `Scene::setSkinning`) or per skin of a cooked model (`SceneAssets::setSkinning`), linear
  the default ([Animation](../animation.md#gpu-skinning)). A bar twisted by 180 degrees keeps its full width (11 of
  11 pixels at the middle, linear 0); rigid motions and translations render as with linear skinning. On the demo's
  crowd of 256 (M4 Pro, offscreen) it costs 0.06 ms of frame CPU and nothing measurable on the GPU. The demo's
  Animation section has a view "Skinning" (two characters with a forearm twisted by 170 degrees, linear against dual
  quaternions), an option for the other characters, a scripted check with golden captures
  (`animation_skinning.txt`) and a check mode item. Nobody has looked at it in motion.
- Retargeting at cook time (stage 8 remaining work): `quench_cook animation IN OUT --skeleton TARGET --retarget
  MAP.json` and `quench_cook_assets(... ANIMATIONS ... SKELETON target.gltf RETARGET map.json)` (output
  `<path>.<target>.qanim`) cook clips for a skeleton with other joint names, proportions and rest pose: a JSON bone
  map by name, the source's model-space rotations with the rest poses and the bones' directions corrected, the root's
  and hips' translations scaled by the hips' height ratio (or a given scale); unmapped joints keep their rest
  ([Animation](../animation.md#retargeting)). The demo's mannequin's walk on its second character (legs 1.15 times
  as long, other names, a T-pose, an extra spine joint), through the runtime: feet within 5.1 mm of 1.15 times the
  mannequin's in world space, hands within 0.8 mm of 1.1 times from the shoulders, planted feet moving at most
  0.46 mm per frame (the mannequin's 0.44 mm). The demo's Animation section has a view "Retargeting" (the two walking
  abreast, the camera following them), a scripted check with golden captures (`animation_retarget.txt`) and a check
  mode item. Nobody has looked at it in motion.
- Blend shapes (stage 8 remaining work): glTF morph targets (position, normal and tangent offsets, names from
  `extras.targetNames`, default weights from the node or mesh) are cooked into `.qmodel` version 4 and applied by
  the renderer's skinning pass before the joints, also for meshes without joints
  ([Render module](../render.md#blend-shapes), [Assets](../assets.md#cooked-formats)). A game sets the weights per
  instance (`Scene::setMorphWeights`) or by name for a cooked model (`SceneAssets::setMorphWeights`). With every
  weight 0 the image is the one of the mesh without blend shapes, bit for bit; with weights (also negative and above
  1) the one of the mesh morphed on the CPU, 0 pixels more than 2 levels apart, on Metal, Vulkan through MoltenVK
  and the native backend, also before linear and dual quaternion skinning; motion vectors, culling bounds and
  ray-traced shadows follow the weights. The demo's Animation section has a view "Blend shapes" (a swaying blob
  with four shapes on sliders), a scripted check with golden captures (`animation_blendshapes.txt`) and a check
  mode item. Nobody has looked at it in motion.
- Blend shape weights from clips (stage 8 remaining work): clips carry named float channels (`ClipDesc::channels`,
  ozz float tracks), the cook turns glTF `weights` animations into them (one per target, named like the model's
  blend shapes), and `AnimationSystem::addChannels` / `channels` give a game their values after each update,
  blended like the pose (blend trees and spaces, cross-fades, override and additive layers, animation LOD; a clip
  without a channel leaves its weight to the rest value). Checked by numbers: exact at and between keys, the blends'
  expected values within 1e-5, a newer file version refused; cooked linear, step and cubic weights through the
  runtime within 3e-3; LOD interpolation within 1e-5 of an update every frame. The demo's blob takes its sway
  clip's squash and stretch (a "Weights from the clip" option, on by default; the sliders add to them), with 10 more
  checks in its scripted check; its golden capture (the sliders' shapes, the clip's weights off) did not change.
  Nobody has looked at it in motion.
- Normal maps in ray hits and the path tracer (stage 11 and 12 remaining work): traced reflections, indirect light,
  the probe grid and the path tracer read a material's normal map (texture 2) in the raster's tangent frame
  ([Render module](../render.md#ray-traced-lighting)). In `renderer_ray_hits_read_normal_maps` a wall whose map
  turns it toward the sun is 4.07 times as bright as a plain one in the raster, 3.99 times in its traced mirror
  image and 3.80 times in the path tracer (1.00 and 0.97 before). The tracing passes have variants with the normal
  maps, used only in frames where an instance has one: frames without normal maps cost what they did (the demo's
  ray tracing section, offscreen at 1080p on the M4 Pro: reflections 0.901 / 0.903 ms, the path tracer 12.70 /
  12.72 ms); in the tunnel, which has them, reflections cost 0.33 ms (5 %) and the path tracer 1.2 ms (4 %) more.
  The tunnel's two native golden images changed (the crates' studs in the puddle).
- The probe grid in turns, for phones (stage 11 remaining work): `RenderSettings::probeUpdatePeriod` (1, 2, 4 or 8,
  default 1) traces that fraction of the 2048 probes per frame, every probe once in n frames, spread evenly over
  the grid ([Render module](../render.md#ray-traced-lighting)). Probes that come into a scrolling grid are left out
  of the shading until their turn (irradiance alpha 0), and a grid that jumps traces every probe. On the M4 Pro
  (offscreen, 1080p, native backend) the probes cost 1.16 ms every frame and 0.68, 0.43 and 0.32 ms in 2, 4 and 8
  turns; light settles to within 10 % of a change in 70, 166, 276 and 629 frames
  ([Performance](../performance.md#probe-grid-in-turns)). The default frame is the one before, bit for bit; the
  `probes` range reads 0.01 ms more. The demo's Graphics panel has "Probe updates" (`set probe_updates`);
  `scenarios/raytracing_phone_probes.txt` measures it on a phone (not run yet).

## Fixes

- **`JobSystem::wait()` could return before the job left `pendingCount()`:** the job was marked done before it was
  counted out, so a `pendingCount()` right after waiting for every job read 1 now and then (`core.jobs_run_and_wait`
  under ThreadSanitizer and load: 13 of 480 runs). `wait()` now returns once the job is also counted out; 0 of 1200
  runs since.

- **Xcode builds put changed assets into the app one build late:** the merged `assets/` folder was a folder
  reference, which Xcode checks before the build that changes it: the demo's textures cooked again as ASTC were in
  the merged folder after a build and in the app only after the next. A script phase after the target's other
  phases now copies the changed files into the bundle and signs it again when it copied something (with Xcode's
  identity and entitlements); a texture changed and changed back reached the app in the build that cooked it, with
  a valid signature ([App shell](../app.md#ios-runs)). `tools/package_demo.sh` no longer builds the merged folder
  in an `xcodebuild` of its own first.

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
- **Motion vectors through the holes of a moving cut-out object:** its motion covered its holes, so TAA, the
  temporal denoiser and MetalFX reprojected what is seen through them with the object's motion. Moving cut-out
  instances now write their motion only where the frame's depth is their own (`velocity_cutout_fs`, without a depth
  target): in `renderer_moving_cutout_motion_vectors` the motion vector in a hole of a panel moving 0.1 m per frame
  read 251 of 255 in a temporary view of the motion vectors before, 0 after, on all three backends.
- **The demo's `menu.txt` in the compact layout:** its clicks and touches on the section list failed on phones and
  in the iOS Simulator (7 of 39), where the list is a sheet; the scenario now opens the sheet first (35 of 35 in
  the Simulator and in the Mac's batches). Scripted gamepad input works in the Simulator now (hello's `input.txt`
  14 of 14, the demo's `physics.txt` 18 of 18 and `input.txt` 22 of 22).
- **The demo's tunnel motion capture on the native backend failed now and then** (mean 0.39 to 0.52 against the
  golden image): the TAA and upscaler jitter, the shaders' noise and the probe rays followed the renderer's count of
  all frames, and in a batch the waits for loads in real time (`until`) took 1 to 5 frames more or fewer under
  load, so the capture had another noise phase. These sequences now restart at every camera cut (`View::cut`, or
  the renderer's own), and the tunnel's drive start is a cut: 13 runs of the native batch, 3 of them beside 14
  busy processes with the capture 1 to 5 frames off, gave the same captures byte for byte. 29 golden images of the
  demo changed (edges only, by the jitter phase).

## Changes for games

What a game on 0.9.0 changes or checks when it moves to this release. Neither game uses `quench_render` yet.

- **Ray hits read normal maps:** with traced reflections, indirect light, the probe grid or the path tracer, a
  material with a normal map (texture 2) looks different in reflections and bounced light than before (its normal
  map was ignored there); captures of such scenes change. The renderer makes three more pipelines when ray-traced
  lighting starts and the path tracer one more (`RenderStats::pipelines`).
- **`render::RenderSettings::probeUpdatePeriod`** is new (after `probeSpacing`; default 1, as before).
  `RenderStats::probes` is the probes traced this frame: 2048 as before by default, 2048 / n in turns. A shader of
  a game's own that reads the probe atlases through `quenchProbeIrradiance` needs nothing; one that reads the
  irradiance atlas directly should weight a probe by its alpha (0: no value yet).

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
- **A camera cut restarts the jitter and noise sequences** (`View::cut`, and the renderer's own cuts: a new size, a
  jump of more than 6 m, a turn of more than about 25 degrees): a game's frames right after a cut have the same
  jitter and noise as after any other cut. A game that sets `cut` every frame gets the same noise every frame.
- **`app::PerfReport::gpuSamples`** (and the perf line's sample count) is the number of frames whose GPU time was
  read, a few less than `frames` (the frames in flight at the end), where it equalled `frames` before; GPU times of
  a region come only from its own frames. **`PerfReport::processCpu`** and **`cpuPerformanceShare`** are new, at its
  end. The `perf:` line has `, process <ms> per frame (<n>% on performance cores)` after the CPU percentiles on
  Apple platforms and then `; wait ms p50 <ms> p95 <ms> (<present mode>, display <n> Hz)` everywhere: a parser of
  the line that expects `; gpu ms` right after `p99 <ms>` needs to allow both. New values `perf_wait_p50`,
  `perf_wait_p95`, `perf_present_mode`, `perf_display_hz`; **`gpu::Device::presentMode(window)`** is new.
- No new dependency library in the programs: the LTC data is code and a table in `quench_render` (BSD-2-Clause,
  with the paper reference its licence asks for), the HRTF data a table in `quench_audio` (Apache-2.0: a game that
  ships `quench_audio` ships the licence text from `THIRD_PARTY_LICENSES`). The cook adds astc-encoder (fetched at every host configure with
  the cook's other libraries, a 35 MB archive).
- **iOS builds cook ASTC 4x4 instead of BC7:** `quench_cook_assets()` without `TEXTURE_FORMAT` (or with `auto`)
  now cooks ASTC 4x4 for iOS (`CMAKE_SYSTEM_NAME` iOS) and BC7 elsewhere, as before; `TEXTURE_FORMAT bc7` keeps
  BC7 everywhere. Every iPhone that runs iOS 26 samples ASTC. A game that reads a texture's format
  (`TextureAsset::format()`) on iOS sees `Astc4x4Srgb` or `Astc4x4` there; a game that ships ASTC files to a device
  without ASTC sampling gets the placeholder and an error (BC7 is still decoded on the CPU there).
- **`cook::TextureFormat`** (the cook library, `tools/cook/Cook.h`) has `Astc4x4` after `Rgba8`; the cook's
  `--format` takes `astc`.
- **Debug builds read `asset_overlay/` in the user directory first:** a file at `asset_overlay/<path>` replaces the
  asset `<path>` in builds without `NDEBUG` (on the Mac too). A game that writes files of its own there, or wants
  Debug builds to read only the bundle, sets `-DQUENCH_ASSET_OVERLAY=OFF`. **`fs::makeOverlayBackend`** is new.
- **`ShaderBlob::manifest`** is a new member at the end of `ShaderBlob` (filled by the generated tables). Code that
  builds a `ShaderBlob` itself with a positional initializer warns under `-Wmissing-field-initializers` until it
  adds it. **`--hot-reload` now reloads shaders too** on desktops (the build's sources); `quench_add_shaders()`
  writes `quench_shaders/<set>/manifest.json` in host builds with a single-configuration generator.
- **`.ogg` files play:** a sound or stream that is an Ogg Vorbis file decoded to `Failed` before and plays now. The
  decoder's error for a file of no known format reads "not a WAV, FLAC, MP3 or Ogg Vorbis file". `quench_miniaudio`
  (for a game that links it) is built from the engine's `src/audio/Miniaudio.c` with stb_vorbis inside, instead of
  `miniaudio.c`; its configuration is unchanged. It also holds verblib's implementation (`verblib_*` symbols).
- **`audio::PlayParams`** has `sendBus` and `sendLevel` at its end (default: no send), **`audio::BusDesc`** and
  **`audio::BusInfo`** have `reverb` (`std::optional<ReverbDesc>`) at theirs: designated initializers and positional
  ones that stop before them compile unchanged. **`audio::ReverbDesc`**, **`AudioSystem::setSend`** and
  **`AudioSystem::setReverb`** are new. Without a reverb bus and sends the mix is bit for bit what it was.
- **`audio::AudioSettings`** has `hrtf` (off) and `hrtfVoices` (8) at its end; **`AudioSystem::setHrtf`** and
  **`hrtf()`** are new. With the HRTF off the mix is bit for bit what it was, but an `AudioSystem` with
  `hrtfVoices` above 0 (the default) keeps the table as floats and the filters, about 440 KB at 48 kHz: set
  `hrtfVoices = 0` to leave them out (then `setHrtf(true)` warns and stays off). Panned 3D voices cost about 8% more
  on the audio thread (about 0.2 µs per millisecond of audio for 32 voices on the M4 Pro).
- **`audio::AudioStats::hrtfVoices`** is new, after `maxVoices`: designated initializers compile unchanged, a
  positional aggregate initialization would fill the members after it one off. New scenario value
  `audio_hrtf_voices`.
- **Xcode projects** (`quench_add_game` with the Xcode generator): the merged assets are no longer a folder
  reference in the target's resources but are copied by a `CMake PostBuild Rules` script phase; a game's own script
  that expects the folder in `Copy Bundle Resources` needs to change. The `<target>_assets` target stays.
- **`animation::StateDef`** has `blend2d` and `blend2dParameters` after `blendParameter`, and
  **`animation::BlendPoint`** has `phase` at its end: designated initializers compile unchanged, a positional
  aggregate initialization of `StateDef` would fill the members after them one off. **`animation::BlendPoint2D`**,
  **`kMaxBlendPoints`** and **`blendSpaceWeights()`** are new.
- **Graphs:** a 1D blend tree takes at most 16 clips (`kMaxBlendPoints`; before, any number), and a state with both
  `clip` and `blend2d`, or `blend` and `blend2d`, is refused by `parseGraph`.
- **Animation LOD:** **`animation::AnimationLod`**, **`AnimationSettings::lod`** (at its end; off),
  **`AnimationSystem::setLod`**, **`lod()`**, **`setLodView`** and **`clearLodView`** are new;
  **`CharacterDesc::height`** (1.8 m, at its end), **`CharacterState::lodRate`**, **`lodIk`** and **`poseChanged`**
  and **`AnimationStats::evaluated`** and **`lodRates`** (at their ends) are new. Nothing changes until a game turns
  it on.
- **`render::Scene::setJointPalette`** with the matrices the instance already has (and no `teleport`) is ignored:
  the instance no longer counts as moved (`RenderStats::movedInstances`) for it. An unchanged palette's vertices are
  copied from the frame before instead of skinned again; **`RenderStats::skinnedReused`** is new, after
  `skinnedVertices` (designated initializers compile unchanged, a positional aggregate initialization would fill the
  members after it one off).
- **`render::InstanceDesc`** has `skinning` at its end (`render::Skinning::Linear`, as before): designated
  initializers compile unchanged. **`render::Skinning`**, **`Scene::setSkinning`** and **`SceneAssets::setSkinning`**
  are new. A game's own compute shader that reads the renderer's palette buffer gets dual quaternions for instances
  that use them (`SkinUniforms::counts.w` 1); nothing changes for instances left linear.
- **`quench_cook_assets`** takes `RETARGET` (needs `SKELETON`); calls without it cook as before. In the cook library
  (`tools/cook/Cook.h`), **`cook::AnimationOptions`** has `retarget` at its end, and `RetargetMap`,
  `parseRetargetMap` and `retargetClips` are new.
- **`.qmodel` version 4:** the cook writes version 4 (blend shapes); this engine still reads version 3 files (as
  models without blend shapes), but an engine before this release cannot read the new files: a game that ships
  cooked models cooks them with the engine it runs. **`assets::ModelMesh::morphTargets`**,
  **`assets::ModelMorphTarget`** and **`assets::ModelMorphVertex`** are new; `writeModel` refuses more than 256
  targets per mesh or a target with an offset count other than the vertex count.
- **Meshes with glTF morph targets cook differently:** such a mesh becomes a part of its own per node (before, its
  primitives were merged with the others of the same material and skin, and the targets were ignored), with its
  vertices welded with their offsets. Models without morph targets cook to the same meshes as before.
- **`render::MeshDesc`** has `morphs` and `morphTargetCount` at its end, **`render::MorphVertex`**,
  **`Scene::setMorphWeights`** and **`Scene::morphTargetCount`** are new: designated initializers compile
  unchanged. A mesh with blend shapes goes through the skinning pass even without joints
  (`RenderStats::skinnedInstances` counts it).
- **`render::ModelAsset::Part`** has `morphs` and `morphDefaults` at its end; **`ModelAsset::morphTargets()`**,
  **`morphDefaults()`** and **`SceneAssets::setMorphWeights`** are new. A model's parts show the file's default
  weights until the game sets them.
- **The skinning pass's buffers** (for a game's own compute shader that reads them): the palette buffer holds an
  instance's (target, weight) rows in front of its joint rows (`SkinUniforms::counts.y` is still its first joint
  row), `SkinUniforms::morph` is new (the count of those rows, the first of them, 1 when the mesh has joints), and
  instances with weights are posed by a second entry, `skin_morph_cs`, which reads a fourth storage buffer, the
  mesh's morph offsets. Skinned meshes without weights cost what they did (within 2.5% on the demo's crowd).
- **The cook library** links fastgltf's simdjson (for glTF extras); `cook::loadGltf` takes an optional
  `GltfExtras*`, and `cook::morphTargetName` is new. `quench_cook model --verbose` prints the blend shape count
  before the byte count.
- **`.qanim` version 2:** a clip set with float channels is written as version 2, which an engine before this
  release cannot read; one without channels is still written as version 1, byte for byte as before (clips cooked
  from glTF files without `weights` animations, and every retargeted clip set of such files). glTF `weights`
  animations, a note before ("morph target weights ... are not cooked"), now cook into channels: a game whose files
  have them gets version 2 clip sets. In a file without skins a node animated only by weights is now a joint of the
  skeleton, as any animated node.
- **`animation::ClipDesc`** has `channels` at its end and **`animation::ChannelTrack`** is new;
  **`ClipBuildOptions`** has `channelTolerance` before `optimize` (designated initializers compile unchanged, a
  positional aggregate initialization would put `optimize` into it); **`Clip::channels()`**,
  **`AnimationSystem::addChannels`** and **`AnimationSystem::channels`** are new. Characters without channel sets
  update as before.
