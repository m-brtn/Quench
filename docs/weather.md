# Weather

Stage 13 of the [Roadmap](roadmap.md) (requested by the user on 2026-11-07): rain, snow, sun and wind, clouds that
form, drift and change shape, light that follows them, rain under a single cloud in a blue sky, ripples in puddles
and weather sounds a game can replace. This page is the design of the whole stage and the description of what is
built. **Part 1, the weather map, the clouds and their light, is built** (2026-11-07): `quench_weather`, a headless
module, and the clouds in `quench_render`, with the demo's "Weather" section. Parts 2 to 4 (rain, snow, wind) are a
plan ([Later parts](#later-parts)). Written for the engine, not moved from a game; neither game uses it. It ran on
the Mac (Metal, the native Metal backend and Vulkan through MoltenVK), in the iOS Simulator (the test executables) and
as WebAssembly under Node.js (the headless tests); not on an iPhone, and nobody has looked at the clouds in motion.
All documentation: [index](README.md).

## Use

```cpp
#include <quench/render/Renderer.h>
#include <quench/render/Scene.h>
#include <quench/weather/Weather.h>

namespace wx = quench::weather;

wx::Settings s;
s.seed = 7;
s.coverage = 0.32f;                       // the share of the sky the clouds cover, 0 clear .. 1 overcast
s.type = wx::CloudType::Cumulus;          // Stratus, Cumulus, Towering, Cumulonimbus
s.wind = {.direction = {0, 1}, .speed = 12};
wx::Weather weather(s);
weather.setTime(1200.0);                  // where the weather's clock starts (s)
// One raining cumulonimbus in a blue sky, drifting with the wind
wx::CellId shower = weather.addCell({.position = {2600, -10500}, .radius = 3200,
                                     .type = wx::CloudType::Cumulonimbus, .precipitation = 1});

quench::render::RenderSettings rs;        // clouds are off by default
rs.clouds = quench::render::Clouds::Volumetric;
rs.cloudResolution = 0.5f;                // half size, accumulated over frames (cloudTemporal)
renderer.setSettings(rs);

// Every frame
weather.update(dt);
scene.setWeather(weather);                // a copy: set it again after each update
float rain = weather.precipitation(listenerPosition);           // 0..1 falling here
float nearest = weather.distanceToPrecipitation({x, z});        // m to the nearest rain
float overcast = weather.cover({x, z}, 6000.0f);                // mean coverage around a point
glm::vec3 wind = weather.windAt(position);                      // m/s
```

`Scene::clearWeather()` removes it (no clouds). A game links `quench_weather` for the queries alone (headless, no
GPU); `quench_render` links it publicly.

## Where the weather lives

`quench_weather` is a module of its own, beside `quench_navigation` and `quench_animation`, and headless: it is
built and tested in every preset, `headless`, `asan`, `tsan` and `wasm-node` included. Reasons:

- **A game asks the weather without the GPU.** Rain at the listener, the distance to the nearest rain, how overcast
  it is and the wind are game logic, audio and physics inputs; a server, a test or a headless tool needs them too.
  In `quench_render` they would need a renderer; in `quench_core` they would put a world model into the foundation
  every module links.
- **One source for the CPU and the GPU.** The field is one function in `shaders/quench/WeatherField.h`, compiled as
  C++ by `quench_weather` and as HLSL by the render module's weather pass: integer hashes (exact on both sides), value
  noise and smoothsteps in 32-bit float. A query and the clouds on the screen agree (they differ by rounding, about
  1e-6). Only the field is shared; the clouds' 3D shapes are the renderer's (below).
- **Deterministic.** The weather is a pure function of its settings, its cells and its clock: the same seed,
  settings, cells and time give the same weather in every run and on every platform. `setTime(t)` gives the weather
  at `t` as if the current settings had always held, so a scripted check, a replay or a saved game gets the same
  sky however it got there.
- **Threads:** not thread-safe for changes (one thread changes it); const queries may run on several threads at once
  while nothing changes it (`weather_queries_in_parallel` on the job system).

## The weather map

A top-down field over the whole world, `Sample{coverage, type, precipitation, cirrus}` at any world xz, each 0..1:

- **Coverage** (the share of the column that holds cloud): the logit of `Settings::coverage` moved by four octaves
  of value noise at `featureSize` (6 km: groups of clouds and clear gaps between them). The field's mean follows the
  setting (measured: 0.195, 0.348, 0.497, 0.647, 0.802 for 0.2 to 0.8); from 0.85 on it closes into an overcast
  deck, at 1 everywhere. The renderer's noise makes the single clouds of a group and covers about this share of the
  sky there.
- **Type** on one continuous scale: stratus 0 (a grey deck 500 m thick), cumulus 1/3 (1.5 km), towering cumulus 2/3
  (5 km), cumulonimbus 1 (up to `cloudTop`, 9 km by default, with an anvil). The setting's type, varied over larger
  patches by `typeVariation`; the thickest parts of a field grow taller. Bases at `cloudBase` (1.5 km).
- **Precipitation** from the thick parts (coverage above about 0.55), more from taller clouds, times
  `Settings::precipitation`. Rain or snow is part 2's and 3's choice.
- **Cirrus** (`Settings::cirrus`): high ice cloud at `cirrusHeight` (8 km), in patches stretched along the wind.
- **Wind and evolution:** the field is sampled in the air's coordinates, so it drifts with `Settings::wind` (the
  air's offset integrates the wind, so a new wind moves the air from where it is), and its noise moves through
  time: at `evolution` 1 a field of clouds has re-formed after about 10 minutes (`kEvolutionPeriod` 600 s; the mean
  coverage change between two times one period apart: 0.186). 0 freezes the shapes; the wind still moves them.
- **Cells** (at most 16): round clouds a game places on top of the field, with lobes that change over time and a
  phase of their own: a single cumulonimbus raining in a blue sky (coverage 0 elsewhere), an overcast region over
  mountains. A cell drifts with the wind or stays over its position (`drifts = false`); its precipitation falls
  within about half its radius.

Queries: `sample(xz)`; `precipitation(point)` (the map's below the cloud base, 0 above; no roofs: part 2's
occlusion map handles cover); `distanceToPrecipitation(xz, threshold, maxDistance)` (rings of 32 directions with a
bisection, at most 48 rings: 0.03 ms on the M4 Pro with rain 3.7 km away, about 0.3 ms when there is none; a few
times a second, not per object per frame); `cover(xz, radius)` (19 samples); `windAt(point)` (the setting's wind
at the cloud base, a logarithmic profile below it: about 0.3 of it at 2 m); `cloudTop(type)`; `highestType()` (an
upper bound over the map, what the renderer marches through); `fill(origin, size, resolution, out)` (a grid). A
sample costs about 0.2 µs with 16 cells (M4 Pro, Release).

## Clouds

`RenderSettings::clouds` (`Off` by default) draws the clouds of the scene's weather, with `sky` on:

| Setting | |
|---|---|
| `clouds` | `Off`, `Layer` (a 2D layer at the cloud base: the fallback and the cheap setting), `Volumetric` (raymarched) |
| `cloudResolution` | Volumetric: the march's share of the render size per side, 0.25 to 1 (0.5 by default) |
| `cloudTemporal` | Volumetric: each frame's march blended into the previous ones', reprojected (on by default) |
| `cloudSteps` | Volumetric: steps along a view ray through the layer, 8 to 256 (64) |
| `cloudShadows` | the clouds shade the sun's light on surfaces (on) |
| `skyCubeSize`, `skyCubeFacesPerFrame` | the sky cube with the clouds that surfaces reflect: texels per side (64; 0: none) and faces drawn again per frame (6) |

`Renderer::effects().clouds` and `.skyCube` say what runs after the fallbacks; `RenderStats::clouds`, `cloudWidth`,
`cloudHeight`, `cloudCover` and `skyCubeFaces` what a frame drew.

**Fallbacks by `gpu::Caps` and formats**, never by platform: `Volumetric` needs sampled Rgba8Unorm 3D textures
(drawn as the layers of a 2D array and copied in: MoltenVK's portability subset has no 2D views of a 3D image),
renderable Rgba8Unorm 2D arrays and Rgba16Float targets, else `Layer` with a line in `Renderer::fallbacks()`; the
sky cube needs a renderable Rgba16Float cube, else surfaces reflect the sky without clouds. Every backend has them
today (SDL GPU's Metal and Vulkan through MoltenVK, the native Metal backend, the iOS Simulator).

**The passes**, every frame with a weather, before the HDR pass (timed ranges `weather`, `clouds`, `sky_cube`;
`cloud_noise` once):

1. **Noise** (drawn once, in the first frame with volumetric clouds): a 128³ Perlin-Worley shape
   texture (its red channel spread evenly over 0..1 by a logistic curve fitted to its distribution, so the share of
   air above `1 - coverage` is about `coverage`) with Worley octaves in the other channels, a 32³ Worley detail
   texture, and a 256² tiling 2D noise for the layer. Procedural, in the shaders (`cloudnoise.hlsl`).
2. **The weather map:** the field drawn by the GPU into a 512² Rgba8Unorm texture, 250 m texels over 128 km around
   the camera (on its texel grid, so it does not swim), with mips. **The cloud shadow map:** a 512² map over 24 km
   (47 m texels): the clouds' transmittance along the sun's direction above the plane y = 0 (16 steps through the
   shape), or the layer's. The two maps change slowly: each is drawn every other frame, in turns, and at once when
   it moved with the camera, after a cut or a change of mode.
3. **The march** (Volumetric) at `cloudResolution` into an Rgba16Float target (premultiplied radiance and
   transmittance) and its depth: the layer is a shell on a sphere of the planet's radius (6360 km), so the clouds
   meet the horizon; the march ends at the highest top on the map (a stratus deck is marched through its few hundred
   metres, a storm through 7.5 km), at most 60 km of path, in steps that grow with the distance (each a constant
   share of it) and stop when the ray is opaque, jittered per frame by the golden ratio. **The accumulation**
   (`cloudTemporal`): each frame's march reprojected by its depth into the previous result and blended at 0.1; a cut
   or a change of mode starts it again.
4. **The sky cube** (with `skyCubeSize`): the sky with its clouds (24 steps, no detail erosion) into an Rgba16Float
   cube, `skyCubeFacesPerFrame` faces per frame in turns (all six on the first frame, a cut or a change of mode),
   with mips for rough reflections.
5. **In the HDR pass** the sky with the clouds over it replaces the plain sky: the march's result upsampled (or the
   layer drawn per pixel), the cirrus above, haze toward the horizon.

**Shapes** (after Schneider's cloudscapes for Horizon Zero Dawn, "The real-time volumetric cloudscapes of Horizon
Zero Dawn", SIGGRAPH 2015, and Nubis, 2017): the weather map's coverage and type at a point; a height profile per
type (flat bases, round tops for cumulus, a tower for towering cumulus, an anvil for cumulonimbus, a slab for
stratus); the shape noise at 6 km per tile, sheared along the wind with height, above `1 - coverage`; eroded at the
edges by the detail noise at 800 m (faded out beyond 20 to 45 km, where it would alias). The noise moves through the
field's evolution time, so the clouds change shape instead of only scrolling.

**Light** (Hillaire, "Physically based sky, atmosphere and cloud rendering in Frostbite", 2016; Wrenninge, "Oz: the
great and volumetric", 2013): Beer-Lambert extinction (a mean free path of 25 m at density 1), the scattering
integrated per step; a light march of 4 to 6 steps toward the sun with a powder term (dark creases, bright edges
toward the sun); four octaves of multiple scattering; two Henyey-Greenstein lobes (the silver lining toward the
sun); ambient light from above and below by the height within the cloud, the tops seeing the sky and the bases the
lit ground, less where much cloud is above; raining bases darker. The sun's color and the sky's light come from the
scene's own sky (`FrameUniforms`), so sunsets tint the clouds (orange edges, dark bases). Distant clouds fade into
the sky's haze, under overcast toward the deck's grey. **The cirrus**: fibrous streaks of the layer noise along the
wind at `cirrusHeight`, lit by the sun's forward scattering. **The 2D layer**: the same coverage with a fraying
noise at the cloud base, lit by the sun and the sky; flat, but its clouds and their shadows are where the volumetric
ones would be.

## Light under the clouds

- **Cloud shadows:** surfaces read the cloud shadow map (surface sampler 14, `quench/Clouds.hlsli`:
  `quenchCloudShadow`) where the sun's ray through them meets the plane y = 0, and multiply the sun's light by it,
  with shadow maps and with ray-traced sun shadows alike. Beyond its 24 km the map's edge holds.
- **How overcast it is:** `Weather::cover` within 6 km of the camera (`RenderStats::cloudCover`). As the cover
  closes the sky's ambient light moves toward the radiance of a deck's base (a third of the light above it, grey
  with a little of the sun's hue), the ground's ambient light falls to 0.45 of it, and the fog turns grey; a closed
  deck shades the sun through the shadow map. The clouds' own ambient light follows it, so clouds under a deck are
  grey, not blue.
- **Reflections:** the sky cube (surface sampler 15, `quenchSkyReflection`) holds the sky with its clouds; surfaces
  reflect it in place of the sky's radiance (the lake in the demo, SSR's misses too), blurred by its mips with the
  roughness.
- **The traced effects** (ray-traced reflections' misses, the probe grid's sky rays, the path tracer) see the sky
  blended toward the deck's radiance by the cover (`quenchCloudySky`), not the clouds themselves, and their hits get
  the sun's light dimmed by the mean cover (`quenchOvercastSun`), not the shadow map. A simplification: the probes
  follow a change of the cover, not single clouds (the cube in their misses is open work).
- `FrameUniforms` has three members for it: `cloudShadow`, `skyCube` and `overcast` (all 0 without clouds); a game
  shader built on `quench/Surface.hlsli` gets all of this without a change.

## Phones and fallbacks

The demo's phone defaults (`App::mobile()`): volumetric at half size with the accumulation, 48 steps, one sky cube
face per frame, 60 frames per second. They come from the costs on the M4 Pro at a phone's size (below) and the
stage 11 ratio of about 3 between the iPhone 17 and the M4 Pro: about 4 ms on the iPhone 17, an estimate, over the
2 ms the roadmap set for the clouds; the 2D layer (about 1 ms by the same ratio) is the cheap setting. Not run on a
phone yet: the defaults wait for that run.

## Cost

GPU busy time per range on the M4 Pro, native Metal backend, offscreen (`scenarios/weather_cost.txt`: the demo's
fair weather and storm presets with the weather moving at 10 times its speed, 3 s per setting, the median of each
range; 2026-11-07). "Added" is the weather, clouds and sky cube ranges and the HDR pass's growth over clouds off
(the HDR pass alone 0.46 ms at 1920 x 1080 and 0.74 ms at 1206 x 2622):

| Setting | 1920 x 1080: weather, clouds, sky cube | added | 1206 x 2622: weather, clouds, sky cube | added |
|---|---|---|---|---|
| 2D layer | 0.22, -, 0.07 | 0.38 | 0.23, -, 0.07 | 0.38 |
| Volumetric, half size, 64 steps, 2 faces | 0.22, 1.47, 0.28 | 1.97 | 0.18, 1.57, 0.29 | 2.01 |
| Volumetric, full size, 64 steps, 2 faces | 0.19, 3.79, 0.31 | 4.29 | 0.23, 4.60, 0.32 | 5.14 |
| Storm, half size | 0.22, 1.07, 0.19 | 1.48 | 0.22, 1.05, 0.19 | 1.46 |
| Storm, full size | 0.22, 2.80, 0.21 | 3.23 | 0.22, 3.11, 0.21 | 3.55 |
| Phone defaults: half size, 48 steps, 1 face | 0.22, 1.11, 0.15 | 1.48 | 0.22, 1.07, 0.15 | 1.44 |

The march's cost follows its steps more than its pixels (half size has a quarter of the pixels and costs 0.4 of
full size); a storm costs less than fair weather because its rays turn opaque sooner. The noise is drawn once,
outside these numbers. Drawing the weather and shadow maps in turns took the weather range from
0.36 to 0.22 ms, and the sky cube without detail and with 24 instead of 32 steps from 0.41 to 0.28 ms (two faces per
frame). Measured offscreen only, never on the screen or on a phone.

## Demo and tests

The demo's "Weather" section (14; [Engine demo](demo.md)): a plain 20 km wide with hills, towers and a lake seen
from a few hundred metres up; presets (fair weather, sun shower, towering, storm, overcast, cirrus), coverage, type,
variation, precipitation, cirrus, a raining cumulonimbus as a cell, wind speed and direction, change of shape, the
weather's speed, the sun, Run day and Sunset, the quality (full size, half size, 2D layer, off), steps, cloud
shadows, the sky cube and its faces per frame, 60 Hz. Its batch part `weather` checks the cell's rain, the distance
to it and the cover, the march's size and the fallback to the layer, with three golden captures per backend
(`demo_weather.png`, `demo_weather_sunset.png`, `demo_weather_layer.png`); check mode item `weather.clouds` waits
for a person.

Tests: `weather.*` (12 cases: the coverage's mean by setting, determinism, drift, evolution, seeds, type and
precipitation, a cell in a clear sky and the distance to it, cells that drift and stay, wind and cloud tops, the
grid against single samples, queries on the job system's threads, a sample's cost), also under Node.js and with the
sanitizers; `gpu_device.texture_3d_from_layers` and `cube_faces` on every backend variant (a 3D texture filled from
the layers of a 2D array; a cube's faces drawn with `quench/Cube.hlsli`'s directions and sampled back, with its
mips); `render.renderer_clouds_shadow_the_ground` on every backend variant (off by default changes nothing; a raining
cell's shadow on the ground, 0.073 under it against 0.775 in the sun, in both modes; the sky cube's faces; no clouds
after `clearWeather`).

## Later parts

The design agreed for the rest of the stage (a plan: nothing below is built).

### Part 2: rain, wetness, puddles, ripples and their sounds

- **Rain from afar:** shafts under raining cloud bases (the weather map's precipitation, marched in the cloud pass
  below the base, lit by the sun so that rain against a dark cloud shines), and as an option a rainbow where the
  sun is behind the viewer.
- **Rain near the camera:** GPU particles in a volume around the camera (compute), drawn as streaks and splashes, as
  dense as `Weather::precipitation` at the camera; an indirect draw where `Caps::indirectDraw` allows it, else a
  fixed count with culled particles drawn degenerate (the iOS Simulator).
- **Rain occlusion:** a top-down depth map of the scene around the camera, so no rain, splashes or wetness under
  roofs; `Weather::precipitation` stays the open-sky value, and the renderer exposes the covered one.
- **Wetness:** a wetness map around the camera (and per point in `quench_weather`, headless, for games) that fills
  under rain and dries after it; wet materials darker and glossier, puddles in the low parts of a surface (a
  material's puddle mask or its height) that fill and dry; ripples by rain intensity in the puddles' normals, so they
  show in the sky cube's, SSR's and traced reflections.
- **Sounds:** below.

### Part 3: snow

Flakes with flutter and wind (the rain's particles with another motion and shape); accumulation on upward-facing
surfaces, none under cover (the same occlusion map), melting by the same clock as drying. Out of scope unless asked:
footprints.

### Part 4: wind

One wind field (direction, speed, gusts that travel with the air, local slowdowns behind terrain) in
`quench_weather`, extending `windAt`: the particles, the clouds' drift (already from the same wind), vegetation sway
(a surface shader's `QUENCH_SURFACE_VERTEX` reading the wind uniforms), forces on physics bodies (opt-in, through a
game's call per body), the wind sound and thunder.

### Weather sounds

A game-replaceable set of sound assets with engine defaults (procedural or CC0, cooked like any sound): rain layers
light and heavy, near and far, per surface (roof, leaves, puddles, ground), wind, thunder. An engine weather audio
object (in `quench_audio`, headless) mixes them from the weather data: rain at the listener and the distance to the
nearest rain (`distanceToPrecipitation`, a few times a second), the cover above the listener (part 2's occlusion
map, through the renderer, or a game's own), the wind speed. A game replaces a layer by naming its own asset for it,
or turns the engine's weather audio off and drives its own sounds from the same queries. They hot reload like any
sound asset.

### Phone budget for the whole stage

Targets for the iPhone 17 at 60 Hz (not measured): the clouds within about 2 ms, rain, splashes and ripples within
about 1 ms, the sky cube's turns within about 0.5 ms, the whole weather within about 4 ms; ten minutes at 60 Hz
without missed intervals with the thermal reaction on.

## Platforms

| Where | What ran |
|---|---|
| Mac, SDL GPU's Metal driver | every test, the demo's batch with its captures |
| Mac, native Metal backend | every test, the demo's batch (captures identical to SDL GPU's), the costs above |
| Mac, Vulkan through MoltenVK | every test with the Khronos validation layer, the demo's batch (captures within a mean of 0.001 of Metal's); needs the engine's SDL patch (`cmake/patches/SdlVulkan3d.cmake`: 3D textures 2D-array compatible only as color targets) |
| iOS Simulator | the test executables (`ios-sim-tests`) |
| WebAssembly under Node.js | `weather.*` (the headless module) |
| iPhone | not run |

## Limitations

- Nobody has looked at the clouds in motion, on the Mac's screen or on a phone (check mode item `weather.clouds`);
  the captures were looked at still.
- The traced effects see an overcast amount, not the clouds (above); the probe grid does not pick up single clouds.
- Cloud shadows are a 24 km map on the plane y = 0: a surface far above or below it reads the shadow of the column
  its sun ray crosses at y = 0, and beyond the map its edge holds.
- The storm's bases are somewhat smooth and sheet-like; the cumulonimbus has no rain shafts until part 2.
- Clouds beyond 64 km fade out (the weather map's reach); a camera above the cloud layer is not tuned (the march
  works, the look is not checked).
- Phone costs are estimates from the Mac; the phone defaults wait for a run on the iPhone 17.
- Precipitation is a number, not yet rain or snow on the screen (parts 2 and 3).
