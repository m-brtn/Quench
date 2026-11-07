# Quench, the next release (draft)

Not released: no version or tag yet (the version in `project()` stays 0.10.0). Changes since [0.10.0](v0.10.0.md)
(tag `v0.10.0`): the native Metal backend as the default on Apple platforms; an opt-in reaction to the device's
heat for games, with the demo's Adapt to heat; an agent skill on building a game on the engine, copied into every
new game; the demo's dual quaternion skinning showcase made readable after a user
report, and its characters' wrists closed; the Xcode project's generation in the full gate; soft shadows of area
lights from shadow maps where the device cannot trace (stage 12's remaining work); a game's surface shaders in ray
hits and the probe grid's cascades, relocation and classification (stage 11's remaining work); Basis Universal
textures, one cooked set for every device (stage 6's remaining work); the per-pixel ray-traced
indirect light settles in a still scene instead of staying grainy (a user report); navigation on Recast & Detour,
a new module with the demo's Navigation section (the user's queue, item 4); profiling with Tracy, opt-in, with
captures recorded and summed up without its GUI; the weather's first part, volumetric clouds that drift, change shape
and shade the light, in a new headless module with the demo's Weather section (stage 13, part 1); its second part,
rain where the clouds drop it, wet ground and puddles that dry afterwards, and the rain's sound (stage 13, part 2);
its third, snow where the air is cold enough, a cover that builds and melts, and quiet (stage 13, part 3).
This file becomes
`v<version>.md` when the user asks for the release; what was verified is in the devlogs of
[2026-11-07](../../devlog/2026-11-07.md) and [2026-11-08](../../devlog/2026-11-08.md).

## What changed

### The GPU backend

- **The native Metal backend is the default on macOS, iOS and the iOS Simulator** (the user's decision,
  2026-11-07; [Architecture](../architecture.md#native-metal-backend), [Platforms](../platforms.md#native-metal-backend)).
  `gpu::defaultBackend()` is the one place that decides it: Metal on Apple platforms, SDL GPU elsewhere.
  `Backend::Default` (`DeviceDesc::backend`, `AppDesc::gpuBackend`) follows it; `QUENCH_GPU_BACKEND=sdl|metal` and
  `--gpu-backend sdl|metal` override it, and a driver only SDL GPU has (`--gpu-driver vulkan`, `SDL_GPU_DRIVER`)
  selects SDL GPU. Nothing else changes by default: `RenderSettings` keep shadow maps and every ray tracing and
  MetalFX effect off.
- **A fallback:** a native Metal device that fails to start gives an SDL GPU device with one warning (`the native
  Metal backend failed to start (...): SDL GPU instead`); when both fail, the error names both reasons.
- **Buffers cycled on the native backend:** a GPU buffer written with `cycle` (an upload or copy into it, a compute
  pass's storage write) moves to a backing no unfinished command buffer used, as on SDL GPU. Before, Metal's hazard
  tracking ordered the write after the previous frame's reads: the same pixels, but each frame's instance and
  skinning uploads waited for the previous frame's draws. `cycle` on a texture is still a no-op there.
- **Tests and batches name their backend:** `gpu_device.*` and `render.*` run on SDL GPU's Metal driver
  (`QUENCH_GPU_BACKEND=sdl`), `gpu_metal_native.*` and `render_metal_native.*` on the native backend, `*_vulkan` on
  Vulkan through MoltenVK; the test harness fails a case whose device runs on another backend than its variant (a
  fallback included), and each batch checks the app's `gpu: backend` line (`quench_add_batch(... GPU
  <backend>:<driver>)`). New cases: `default_backend`, `native_fallback`, `buffer_cycle`. Debug and sanitizer builds
  run `demo_smoke_metal` and `demo_smoke_metal_native` on Apple platforms.

### Rendering: shadows of area lights without ray tracing

- **Area lights take shadow maps** (stage 12's remaining work; [Render module](../render.md#local-light-shadow-maps)):
  with `RenderSettings::localShadowMapSize` (off by default, as before) the nearest lights with `castsShadows` that
  are not traced, area lights among them, get maps. An area light's cube of maps is in its own frame, one-sided rects
  and disks skip the face behind them (five maps), and the maps reach the range plus the shape's radius.
- **A penumbra that follows the shape:** percentage-closer soft shadows whose blocker search and filter use points
  of the rect, disk or tube (the tube's half that faces the receiver), each weighted by the light it sends to the
  receiver, with depths measured across the shape. A narrow strip casts shadows soft across it and sharp along it.
- **Read in screen space:** in a frame where an area light has maps, every map (the spot and point lights' too) is
  read by a screen-space pass at half resolution over a depth prepass, blurred by a depth-aware 3 x 3 filter, and
  the surfaces read the result as they read traced shadows (`options.w` bit 4); frames with point and spot maps only
  read them in the surfaces as before. Timed ranges `local_shadows` (the maps) and `local_filter` (the pass).
  Transparent surfaces leave area lights unshadowed.
- **Cost** on the M4 Pro (offscreen, the tunnel's four nearest lamps, 21 maps at 512): 0.92 ms of GPU time at the
  tunnel's 0.67 render scale and 1.7 ms at 1080p ([Performance](../performance.md#area-light-shadow-maps)).

### Rendering: surface shaders in ray hits

- **`quench_add_ray_hits(<target> NAME <set> SURFACES <file>...)`** (`cmake/QuenchShaders.cmake`; stage 11's
  remaining work; [Render module](../render.md#ray-hits-of-surfaces)): the engine's tracing entries (traced
  reflections, indirect light, the probe grid's rays, the path tracer; with and without normal maps) compiled again
  at build time with the game's surface files, each in a namespace of its own, and a generated switch over their
  number. `SurfaceDesc::rayHitShaders` and `rayHit` name the set and the surface's number in it; the hits of its
  materials then run its function (albedo, opacity, roughness, metallic, normal, emissive, anisotropy) with the
  engine's lighting, instead of the standard surface from the material's values. One set per renderer; its 8
  pipelines are made in the first frame that traces such a surface, and frames without one use the engine's own.
- **`QUENCH_SURFACE_HIT`** is 1 in those hits: `Surface.hlsli` declares no resources or lighting there, the
  material and its textures come from the hit tables (the textures' 256 x 256 copies at level 0), `ddx`, `ddy` and
  `fwidth` are 0, `pixel` is 0 and `view` points toward the ray's origin.
- **`RenderStats::rayHitSurfaces`** counts the instances whose hits run their function; the hit table's entry
  (`RayInstanceData`) grew from 144 to 256 bytes per instance (the material's base color and params, the
  instance's color and custom vector).
- **The demo's checker grounds** have their hits: the ray tracing section's mirror and gold spheres show the
  ground's cells instead of its mean color. A native batch part `raytracing_mirror` with its golden image; the
  demo's `camera` command takes the point it orbits (`camera YAW PITCH DISTANCE X Y Z`).
- **Cost** on the M4 Pro (offscreen, 1080p, the demo's ray tracing section, whose ground is the checker; GPU p50 of
  the pass, the lowest of alternated runs under other agents' load): traced reflections 0.93 to 0.97 ms, the probe
  grid's rays 1.24 to 1.27, per-pixel indirect light 3.98 to 4.09, the path tracer 11.7 to 12.7 (6 to 8 % more);
  the tunnel (no such surface) unchanged ([Performance](../performance.md#surface-functions-in-ray-hits)).

### Rendering: probe cascades, relocation and classification

- **`RenderSettings::probeCascades`** (1 to `render::kMaxProbeCascades` = 4, default 1; stage 11's remaining work;
  [Render module](../render.md#ray-traced-lighting)): grids of 16 x 8 x 16 probes around `View::probeCenter`, each
  twice the spacing of the one before, so the probes are dense near it and reach far (three from 1 m cover
  64 x 32 x 64 m). A surface takes the finest cascade around it, blended into the next over its last cell; each
  cascade scrolls and is traced in turns on its own rows of the atlases. `RenderStats::probes` counts all of them.
- **`probeRelocation`** (default off): each traced probe moves out of geometry within its cell (through the nearest
  back face when more than a quarter of its rays hit back faces, away from a surface nearer than 0.2 spacings, back
  toward its cell's middle otherwise, at most 0.45 spacings), kept by a probe data pass (`rt_probe_data_fs`) and
  read by the shading and the next frame's rays.
- **`probeClassification`** (default off): a probe with no surface within 1.5 spacings, or inside geometry, for 8
  updates is left out: a quarter of its rays, no light; it comes back when its rays find a surface.
- **The demo:** the Graphics panel's Probe cascades, Move out of geometry and Leave out probes in the open
  (`probe_cascades`, `probe_relocation`, `probe_classification`); a native batch part `raytracing_probes` with its
  golden image (1 m in 3 cascades, moved and classified); `raytracing_phone_probes.txt` measures classification and
  cascades on a phone.
- **Cost** on the M4 Pro (offscreen, the sections' Mac defaults otherwise, GPU p50): a cascade about 1 ms of probes
  (1.20 ms for the ray tracing section's grid, 2.20 with 2 cascades, 3.16 with 3) and 0.1 to 0.3 ms more in the main
  pass, in the tunnel about 0.8 ms more in the reflections; classification takes 40 to 45 % off the probes (1.20 to
  0.72 ms, the tunnel's 1.21 to 0.67), relocation adds 0.06 to 0.07 ms; with neither and one cascade the probes are
  1 to 2 % slower than before and the main pass the same
  ([Performance](../performance.md#probe-cascades-relocation-and-classification)).

### Assets: Basis Universal textures

- **One cooked set for every device** (stage 6's remaining work; [Assets](../assets.md#basis-universal)): the cook
  writes Basis Universal KTX2 files (`quench_cook texture ... --format basis [--basis-codec auto|etc1s|uastc]`;
  `quench_cook_assets(... TEXTURE_FORMAT basis [BASIS_CODEC auto|etc1s|uastc])`, also for a model's textures). ETC1S
  is small and lossy, UASTC about BC7's quality; `auto` takes UASTC for linear data and normal maps and ETC1S for
  color. Basis Universal v2.50 (Apache-2.0, pinned with its SHA256 and kept once per machine in the tool cache;
  [Dependencies](../dependencies.md)): the encoder in the cook only, the transcoder in `quench_assets` (0.96 MB in a
  stripped arm64 program).
- **Transcoded at load time**, on the loading job, to what `gpu::Caps` allow: UASTC to ASTC 4x4 (exact), BC7 or
  ETC2; ETC1S to ETC2 (exact, half the memory), ASTC 4x4 or BC7; RGBA8 where the device has none of them or a side
  is not a multiple of 4, with a log line. `assets::prepareTexture` reads either kind of KTX2 file (`readBasisInfo`,
  `basisTarget`, `transcodeBasis` for the parts); `render::TextureAsset::basisCodec()` and `decodeMilliseconds()`
  say what happened, `SceneAssets::textureSupport()` what the device takes. Hot reload and the asset overlay read
  them like any other file.
- **ETC2 textures on the native Metal backend:** `gpu::TextureFormat::Etc2Rgb8Unorm`, `Etc2Rgba8Unorm` (EAC alpha)
  and their sRGB variants, and `gpu::Caps::etc2Textures` (Apple7 GPUs and later on macOS, every iOS GPU); SDL GPU
  3.4.18 has no ETC2 formats.
- **The default stays BC7 on the Mac and ASTC on iOS:** on the demo's 19 textures ETC1S is 13 times smaller than
  BC7 but 19 dB worse on color (37.9 against 56.7 dB), UASTC half BC7's size (no smaller once a package is
  compressed) and 1.5 dB below it, and both cook 3 to 7 times slower and add a transcode at load time.

### Rendering: the per-pixel indirect light settles

- **The temporal denoiser of `IndirectLight::RayTraced`** ([Render module](../render.md#denoising)): a user report
  from the iPhone 17 (2026-11-07: the ray tracing section's phone defaults with Indirect light "Ray traced" grainy
  all the time, also when nothing moves). At one ray per pixel the 9 new samples around a pixel often agree by
  chance, and the history's clamp and anti-lag, which used their spread alone, pulled the history to their mean
  every few frames: the light never settled, and its mean came out darker. Its filter now also keeps each pixel's
  standard deviation over its history and clamps and cuts with no less than it, and takes a third a-trous pass. The
  sun, occlusion, lamp shadow and reflection filters are unchanged (bit for bit).
- **Measured** (the demo at its phone defaults with per-pixel indirect light, offscreen on the M4 Pro;
  [Performance](../performance.md#the-denoiser-in-a-still-scene)): flicker from frame to frame 0.84 levels before,
  0.26 after (the probe grid alone 0.25), the same from 60 frames (1 s at 60 Hz) on; in motion the error against
  the same instant settled 2.27 before, 1.57 after. Cost: the indirect light's range +0.10 ms of 1.05 at half
  size; the phone defaults without per-pixel indirect light unchanged.

### The app: a reaction to heat

- **`App::thermalGovernor()`** (`quench/app/ThermalGovernor.h`; [App shell](../app.md#thermal-state)): a game's
  opt-in reaction to the system's thermal state, off by default; the engine still changes nothing by itself. Enabled
  with a `ThermalGovernorDesc`, it maps the states to limits the game allows (`ThermalLimits`: a frame rate limit, a
  factor on the render scale, `reduceEffects` for the game's own choice of effects). Defaults: nothing at fair; 60 Hz
  and x0.8 from serious on; 30 Hz, x0.6 and `reduceEffects` at critical. A higher state applies at once, a lower one
  only after `recoverSeconds` (60 s) of app time below, then to the highest state of that time; unknown counts as
  nominal. The App applies the frame rate limit on top of `setMaxFrameRate`, never above it (`App::frameRateLimit()`
  is the result), and the player's value comes back when it is lifted; `renderScale(playerScale)` gives the game the
  reduced scale without touching its settings. A log line per change, `onChange`, a perf line field and
  `PerfReport::reactionStart`, `reactionEnd`, `reactionMax`.
- **Checks without a hot device:** `--thermal-state nominal|fair|serious|critical|unknown` and the scenario command
  `thermal_state NAME` (`system` gives the state back) replace the system's state; new scenario values
  `thermal_reaction`, `thermal_frame_rate_cap`, `thermal_render_scale`, `thermal_reduce_effects`,
  `thermal_reaction_changes`, `frame_rate_limit`, and after `perf_end` `perf_thermal_reaction_start`, `_end`, `_max`.
- **The demo's Adapt to heat** (Settings, off; `adapt_to_heat` in `settings.json`, still version 1): the engine's
  defaults; the render scale goes into what the renderer gets and, at critical, the traced reflections and indirect
  light fall back to the sky's and the lamp shadows and the shadow and occlusion rays go down to one. The Performance
  panel shows the thermal state and what is reduced (in both layouts), the Graphics panel a line while limits apply;
  `benchmark.json` gets the reaction per section. A batch part `heat` (no capture) and a check mode item
  `tunnel.heat` (the tunnel's phone defaults for 10 minutes with it on; 41 items now).
- **The template** has the same setting as an example (`"adapt_to_heat": false` in `assets/settings.json`, a
  checkbox, the scenario command `adapt_to_heat on|off`), and its smoke scenario checks it with `thermal_state`.

### Navigation

- **`quench_navigation`** (headless; Recast & Detour 1.6.0, zlib; [Navigation](../navigation.md)): navmeshes built
  from a level's triangles tile by tile on the job system across frames, or cooked into `.qnav` assets; up to 16
  area types with costs (`BuildSettings::areas`, `AreaBox`), off-mesh links (`Link`: jumps, ladders, teleporters,
  with the game's id), cylinder and box obstacles that rebuild only the tiles they touch, `NavMesh::rebuild` for
  changed geometry; `NavQuery` (paths with partial results, nearest points, raycasts, random points, `Filter`s with
  area costs and excluded areas); `Crowd` (DetourCrowd: avoidance levels, separation, arrival distance, filters,
  `setVelocity` for steering by input, `LinkStarted`, `LinkFinished` and `Arrived` events, agents that wait while
  their tiles are missing); `DebugGeometry` (triangles and lines of the mesh, obstacles, paths and agents for any
  renderer). Public headers `quench/navigation/NavMesh.h`, `NavQuery.h`, `Crowd.h`, `DebugGeometry.h`,
  `NavMeshAsset.h`; log category `navigation`; written for the engine, not moved from a game.
- **Deterministic tiles:** every layer of a tile column has a fixed tile slot and a rebuilt tile takes the slot's
  next salt, so a build gives the same polygon references whatever order its jobs finish in, and
  `NavMesh::contentHash()` is the same with 0 or 4 worker threads.
- **Area costs in crowds:** Detour's visibility shortcut (`DT_CROWD_OPTIMIZE_VIS`) takes a straight way through any
  area the filter lets through, whatever it costs; the engine enables it only for filters whose areas all cost the
  same, so agents walk around costly areas as their paths do.
- **Detour patched:** with 64-bit polygon references a `dtLink` needs 8-byte alignment that Detour's 4-byte tile
  layout does not give (UndefinedBehaviorSanitizer; upstream issue #669); `cmake/patches/DetourAlign8.cmake` makes
  its `dtAlign4` round up to 8 bytes in that mode ([Dependencies](../dependencies.md#added-for-navigation)).
- **Cooked navmeshes:** `quench_cook navmesh IN OUT [--settings FILE] [--depfile FILE]` (a glTF, GLB or `.qmodel`
  and a `<stem>.nav.json` of agent, cell, area, surface, box and link settings) and `NAVMESHES` in
  `quench_cook_assets()`; the same input gives the same bytes. At run time `NavMeshAsset` loads `.qnav` files on the
  asset manager's jobs (magic, version, size and an XXH64 checksum checked), and `NavMesh::create` makes a mesh of
  them.
- **Cost** on the M4 Pro (Release): a 40 x 40 m level of two floors, 5 x 5 tiles, 120 polygons, built in 1.3 to
  3.4 ms on the job system; a path 7 to 15 µs; a crowd update 0.02 to 0.05 ms for 25 agents and 0.13 to 0.20 ms for
  100 ([Navigation](../navigation.md#cost)).

### Weather: clouds and their light (stage 13, part 1)

- **`quench_weather`** (headless; [Weather](../weather.md)): `weather::Weather`, a map of clouds over the ground
  from `Settings` (seed, coverage, cloud type from stratus to cumulonimbus and its variation, precipitation, cirrus,
  wind, evolution, feature size, cloud base and top) and up to 16 `Cell`s a game places (one raining cumulonimbus in
  a blue sky, an overcast patch that stays over a mountain), moved by the wind and changing shape over time.
  Deterministic: a pure function of its settings, cells and clock (`update`, `setTime`), the same in every run and on
  every platform. Queries: `sample` (coverage, type, precipitation, cirrus), `precipitation`,
  `distanceToPrecipitation` (for the sound of distant rain), `cover` (how overcast within a radius), `windAt`,
  `fill`. The field is one header shared by C++ and HLSL (`quench/WeatherField.h`), so a query and the clouds on the
  screen agree. Public header `quench/weather/Weather.h`; written for the engine, not moved from a game.
- **The clouds** (`RenderSettings::clouds`, off by default; [Render module](../render.md#clouds)): `Scene::setWeather`
  gives the renderer a copy of the weather. `Clouds::Volumetric` raymarches them after Nubis (Horizon Zero Dawn):
  procedural 3D noise made once on the GPU, cloud types by height profiles, Beer-Lambert with a powder term, multiple
  scattering in four octaves and two Henyey-Greenstein lobes, so the bases are dark, the edges against the sun bright
  and sunsets tint them; at `cloudResolution` of the render size (half by default), `cloudSteps` steps, accumulated
  over frames with reprojection (`cloudTemporal`). `Clouds::Layer` is a 2D layer from the same map, the fallback
  where the device lacks the formats (`gpu::Caps`) and the cheap setting.
- **Light under the clouds:** the clouds' shadow on the ground (`cloudShadows`: a map 24 km across, read by the
  surfaces, the shadow maps' sun and the ray-traced sun shadows alike), the sun dimmed and the sky's ambient light,
  the fog and the traced effects' sky (misses, the probe grid, the path tracer) moved toward an overcast's grey by
  the cover around the camera, and a cube of the sky with its clouds (`skyCubeSize`, `skyCubeFacesPerFrame`) that
  surfaces reflect instead of the bare sky. `ActiveEffects::clouds` and `skyCube`, `RenderStats::clouds`,
  `cloudWidth`, `cloudHeight`, `cloudCover`, `skyCubeFaces`; timed ranges `cloud_noise`, `weather`, `clouds` and
  `sky_cube`.
- **3D textures drawn as 2D array layers:** the clouds' noise is drawn into the layers of a 2D array and copied into
  a sampled 3D texture on every backend, because MoltenVK's portability subset has no 2D views of 3D images; SDL GPU
  is patched to create 3D textures 2D-array compatible only where they are color targets
  (`cmake/patches/SdlVulkan3d.cmake`; [Dependencies](../dependencies.md)). New GPU cases `texture_3d_from_layers`
  and `cube_faces`.
- **Cost** on the M4 Pro (offscreen, native backend, GPU busy time; [Performance](../performance.md#clouds)): at
  1920 x 1080 the 2D layer 0.38 ms, volumetric clouds at half size 1.97 ms (fair weather) and 1.48 ms (a storm), at
  full size 4.29 and 3.23 ms; at the iPhone 17's 1206 x 2622 within 0.05 ms of those at half size and 5.14 and
  3.55 ms at full size.

### Weather: rain (stage 13, part 2)

- **Wetness the weather remembers** ([Weather](../weather.md#wetness-and-puddles)): `Weather::wetness(xz)` (ground
  and standing water, 0..1) follows the rain that fell there: a grid of 16 x 16 cells of 64 m around
  `setWetnessFocus` (a game's camera or player) wets and dries in steps of 0.25 s of weather time by
  `Settings::wettingTime`, `dryingTime`, `puddleFillTime` and `puddleDryTime`; `setTime` estimates it from the last
  hours of the weather, so a jump in time and a saved game give the same ground. `wetnessGrid`, `wetnessOrigin`,
  `wetnessVersion`; `rainVelocity(point)` and `kRainFallSpeed` (the rain carried by the wind). Headless: also under
  Node.js.
- **The rain in the renderer** (`RenderSettings::rain`: `Off` by default, `Low`, `Medium`, `High`;
  [Render module](../render.md#rain)): where the weather's precipitation falls, streaks around the camera lit by the
  sky and the sun, splashes where they land (Medium and High), and a rain occlusion map (the shadow casters drawn
  along the rain, 128 m around the camera) that keeps them and the wetness from under roofs and tells how covered
  the camera is (`RenderStats::rainCover`). With clouds, the rain's shafts below raining clouds from afar
  (`rainShafts`, on; at a quarter or half of the render size) and a rainbow (`rainbow`, off), on the 2D layer too.
- **Wet surfaces, puddles and ripples:** the surface entries call `quenchApplyRain` (`quench/Rain.hlsli`): wet
  surfaces darker and glossier as the weather's ground, standing water in the hollows of materials with
  `MaterialDesc::puddles`, and rings of the drops on it and on `MaterialDesc::water` (a lake) in the normals, so they
  show in the reflections. `MaterialDesc::wetness` (1 by default) scales it per material; `QUENCH_SURFACE_NO_RAIN`
  keeps a surface file dry. `ActiveEffects::rain`, `rainShafts`; `RenderStats::rain`, `rainAtCamera`, `rainStreaks`,
  `rainSplashes`, `rainOccluders`, `rainShafts`, `rainbow`; timed ranges `rain_map`, `rain_shafts`, `rain`.
- **The rain's sound** (`audio::WeatherAudio`, `quench/audio/WeatherAudio.h`; [Audio](../audio.md#weather-sounds)):
  light and heavy rain crossfaded by the precipitation at the listener, distant rain by the distance to the nearest
  rain, rain on the roof by the cover, on a bus, smoothed; a game names its own sound for a layer (`setSound`),
  silences one, or switches the object off (`setEnabled`) and plays its own from the same numbers
  (`targetGains`). Its sounds are assets, so a reloaded file crossfades into the new data. Engine defaults: four
  procedural loops (`tools/assets/make_weather_sounds.py`, CC0) cooked by the CMake function
  `quench_weather_sounds(<name>)`.
- **Cost** on the M4 Pro (offscreen, native backend, GPU busy time; [Performance](../performance.md#rain)): at
  1920 x 1080 0.38, 0.75 and 1.30 ms for Low, Medium and High on the ground and up to 1.88 ms looking at a storm
  from the air; the phone defaults (Low without shafts) 0.20 ms, 0.25 ms at 1206 x 2622 (measured again with part
  3's splashes).
- **Polished with part 3:** the splashes show (the occlusion map was drawn with the cascades' slope bias, which sank
  it by decimetres under slanted rain and put the splashes under the ground; now crowns of droplets with twice the
  light and the sun's glint, 1536 and 4096 at Medium and High instead of 1024 and 3072), and the ripples differ from
  puddle to puddle (each drop's size, the density and strength by place). The streaks and splashes are not drawn
  where no rain falls near the camera (`RenderStats::rainStreaks` and `rainSplashes` are 0 then).

### Weather: snow (stage 13, part 3)

- **Rain or snow by the temperature** ([Weather](../weather.md#snow)): `Settings::temperature` (15 degrees C at the
  ground by default) and `lapseRate` (0.0065 per metre); snow at or below 0 degrees, rain from 2.5, sleet between;
  `Cell::snow` gives a cell its own share. `Weather::snowShare(point)`, `temperature(point)`, `snowVelocity(point)`
  and `kSnowFallSpeed` (1 m/s); `distanceToPrecipitation(..., Precipitation kind)` finds rain or snow only
  (`weather::Precipitation`: `Any`, `Rain`, `Snow`). In the shared field `quenchWeatherSnow`. With the defaults
  nothing below the default cloud base is snow.
- **A snow cover the weather remembers:** `Wetness::snow` (0..1) in the wetness grid builds by the snowfall in
  `Settings::snowTime` (600 s) and melts above freezing in `meltTime` (900 s at 5 degrees), its water wetting the
  ground and filling the puddles; deterministic like the wetness; `WetnessCell::snowShare`. Headless: also under
  Node.js.
- **The snow in the renderer** (`RenderSettings::snow`: `Off` by default, `Low`, `Medium`, `High`;
  [Render module](../render.md#snow)): flakes that flutter and drift with the wind near the camera, none under
  roofs (the rain's occlusion map, looking at most 17 degrees off the vertical under snow); with clouds the shafts of
  snow from afar, 3 to 5 times as dense as rain's and whiter. Surfaces get the cover where they face up and the snow
  reaches them (`quenchApplySnow` in the surface entries after `quenchApplyRain`): patches that close as it deepens,
  snow's albedo and roughness. `MaterialDesc::snow` (1 by default) scales it, `QUENCH_SURFACE_NO_SNOW` opts a surface
  file out. `ActiveEffects::snow`; `RenderStats::snow`, `snowAtCamera`, `snowShareAtCamera`, `snowFlakes`; timed
  range `snow`.
- **Quiet:** `audio::WeatherSoundState::snow` (the share at the listener) takes the snowfall out of the rain's
  layers and muffles the distant rain in a snowfall.
- **Cost** on the M4 Pro (offscreen, native backend, GPU busy time; [Performance](../performance.md#snow)): at
  1920 x 1080 0.35, 0.76 and 1.02 ms for Low, Medium and High under a snowfall, up to 0.97 ms in snow showers seen
  from the air; the phone defaults (rain and snow Low without shafts) 0.23 ms, 0.34 ms at 1206 x 2622.

### Profiling with Tracy

- **`QUENCH_TRACY`** (CMake option, off by default; the `profile` configure, build and test preset: Release with
  it on; [Performance](../performance.md#profiling-with-tracy)): Tracy 0.15.0's client in `quench_core`, on demand
  (it records only while a Tracy server is connected) and on localhost only on desktops. Off, nothing of Tracy is
  fetched, compiled or linked. Forced off in WebAssembly builds. `QUENCH_TRACY_MEMORY` (off) adds allocation
  tracking through the global `operator new` and `operator delete`.
- **Profiling macros for the engine and games** (`quench/core/Profile.h`): `QUENCH_PROFILE_ZONE`,
  `QUENCH_PROFILE_ZONE_DYNAMIC`, `QUENCH_PROFILE_ZONE_TEXT`, `QUENCH_PROFILE_FRAME`, `QUENCH_PROFILE_PLOT`,
  `QUENCH_PROFILE_MESSAGE`, `QUENCH_PROFILE_THREAD_NAME`, nothing without `QUENCH_TRACY` (their arguments are not
  evaluated); `quench::profile::connected()`. No Tracy header in public headers unless it is on.
- **What the engine records:** a frame mark and zones of the app loop (update, fixed steps, UI, render, the wait for
  the GPU, submit), a CPU zone per timed range of passes on every backend, and on the native Metal backend the same
  ranges as GPU zones from the engine's own timestamps (their busy intervals, so they add up to the perf lines' busy
  times); named worker threads and a zone per job (`JobSystem::run(name, ...)`, `parallelFor(name, ...)`),
  `physics.step` with Jolt's own zones, the animation update, asset loads, reads, decodes, transcodes and uploads,
  audio stream refills; plots of the frame, CPU and GPU times and the mix time; log lines as messages, perf regions
  as `perf_begin` and `perf_end` messages. The audio device's callback records nothing (Tracy's zone path may
  allocate).
- **Captures without the GUI:** Tracy's `tracy-capture` and `tracy-csvexport`, built by the profile preset with
  their dependencies pinned by hash (`QUENCH_TRACY_TOOLS`), and `tools/tracy_capture.py`: records a scenario run
  offscreen into a `.tracy` file and writes zone statistics per perf region to CSV (count and time per frame, mean,
  p50, p99, self time). The demo's `scenarios/raytracing_profile.txt`: the ray tracing section at its defaults and
  at heavier settings.
- **Tests:** `core.profile_macros`, `core.profile_without_server`, `core.jobs_named`; in profile builds
  `hello_tracy_capture` (a capture of hello, its zones and, on the native backend, its GPU zones).

### Building games

- **The agent skill `quench-game`** (`.agents/skills/quench-game/`: `SKILL.md` and `reference.md`): how a coding
  agent builds a game on Quench: `tools/new_game.py`, the `Game` interface and the fixed-step loop,
  `quench_add_game()`, the renderer, cooked assets, physics, audio, animation, jobs, input, ImGui, `quench::fs`,
  offscreen scenarios, captures and golden images (with the game's own build of `quench_image_compare`), iOS builds,
  the rules a game keeps and pitfalls. `tools/new_game.py` copies it unchanged into every new game, whose
  `AGENTS.md` points to it. `AGENTS.md`: a change to what games build on (public API, `quench_add_game()`, the
  template, common flags, scenario commands, asset cooking) updates the skill in the same commit. Linked from the
  README.
- **The skill's navigation:** `SKILL.md` names the module and `NAVMESHES`, `reference.md` has a section on
  building a navmesh, a crowd and loading a cooked one; `tools/check_skill.py` checks the names after
  `navigation::`.
- **The template's iOS presets cook assets:** `ios-base` passes `QUENCH_HOST_COOK` and the `host-tools` build
  preset builds `quench_cook` too; before, a template game that called `quench_cook_assets()` stopped at its iOS
  configure (`cmake/QuenchCook.cmake` needs the host cooker in a cross build). See "Changes for games".

### The demo

- **The skinning showcase** (section 10, view "Skinning"; [Engine demo](../demo.md#sections)): the user toggled
  "Dual quaternion skinning" there and saw nothing change (2026-11-07): the checkbox switched only the other
  characters, whose joints hardly twist, and the showcase was two small mannequins with a fixed skinning each. Now
  the view is a close-up of two red and white checkered bars on posts, their upper halves turned by 170 degrees
  about their length, with a standing character beside each whose inner forearm is turned the same way. The left
  pair is labelled "Linear (reference)" and always skinned linearly; the right pair follows the checkbox and its
  label reads "Linear" or "Dual quaternion". Linear skinning pinches the bars and forearms to a thin neck at the
  twist; with the checkbox on, the right bar stays round and its squares run around it in a spiral. A "Twist"
  slider (0 to 180 degrees, scenario value and command `twist`) and a line in the panel say what to look at. The
  bars are drawn in that view only. The check mode's `animation.skinning` item says what a person should see.
- **Two captures in one part:** `animation_skinning` captures the view linear (`demo_animation_skinning.png`) and
  with the checkbox on (`demo_animation_skinning_dq.png`); a batch part can name several golden images
  (`quench_add_batch(... PARTS <file>:<golden>:<golden>)`, `tests/CheckPart.cmake`). Its waits end half a frame off
  the fixed clock's steps, as the retarget and blend shape parts' do, so the native backend's captures match SDL
  GPU's.
- **The characters' wrists:** a gap showed the sky between the forearms and the hands of the mannequin (the user's
  screenshots of 2026-11-07 in the Skinning view) and of the strider. Their forearm tubes ended 1 cm (mannequin) and
  1.3 cm (strider) short of the wrist, where the rigid hand box begins. Now each forearm runs through the wrist into
  the hand box, narrowed to the hand's width there; the ring at the wrist is skinned half to the hand, the one inside
  the box wholly, so the join follows the hand when it turns (`tools/assets/make_demo_character.py`,
  `make_demo_strider.py`: 884 vertices each, 836 before). The Skinning view's camera is a little further back and
  lower, so that the bars clear the demo's caption and the menu at the default window size.
- **The tunnel's lamp shadow maps** (section 12 without ray tracing: SDL GPU, or a device that cannot trace): a
  "Lamp shadow maps" choice in its panel (Off, 256, 512, 1024; off by default), a batch part `tunnel_maps` with golden
  captures on SDL GPU's Metal driver and on Vulkan through MoltenVK, and a check mode item `tunnel.maps` (42 items
  now).
- **Basis Universal tiles** (section 8, Assets): the tiles are also cooked as an ETC1S and a UASTC set; "Tile
  textures" streams them instead of the platform's set, and a line names the format they were transcoded to. The
  batch part `assets_basis` (no capture) checks both sets on every backend, and a check mode item asks a person to
  look at them (43 items now).
- **The native backend by default:** the ray tracing section and the tunnel trace where `gpu::Caps` allow it
  without a flag now (on the Mac and on phones with ray tracing: the iPhone 17 and the iPhone 18 Pro Max), with their
  phone defaults on phones; `--gpu-backend sdl` shows their fallbacks. About names the platform's default backend and
  says when a run is not on it; the Graphics panel's relaunch buttons mark the default. The settings (version 1)
  store no backend, so nothing is migrated. The `*_phone` scenarios measured on SDL GPU say `--gpu-backend sdl`.

- **Navigation** (section 13; [Engine demo](../demo.md#sections)): a level of two floors joined by a ramp, stairs
  and a narrow bridge, a mud field whose cost a slider sets (1 to 10), one-way jumps down the ledges, two crates and
  a barrel that move as obstacles, and two groups of mannequins (1 to 12 each) that patrol their rounds or go where a
  click or tap on a floor sends them, walking or running by their speed, lifted on an arc with the jump clip over a
  link; the navmesh, paths and agents as overlays (a demo shader, `shaders/debug.hlsl`). The section builds its
  level at run time and shows the build's and the crowd step's times. A batch part `navigation` with golden
  captures (the same image on SDL GPU's Metal driver and the native backend; Vulkan through MoltenVK), a benchmark
  region, a check mode item `navigation.crowd` (45 items now) and `Section::prepare` (uploads before the frame) for
  sections with their own passes. The menu has 13 entries (`demo_ui.png` changed in both golden sets).
- **Weather** (section 14; [Engine demo](../demo.md#sections)): a plain kilometres wide with hills, a few towers and
  a lake that reflects the clouds, seen from a few hundred metres up; presets (Fair weather, Sun shower, Towering,
  Storm, Overcast, Cirrus), the weather's settings, a raining cumulonimbus to switch on, the wind, the change of
  shape and the weather's speed, the sun (a Sunset button, Run the day), the clouds' quality (Off, 2D layer,
  Volumetric at half or full size; steps, shadows, the sky cube and its faces per frame) and what was drawn. Phone
  defaults (half size, 48 steps, one cube face, 60 Hz) estimated, not measured on a phone. A batch part `weather`
  with three golden captures (`demo_weather.png`, `demo_weather_sunset.png`, `demo_weather_layer.png`; SDL GPU's
  Metal driver and the native backend share them, Vulkan through MoltenVK has its own), a benchmark region,
  `scenarios/weather_cost.txt` (the cost per quality level, offscreen) and a check mode item `weather.clouds` (46
  items now). The menu has 14 entries (`demo_ui.png` changed in both golden sets). With part 2 a place on the ground
  (paving, a pond, a shelter with a crate under it and one in the open), views (from the air, the ground, toward the
  cloud, the shelter), presets Rain here, Downpour and Passing shower, the rain's quality, shafts and rainbow, the
  wetness and the sound with its layers; phone defaults Low without shafts, estimated. A batch part `rain` with
  three golden captures (`demo_rain.png`, `demo_rain_rainbow.png`, `demo_rain_shelter.png`), the weather captures
  replaced (the shafts under their clouds), `scenarios/rain_cost.txt` and check mode items `weather.rain` and
  `weather.rain_sound` (48 items). With part 3 presets Snowfall, Thaw and Snow showers, a view over the shelter, the
  temperature and the snow's quality (phone defaults Low, estimated); a batch part `snow` with four golden captures
  (`demo_snow.png`, `demo_snow_building.png`, `demo_snow_thaw.png`, `demo_snow_sky.png`),
  `scenarios/snow_cost.txt` and a check mode item `weather.snow` (49 items). The rain's three captures replaced (the
  splashes and the ripples).

### Gates

- **`tools/check.sh step new-game`** also runs `tools/check_skill.py` (the engine paths, linked files, flags,
  scenario commands and values, `quench_*` CMake names and engine C++ names the skill uses exist in the checkout)
  and checks that the new game's copy of the skill equals the engine's.

- **`tools/check.sh step xcode`**, also a step of `full`: generates the `ios-xcode` project into
  `build/ios-xcode-check` (configure only: no build, no signing, no team), so that what the Xcode generator refuses
  and Ninja accepts is caught before a package. 0.10.0's iOS package failed that way (fixed in `81fe747`); with that
  fix taken out, the step fails with the same error. `tools/check.sh quick` names the step when CMake files changed
  ([App shell](../app.md#ios-runs)).

## What was verified

From the devlog of [2026-11-07](../../devlog/2026-11-07.md). Mac (Apple M4 Pro).

- **The native backend as the default** (offscreen): hello and the demo without `--gpu-backend` run on it;
  `--gpu-backend sdl`, `QUENCH_GPU_BACKEND=sdl` and `--gpu-driver vulkan` give SDL GPU; `native_fallback` gets an SDL
  GPU device from a native device that fails; `buffer_cycle` passes on SDL GPU's Metal driver, the native backend
  and Vulkan through MoltenVK; every batch checks the backend it ran on.
- **The native backend as the default in the iOS Simulator** (iPhone 18 Pro Max Simulator, iOS 27.0, no backend
  flag): hello and the demo's smoke scenario (every section once, 17 of 17) ran on it, the ray tracing section and
  the tunnel with their fallbacks (no ray tracing there); the captures were looked at.
- **The native backend as the default on the iPhone 17** (iOS 27.2, no backend flag; [Performance](../performance.md#the-native-default-on-the-screen-and-on-the-iphone-17)):
  the device tests with the Metal API validation layer, 262 cases, 258 passed, 0 failed, 4 skipped (SDL GPU by
  `QUENCH_GPU_BACKEND=sdl`: 232 passed, 0 failed, 30 skipped); hello and the demo's smoke run on it; every section
  of the demo's benchmark but the two that trace held 120 Hz, GPU p50 as on 2026-11-06 and CPU p50 within -3 to
  +7 % (0.8.0's demo then), and the ray tracing section and the tunnel held 60 Hz at their phone defaults; the costs
  of the ray-traced effects within 3 % of 2026-11-06's but the first region's (5.5 % lower; the native backend by
  flag then). Numbers only; nobody looked at the screen during the runs.
- **The hello benchmark on the Mac's screen, both backends** (60 Hz, vsync, two runs each, alternated): GPU p50
  0.549 ms on both; CPU p50 0.298 ms on both (means of the runs), p99 0.525 against 0.512 ms.
- **Performance against SDL GPU** (offscreen, unpaced; [Performance](../performance.md#sdl-gpu-and-the-native-backend-before-it-became-the-default)):
  at 480 x 270 (CPU-bound) the native backend's CPU p50 is within -11 to +2 % of SDL GPU's without per-pass
  timestamps and +2 to +8.5 % (8 to 12 µs) with them, as in every perf region; the animation section 0.646 against
  0.673 ms (it was 0.882 against 0.647 before buffers were cycled). At 1920 x 1080 (GPU-bound) 0.3 % more to 3.1 %
  fewer frames. hello: CPU p50 0.022 ms and GPU p50 0.145 to 0.146 ms on both. The memory footprint in the
  animation section with 256 characters: 505 MB on the native backend, 761 MB on SDL GPU.
- **Gates with the native default:** `tools/check.sh full` (2026-11-07, 1130.6 s): format; `debug` 496 tests;
  `release` 605; `asan` 496; `tsan` 496; `wasm-node` 140 (under Node.js); `ios-sim-tests` 369 in the iOS Simulator;
  `ios-xcode` generated; a new game built and started. All passed.

- **The showcase's captures**, looked at on Metal and on Vulkan through MoltenVK, linear and with the checkbox on:
  the linear capture against the dual quaternion one differs in 4.70 % of the pixels (mean 4.02; 5.08 % before the
  camera moved back); the native backend's captures are identical to SDL GPU's (0 pixels differ), Vulkan through
  MoltenVK within 15 levels (mean at most 0.0017). `demo_animation_retarget`'s golden images changed with it: the two showcase characters far in its
  background stand further apart (0.28 % of the pixels, the walking pair unchanged).
- **The xcode step:** about 11 to 13 s with its directory in place, 254.5 s the first time (downloads); configured
  with `QUENCH_IOS_TEAM` empty.
- **The skill:** a dry run built a game from it alone (a lit scene with a cooked texture and model, a Jolt box, a
  cooked sound, an input action, a scenario with a capture and a golden image test): Debug and Release ctest 3 of
  3, the iOS Simulator build (ASTC textures cooked) launched offscreen; `tools/check_skill.py` passes on it and
  reported two deliberately broken copies line by line; `step new-game` 142.1 s.
- **The wrists:** the generated characters' weights sum to 1 with at most two joints per vertex; the 12 vertices of
  each wrist ring are half forearm, half hand. Close-ups at 2560 x 1440 looked at: no gap on either character,
  untwisted or twisted. The skinning part's golden images were replaced (camera and wrists: mean 37.56 and 38.28
  against the previous images); the other captures with characters stay within their golden images (at most 62
  pixels over 16, 0.012 %), the native backend's skinning captures equal SDL GPU's.
- **Gates:** `tools/check.sh full` with the xcode step (2026-11-07, 378.1 s): format; `debug` 486 tests; `release`
  596 (the captures of every backend, Vulkan through MoltenVK included); `asan` 486; `tsan` 486; `wasm-node` 140
  (under Node.js); `ios-sim-tests` 363 in the iOS Simulator; `ios-xcode` generated; a new game built and started.
  All passed.

- **The reaction to heat** (offscreen, fake states): 10 unit cases of the policy (`thermal_governor_*`,
  `thermal_limits_text`: escalation at once, the hold and its restart, recovery to the highest state in the hold,
  the player's frame rate first, unknown as nominal, accumulated limits, disable and callbacks) and the flag's
  parsing; the demo's `heat` part 48 of 48 checks in each batch: SDL GPU's Metal driver, the native backend (there
  1 lamp shadow of 4 and the sky's reflections and light at critical, all back after the recovery) and Vulkan
  through MoltenVK; the template's smoke scenario 11 of 11 in a game made by `tools/new_game.py`. No golden image
  changed: the reaction is off by default.
- **Gates with the thermal reaction:** `tools/check.sh full` (2026-11-07, 353.5 s): format; `debug` 506 tests;
  `release` 618; `asan` 506; `tsan` 506; `wasm-node` 140 (under Node.js); `ios-sim-tests` 379 in the iOS
  Simulator; `ios-xcode` generated; a new game built and started. All passed.

- **Surface shaders in ray hits** (native Metal backend, offscreen): `renderer_ray_hits_run_surface_functions`:
  the test surface `flat` (green from its params; the material's base color red) on two boxes over a mirror, one
  with its hits in the set: the raster shows both green (15 243 15), the mirror that one green (16 243 16) and the
  other red from the material's values (200 20 20), the path tracer the same (15 243 15 and 208 17 17); `addSurface`
  refuses a `rayHit` without a set and a second set. The demo's `demo_raytracing_mirror_native.png` looked at: the
  mirror shows the ground's cells and the sun's shadows on them; `demo_raytracing_native.png` changed within its
  thresholds (mean 0.136, 3 pixels over 16) and was replaced.
- **Gates with surface shaders in ray hits:** `tools/check.sh full` (2026-11-07, 547.3 s): format; `debug` 512
  tests; `release` 627; `asan` 512; `tsan` 512; `wasm-node` 140 (under Node.js); `ios-sim-tests` 383 in the iOS
  Simulator; `ios-xcode` generated; a new game built and started. All passed.
- **Probe cascades, relocation and classification** (native Metal backend, offscreen): `renderer_probe_cascades`: a
  strip 12 m from the grid's center gets no bounce light from a 1 m grid (0.0334, its direct light) and 0.2826 from
  the same grid's second cascade; near the center 0.3552 with 2 cascades against 0.3556 with one; 2 cascades in 4
  turns hold within 5 %; classification within 0.5 % there and 2.8 % far away with 3 cascades.
  `renderer_probe_relocation`: a corridor whose probes sit in its walls has a dark floor (0.0000) and 0.2380 with
  relocation, against 0.2716 in a corridor whose probes are in its middle; 0.2431 with classification on top. The
  existing probe tests give the numbers they gave before. The demo's `demo_raytracing_probes_native.png` looked at
  (the ground lit out to the horizon; the grey band of the one-grid frame gone), and against the path tracer's 512
  samples the frame with 1 m in 3 cascades is closer (mean difference 4.62 against 5.24 levels, pixels more than 16
  off 5.2 % against 10.1 %); the other captures of the batches are unchanged.
- **Gates with probe cascades:** `tools/check.sh full` (2026-11-07, 423.9 s): format; `debug` 518 tests; `release`
  634; `asan` 518; `tsan` 518; `wasm-node` 140 (under Node.js); `ios-sim-tests` 387 in the iOS Simulator;
  `ios-xcode` generated; a new game built and started. All passed.
- **Basis Universal** (M4 Pro, Release; [Assets](../assets.md#basis-universal)): the comparison on the demo's 19
  textures (1.88 Mtexels in level 0): BC7 and ASTC 2,519,504 bytes, ETC1S 188,980, UASTC 1,255,728; color 56.7 dB
  (BC7), 60.0 (ASTC), 37.9 (ETC1S), UASTC 58.2 to ASTC and 55.2 to BC7; normal maps 0.77° (BC7), 0.51° (ASTC),
  3.3° (ETC1S), 0.68° (UASTC to ASTC). Transcoding 1.8 (ETC1S to ETC2) to 8.8 ms (UASTC to ETC2) per Mtexel, a
  64 x 64 file with mips 0.03 to 0.10 ms (0.02 to 0.18 ms as WebAssembly under Node.js), the transcoder's tables
  once per process 17 to 34 ms. `assets.basis_*` (also under Node.js), `cook.cook_basis_*`,
  `render.scene_assets_basis_textures` on SDL GPU's Metal driver, the native backend (ETC1S to ETC2 RGB8) and
  Vulkan through MoltenVK (ASTC), the demo's `assets_basis` part on all three; the golden captures unchanged. In
  the iOS Simulator the same cases and targets (ETC2 on the native backend, ASTC on SDL GPU).
- **Gates with Basis Universal** (after surface shaders in ray hits): `tools/check.sh full` (2026-11-07, 565.1 s):
  format; `debug` 524 tests; `release` 642 (the captures of every backend, Vulkan through MoltenVK included); `asan`
  524; `tsan` 524; `wasm-node` 146 (under Node.js); `ios-sim-tests` 391 in the iOS Simulator; `ios-xcode`
  generated; a new game built and started. All passed.
- **The per-pixel indirect light's denoiser** (offscreen, native backend): the new case
  `renderer_indirect_light_settles` (0.3 % from frame to frame, the spatial filter's long mean within 1.5 %; the
  filter before the change: 9.8 % and a quarter of that mean), the other denoiser and indirect light cases
  unchanged; the demo's noise scenarios, captures looked at, still and in motion; no golden image changed (the
  demo's captures trace the probe grid, not per-pixel indirect light). On the iPhone 17's screen (an interim build, the ray tracing
  section with Indirect light "Ray traced"), the user: "the grain is fine now" (2026-11-07 17:47).
- **Profiling with Tracy** (Mac, offscreen; [Performance](../performance.md#profiling-with-tracy)): the `profile`
  preset's ctest 656 of 656 (Vulkan through MoltenVK included), `hello_tracy_capture` recording hello with its CPU
  zones, the frame plot and the GPU zone `game` on the native backend; the ray tracing section recorded at
  1920 x 1080 (its GPU zones per frame within 0.03 ms of the perf lines' busy times). Off, against the build before:
  the demo's sections' CPU p50 -0.1 % and p99 -0.9 % summed, hello's p50 the same; compiled in without a server
  +0.5 % p50; recorded +0 to +2.4 % per section. The iOS Simulator and device builds with `-DQUENCH_TRACY=ON`
  compile; `QUENCH_TRACY_MEMORY` recorded hello once.
- **Gates with Tracy** (Tracy off): `tools/check.sh full` (2026-11-07, 1342.9 s): format; `debug` 536 tests;
  `release` 655; `asan` 536; `tsan` 536; `wasm-node` 149 (under Node.js); `ios-sim-tests` 400 in the iOS Simulator;
  `ios-xcode` generated; a new game built and started. All passed.

- **Navigation** (M4 Pro): `navigation.*` (16 cases), `cook.cook_navmesh`, `cook.cook_navmesh_errors` and the
  navmesh in `cook_cli` in `headless`, `debug`, `release`, `asan` and `tsan`, as WebAssembly under Node.js and in
  the iOS Simulator. A crowd keeps area costs (0 steps in the mud at cost 5; 86 with Detour's visibility shortcut
  left on); a tiled build gives the same bytes and tile hashes with 0 and 4 workers. Timings in Release: a 120
  polygon level built in 1.3 to 3.4 ms, a path 7 to 15 µs, crowd updates 0.02 to 0.05 ms (25 agents) and 0.13 to
  0.20 ms (100). The demo's `navigation` part, 26 checks on SDL GPU's Metal driver, the native backend and Vulkan
  through MoltenVK (offscreen); `demo_navigation.png` looked at, the native capture equal to Metal's; `demo_ui.png`
  replaced (the 13th menu entry: mean 3.00 and 3.02, 5.0 % of the pixels over 16, all inside the menu).
- **Gates with navigation:** `tools/check.sh full` (2026-11-07, 508.0 s): format; `debug` 551 tests; `release` 673;
  `asan` 551; `tsan` 551; `wasm-node` 162 (under Node.js); `ios-sim-tests` 413 in the iOS Simulator; `ios-xcode`
  generated; a new game built and started. All passed.

- **The weather and the clouds** (M4 Pro; [Weather](../weather.md)): `weather.*` 12 cases in every preset, under
  Node.js and in the iOS Simulator (coverage means within 0.005 of the setting from 0.2 to 0.8; a raining cell found
  3692 m away in 0.032 ms; `sample` with 16 cells 206 ns); `renderer_clouds_shadow_the_ground` (the ground 0.073
  under a cell, 0.775 in the sun) and the GPU cases `texture_3d_from_layers` and `cube_faces` on SDL GPU's Metal
  driver, the native backend and Vulkan through MoltenVK (validation layer on), and in the iOS Simulator on both of
  its backends. The demo's three weather captures looked at (offscreen); the native backend's are the same bytes as
  SDL GPU's, two batches gave the same bytes. The other sections' captures unchanged; `demo_ui.png` replaced (the
  menu's 14th entry: mean 2.93 and 2.95, 4.9 % of the pixels over 16, all inside the menu).
- **Gates with the weather:** `tools/check.sh full` (2026-11-07, 481.9 s): format; `debug` 575 tests; `release` 700;
  `asan` 575; `tsan` 575; `wasm-node` 177 (under Node.js); `ios-sim-tests` 434 in the iOS Simulator; `ios-xcode`
  generated; a new game built and started. All passed.

- **The rain** (M4 Pro; [Weather](../weather.md#rain)): `weather.*` 15 cases (60 s of rain give ground 0.500 and
  water 0.067; a passing shower's wetness updated every frame 0.498 and 0.033, estimated by `setTime` 0.465 and
  0.020); `renderer_rain_wets_the_open_ground` (open ground 0.521 dry and 0.288 wet, the same 0.521 under the roof;
  1041 of 4096 pixels with drops) and `renderer_rain_shafts_from_afar` on SDL GPU's Metal driver, the native backend
  and Vulkan through MoltenVK, and in the iOS Simulator on both of its backends; `audio.weather_*` (the mix rules,
  crossfades, a game's sound, hot reload, off, the default loops' levels and spectra). The demo's `rain` part (44
  checks: the mix's RMS 0.0695 in the open, 0.0932 under the shelter, 0.0165 from a shower 1.4 km away; no
  underruns or clipping) and its three captures, looked at (offscreen); the native backend's are the same bytes as
  SDL GPU's, Vulkan through MoltenVK within a mean of 0.036. The weather captures replaced with the shafts (mean
  0.98 to 1.52 against part 1's, looked at); the other sections' captures unchanged.
- **Gates with the rain:** `tools/check.sh full` (2026-11-07, 521.1 s): format; `debug` 593 tests; `release` 721;
  `asan` 593; `tsan` 593; `wasm-node` 183 (under Node.js); `ios-sim-tests` 448 in the iOS Simulator; `ios-xcode`
  generated; a new game built and started. All passed.

- **The snow** (M4 Pro; [Weather](../weather.md#snow); devlog of [2026-11-08](../../devlog/2026-11-08.md)):
  `weather.*` 18 cases (300 s of snowfall give a cover of 0.500; 225 s at 5 degrees after it 0.250, the ground
  0.500 and standing water 0.125); `renderer_snow_covers_the_open_ground` (open ground 0.318 bare and 0.875 under
  snow, 0.318 under the roof, a material with `snow = 0` bare, flakes over 451 of 4096 pixels, no streaks) and
  `renderer_snow_shafts_from_afar` on SDL GPU's Metal driver, the native backend and Vulkan through MoltenVK, and in
  the iOS Simulator; `audio.weather_audio_gains` (no rain layer in a snowfall, sleet as its rain's share, the
  distant rain muffled). The demo's `snow` part (35 checks) and its four captures looked at (offscreen); the native
  backend's are the same images as SDL GPU's, Vulkan through MoltenVK within a mean of 0.006. The rain's three
  captures replaced with the visible splashes and the ripples that differ by puddle (mean 0.65 to 1.24 against the
  old ones, looked at); the other sections' captures unchanged.
- **Gates with the snow:** `tools/check.sh full` (2026-11-08, 512.0 s): format; `debug` 602 tests; `release` 733;
  `asan` 602; `tsan` 602; `wasm-node` 186 (under Node.js); `ios-sim-tests` 455 in the iOS Simulator; `ios-xcode`
  generated; a new game built and started. All passed.

## What was not verified

- **The per-pixel indirect light's denoiser on a phone:** measured offscreen on the Mac only, no numbers from a
  phone; looked at on the iPhone 17 only (above), not on the iPhone 18 Pro Max. After a sudden change of light it is
  a few levels behind the old filter in the first 8 frames.

- **The reaction to heat on a hot phone:** it has run with fake states only. Its defaults are a starting point: in
  the iPhone 17's ten-minute run of 2026-11-06 the tunnel missed intervals at fair, about a minute before serious,
  where the defaults begin. On a phone the frame rate limit acts through the display link; offscreen runs do not
  limit frames, so the checks read `frame_rate_limit`, not the frame rate. Nobody has looked at the demo's
  Performance and Graphics lines on a phone.

- **The native backend as the default on the iPhone 18 Pro Max:** it ran there only when chosen, and nobody has
  looked at the default on a phone's screen. The iPhone 18 Pro Max has not run the ray tracing section's and the
  tunnel's phone defaults, which the demo now shows there without a flag (the iPhone 17 has). On the iPhone 17 the
  tunnel's phone defaults turned the phone fair within their minute at 60 Hz in a run that followed a quarter of an
  hour of other load (nominal on 2026-11-06); not repeated from a cold phone.
- Per-pass timestamps cost the native backend 8 to 12 µs of CPU per frame, and 6.5 % of the frames in the animation
  section (perf regions and `App::requestPassTiming` only); `cycle` on a texture is still a no-op there.

- Nobody has looked at the new showcase yet, on the Mac's screen in motion or on a phone; the user's report was
  about the previous one. The closed wrists were looked at in still captures only, not while a hand turns (the
  hero's wave) or in motion.
- The skill: one dry run on the Mac (Debug, Release, the iOS Simulator); its animation, jobs, surface shader and
  device parts were not run. `tools/check_skill.py` finds renamed or removed names, not changed behaviour.
- Surface shaders in ray hits on a phone: built into the iOS presets, not run on an iPhone (the device tests include
  the new render case); nobody has looked at the checker in the mirror on a screen in motion. A game's surface with
  resources of its own, or several surfaces in one set, is not tested beyond the one-file sets of the test and the
  demo.
- The xcode step generates the project only: a build or signing problem in `xcodebuild` is still found only by a
  device build or `tools/package_demo.sh --ios`.
- Probe cascades, relocation and classification on a phone: built into the iOS presets, not run on an iPhone (the
  device tests include the two new render cases); the phone costs are estimates from the iPhone 17's ratio, and the
  proposed phone default (classification in 4 turns) is not set. Nobody has looked at them on a screen in motion
  (a cascade's edge passing over a surface as the camera moves, a probe's relocation changing as the rays turn).
- Basis Universal has not run on an iPhone (the transcode time there, ETC2 on the native backend) and nobody has
  looked at the ETC1S and UASTC tiles on a screen; the numbers are of the demo's procedural textures on the M4 Pro.
  No ETC1S quality option and no rate-distortion optimization for smaller UASTC files yet.
- **Tracy on a phone and Tracy's GUI:** the iOS builds compile, nothing recorded from an iPhone (the app has no
  local network usage description yet); the GUI was not built or started (it opens a window), the captures were
  read by `tracy-csvexport` only. GPU zones exist on the native backend only (SDL GPU: the frame's GPU time as a
  plot). Tracy's statistics leave out zones shorter than its timer's tick (about 42 ns on Apple silicon).
- The ray tracing section's render scale by the window's pixel count is a proposal from offscreen numbers on the
  M4 Pro ([Performance](../performance.md#the-ray-tracing-section-by-frame-size)); the defaults are unchanged.

- **Navigation in motion and on a phone:** nobody has looked at the crowds moving (avoidance, turns, the walk and
  run blend, the link arcs, the moving obstacles); the check mode's `navigation.crowd` asks for it. Not run on an
  iPhone, not benchmarked on the screen. Link crossings are Detour's straight line at half speed; tiles have no
  detail meshes; the demo builds its level at run time (the cooked path is tested, not shown). Detour carries an
  alignment patch until upstream fixes it.

- **The clouds on a phone and on a screen:** not run on an iPhone; the Weather section's phone defaults are an
  estimate (about 4.3 ms on the iPhone 17 by its ratio to the M4 Pro, over the roadmap's 2 ms); nobody has looked at
  the clouds in motion (drift, change of shape, the accumulation when the camera turns) or on a phone's screen. The
  storm's bases look smooth in the captures; a camera above the clouds is not tuned. The wind is part 4
  ([Weather](../weather.md#later-parts)).
- **The rain in motion, by ear and on a phone:** nobody has looked at the streaks, splashes and rings moving or
  listened to the rain (measured only: levels, spectra, the mix's RMS); not run on an iPhone, the phone defaults'
  0.7 ms there an estimate by the iPhone 17's ratio. The storm from the air is a grey veil more than shafts; the 2D layer's shafts look smoky; the occlusion map reaches 64 m from the camera,
  so roofs beyond it do not keep the rain off (the wetness there is the weather's open ground); streaks and
  splashes are not in reflections, and the path tracer's frame has no rain; the heavy layer's default loop peaks
  at 0.98.
- **The snow in motion and on a phone:** nobody has looked at the flakes moving or the cover building on a screen;
  not run on an iPhone, the phone defaults' cost there an estimate. The cover is a whitening of open ground, not
  geometry (no depth, drifts or footprints); beyond 64 m of the camera floors under roofs whiten too; the grid melts
  by the temperature at y = 0. Sleet is a mix of the rain's and the snow's drawing.

## Changes for games

- **Profiling with Tracy (optional, nothing to change):** a game that configures the engine with
  `-DQUENCH_TRACY=ON` (or its own preset like the engine's `profile`) gets the engine's zones and can add its own with
  the macros of `quench/core/Profile.h`, which are nothing in its other builds. `quench_core` then links
  `quench_tracy` publicly and defines `QUENCH_TRACY=1` for its users; the engine's Jolt is built with
  `PROFILER_IN_DISTRIBUTION` and `JPH_USE_EXTERNAL_PROFILE` (Jolt's zones into Tracy), so a game that builds Jolt
  code of its own against the engine's Jolt sees `JPH_PROFILE` active there. New overloads `JobSystem::run(name,
  fn, ...)` and `parallelFor(name, count, minRange, fn, ...)` name a job's zone; the old ones are unchanged. Off (the
  default) nothing changes.
- **The default backend on Apple platforms is the native Metal backend.** A game that leaves
  `AppDesc::gpuBackend` (or `DeviceDesc::backend`) at `Backend::Default` runs on it on the Mac, the iPhone and in the
  iOS Simulator after moving its pin, through the same `quench::gpu` API and shaders; the engine's captures are the
  same on both backends. What differs: `gpu::Caps` (`backend`, `gpuPassTiming`, `rayTracing`, the upscaling caps,
  `driver` stays `metal`), per-pass GPU times in perf regions, and the memory a run takes. A game that wants SDL GPU
  there sets `AppDesc::gpuBackend = gpu::Backend::SdlGpu`, or passes `--gpu-backend sdl` or `QUENCH_GPU_BACKEND=sdl`;
  `--gpu-driver vulkan` still gives SDL GPU. A game whose own tests or scripts assumed SDL GPU on Apple platforms
  names it now.
- **Area lights with `castsShadows` take shadow maps** where a game sets `RenderSettings::localShadowMapSize` and
  the lights are not traced: they cast soft shadows now and cost a screen-space pass in those frames (0.9 to 1.7 ms
  of the M4 Pro for four lamps). A game that wants them unshadowed sets `castsShadows = false` on them. Custom
  surface shaders built on the engine's `Surface.hlsli` need no change: in those frames opaque surfaces read the
  screen-space term as they read traced shadows, and `quenchLocalShadows` skips area lights.
- **Surface shaders in ray hits (optional):** `quench_add_ray_hits()`, `SurfaceDesc::rayHitShaders` and `rayHit`,
  `RenderStats::rayHitSurfaces`, the macro `QUENCH_SURFACE_HIT` (0 in every other entry). A game that does nothing
  keeps the standard surface in its hits, as before. A surface listed in a set compiles once more inside the
  engine's tracing source: its own `cbuffer`s and textures go behind `#if !QUENCH_SURFACE_HIT`, and its macros must
  not clash with another listed surface's. `Surface.hlsli` declares its bindings and lighting only where
  `QUENCH_SURFACE_HIT` is 0, so shader code of a game's own that defines `QUENCH_SURFACE_HIT` for another purpose
  breaks.
- **The probe grid's frame uniforms** (`quench/SceneUniforms.h`, read through `quench/Probes.hlsli`):
  `FrameUniforms::probeGrid` is an array of `QUENCH_MAX_PROBE_CASCADES` (4), one per cascade, and `probeAtlas.z` and
  `.w` hold the cascades and the relocation and classification bits; the struct grew by 48 bytes. A game shader that
  reads `quenchFrame.probeGrid` itself reads `probeGrid[0]` for the finest cascade. `Probes.hlsli`'s helpers take a
  cascade: `quenchProbePosition(cell, cascade)`, `quenchProbeUv(slot, cascade, dir, tile, rows)`,
  `quenchProbeAtlasSize(tile, rows)`; `quenchProbeIrradiance` is unchanged. New and optional:
  `RenderSettings::probeCascades`, `probeRelocation`, `probeClassification` and `render::kMaxProbeCascades`; a game
  that sets none keeps one grid as before.
- **`IndirectLight::RayTraced` looks different:** no API change, but its light no longer stays grainy in a still
  scene, and where few of its rays see the sky (under roofs and fences) it is brighter than before, closer to the
  probe grid's; a game's captures with it change.
- **New:** `gpu::defaultBackend()` (`quench/gpu/Device.h`). `Device::create` no longer returns an error when only
  the native device fails to start: it returns an SDL GPU device (check `caps().backend` where it matters).
- **iOS presets of games made from the template before this release:** a game that cooks assets
  (`quench_cook_assets()`) and builds for iOS adds the host cooker to its `CMakePresets.json`, as the template now
  has it. In the configure preset that the iOS presets inherit (`ios-base` in the template), next to
  `QUENCH_HOST_SHADERC`:

  ```json
  "QUENCH_HOST_COOK": "${sourceDir}/build/host-tools/_deps/quench-build/quench_cook"
  ```

  and in the `host-tools` build preset:

  ```json
  { "name": "host-tools", "configurePreset": "host-tools", "targets": ["quench_shaderc", "quench_cook"] },
  ```

  Without them its iOS configure stops at `quench_cook_assets()`. A game without cooked assets needs nothing.
- **The thermal state's header:** `app::ThermalState` and `thermalStateName` moved from `quench/app/App.h` to
  `quench/app/ThermalGovernor.h`, which `App.h` includes: no change for code that includes `App.h`. New and optional:
  `App::thermalGovernor()`, `App::frameRateLimit()`, `PerfReport::reactionStart`, `reactionEnd`, `reactionMax`
  (`Unknown` while the reaction is off), the flag `--thermal-state` and the scenario command `thermal_state`. A game
  that does not enable the governor behaves as before; a game whose own flag or scenario command is named
  `--thermal-state` or `thermal_state` loses it to the engine's.
- **Basis Universal:** the first configure of a game on a machine downloads Basis Universal's archive once
  (197 MB, 12 MB kept in the tool cache next to DXC; `QUENCH_BASISU_DIR` names a source tree instead), because
  `quench_assets` links its transcoder. New and optional: `TEXTURE_FORMAT basis` and `BASIS_CODEC` in
  `quench_cook_assets()`, `--format basis` and `--basis-codec` in `quench_cook`, `assets::prepareTexture` and the
  Basis functions in `quench/assets/Texture.h`, `TextureAsset::basisCodec()`, `decodeMilliseconds()`,
  `SceneAssets::etc2Textures()` and `textureSupport()`. New enum values: `assets::PixelFormat::Etc2Rgb8`,
  `Etc2Rgba8` and their sRGB variants, the four `gpu::TextureFormat::Etc2*`, `gpu::Caps::etc2Textures`; a game
  that switches over these enums without a default gets a `-Wswitch` warning. `TextureAsset::decodedOnCpu()` is
  also true for a Basis Universal file transcoded to RGBA8. Neither game uses these.
- **Navigation (optional):** the new library `quench_navigation` (`target_link_libraries(my_game PRIVATE
  quench_navigation)`), `NAVMESHES` in `quench_cook_assets()`, the `navmesh` command of `quench_cook` and `.qnav`
  files. The first configure of a game downloads recastnavigation's archive (1.3 MB, pinned with its SHA256) with the
  other runtime pins, because the engine defines the library whether a game links it or not. A game that does not
  use it changes nothing.
- **Clouds (optional):** `quench_render` links `quench_weather` and `quench/render/Scene.h` includes
  `quench/weather/Weather.h`; new `Scene::setWeather`, `clearWeather`, `weather()`, `render::Clouds` and the
  `RenderSettings`, `ActiveEffects` and `RenderStats` members above. A game that sets no weather and leaves
  `RenderSettings::clouds` off draws what it drew before. `FrameUniforms` (`quench/SceneUniforms.h`) grew by 48 bytes
  (`cloudShadow`, `skyCube` and `overcast` at its end), and surface shaders built on `Surface.hlsli` read samplers 14
  (the clouds' shadow) and 15 (the sky cube) where those members say so; the renderer binds placeholders there when
  there are no clouds. A surface's fragment samplers 0 to 15 are now all the engine's (SDL GPU allows 16 per stage):
  a game surface shader that declared its own textures in 14 or 15 loses them. A game's own SDL declaration
  (TrailForge's) does not get the engine's new SDL patch.
- **Rain (optional), and surface shaders that see it:** `FrameUniforms` grew by 96 bytes (`rainViewProj`, `rain`,
  `rainGrid` at its end) and `MaterialUniforms` by 16 (`weather`), in `quench/SceneUniforms.h`. Surface sampler 14
  is now the weather maps (`QUENCH_WEATHER_MAPS_*`, `QUENCH_RAIN_MAP_ROW`, `QUENCH_WETNESS_ROW`): the clouds'
  shadow is its first 512 rows, and `quenchCloudShadow(maps, sampler, P)` reads that region (a game shader that
  sampled sampler 14 itself reads it through `quenchCloudShadow` now). The surface entries apply the rain after the
  surface function: a game surface built on `SurfaceEntries.hlsli` gets wet where the rain falls once a game turns
  `RenderSettings::rain` on; one that must stay dry defines `QUENCH_SURFACE_NO_RAIN`, and a material
  sets `MaterialDesc::wetness = 0`. New: `render::Rain`, `RenderSettings::rain`, `rainShafts`, `rainbow`, the
  `ActiveEffects` and `RenderStats` members, `MaterialDesc::wetness`, `puddles`, `water`; in `quench_weather`
  `Wetness`, `WetnessCell`, the wetness calls and settings above, `rainVelocity`, `kRainFallSpeed`; every
  `Weather` gets a `wetnessVersion` no other has had (a copy keeps it). `audio::WeatherAudio` and
  `quench_weather_sounds()` (needs Python 3 at build time, as the demo's sounds do). A game that leaves the rain off
  draws what it drew before; `Weather::update` now also steps the wetness grid (256 samples of the field per 0.25 s
  of weather time, about 0.05 ms by the `sample` cost above).
- **Snow (optional), and surface shaders that see it:** `FrameUniforms` grew by 16 bytes (`snow` at its end);
  `FrameUniforms::rain.w` is now 1 where the rain is drawn (it was the ripples' strength, the same value with the
  rain on); `MaterialUniforms::weather.w` is the material's snow; the weather maps' wetness rows have the rain's share
  in `a` and the snow cover in the 16 x 16 texels beside the grid. The surface entries apply the snow after the rain:
  a game surface built on `SurfaceEntries.hlsli` gets the cover where it faces up once a game turns
  `RenderSettings::snow` on and its weather is cold enough; one that must stay bare defines
  `QUENCH_SURFACE_NO_SNOW`, a material sets `MaterialDesc::snow = 0`. New: `render::Snow`, `RenderSettings::snow`,
  `ActiveEffects::snow`, the `RenderStats` members; in `quench_weather` `Settings::temperature`, `lapseRate`,
  `snowTime`, `meltTime`, `Cell::snow`, `Precipitation`, `Wetness::snow`, `WetnessCell::snowShare`, `snowShare`,
  `temperature`, `snowVelocity`, `kSnowFallSpeed` and the `kind` argument of `distanceToPrecipitation` (`Any` by
  default, as before); `audio::WeatherSoundState::snow` (0 by default: everything plays as rain, as before). A game
  that leaves the temperature at its default gets no snow; one that sets it lower gets snow wherever the air is
  below 2.5 degrees (under the default cloud base from about 12 degrees at the ground, in the shafts first): pass
  `Precipitation::Rain` for the distant rain's sound and the snow share to `WeatherAudio` then. The rain occlusion map is drawn without the slope bias now (pipelines of its own for game
  surfaces, made when the rain or snow first falls); `RenderStats::rainStreaks` and `rainSplashes` are 0 where no
  rain falls near the camera.
- **The skill:** a game made before this release has no copy; copy `.agents/skills/quench-game/` from the engine
  at the pinned commit if its agents should use it (and again when the pin moves).
- The rest is in the demo and the engine's own test and gate scripts; `quench_add_batch()` in
  `tests/CMakeLists.txt` (now with `GPU <backend>:<driver>`) is not part of what games use.
