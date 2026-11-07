# Weather

Stage 13 of the [Roadmap](roadmap.md) (requested by the user on 2026-11-07): rain, snow, sun and wind, clouds that
form, drift and change shape, light that follows them, rain under a single cloud in a blue sky, ripples in puddles
and weather sounds a game can replace. This page is the design of the whole stage and the description of what is
built. **Part 1, the weather map, the clouds and their light, is built** (2026-11-07): `quench_weather`, a headless
module, and the clouds in `quench_render`, with the demo's "Weather" section. **Part 2, rain, is built**
(2026-11-07): the wetness the weather remembers, the rain in `quench_render` (streaks, splashes, shafts, a rainbow,
the rain occlusion map, wet surfaces, puddles and ripples) and the rain's sound (`audio::WeatherAudio`). Parts 3 and
4 (snow, wind) are a plan ([Later parts](#later-parts)). Written for the engine, not moved from a game; neither game
uses it. It ran on the Mac (Metal, the native Metal backend and Vulkan through MoltenVK), in the iOS Simulator (the
test executables) and as WebAssembly under Node.js (the headless tests); not on an iPhone, and nobody has looked at
the clouds or the rain in motion or listened to the rain. All documentation: [index](README.md).

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

quench::render::RenderSettings rs;        // clouds and rain are off by default
rs.clouds = quench::render::Clouds::Volumetric;
rs.cloudResolution = 0.5f;                // half size, accumulated over frames (cloudTemporal)
rs.rain = quench::render::Rain::Medium;   // streaks, splashes, wet surfaces; shafts with the clouds
renderer.setSettings(rs);
// What gets wet: every material by default (wetness 1); puddles where a material asks for them
MaterialId paving = scene.addMaterial({.baseColor = {0.4f, 0.4f, 0.42f, 1}, .roughness = 0.7f, .puddles = 1});
MaterialId lake = scene.addMaterial({.baseColor = {0.02f, 0.05f, 0.06f, 1}, .roughness = 0.03f, .water = true});

quench::audio::WeatherAudio rainSound(app.audio()); // the engine's default rain sounds (quench_weather_sounds)

// Every frame
weather.update(dt);
weather.setWetnessFocus({camera.x, camera.z}); // where the remembered wetness follows the rain that fell
scene.setWeather(weather);                // a copy: set it again after each update
float rain = weather.precipitation(listenerPosition);           // 0..1 falling here
float nearest = weather.distanceToPrecipitation({x, z});        // m to the nearest rain
float overcast = weather.cover({x, z}, 6000.0f);                // mean coverage around a point
glm::vec3 wind = weather.windAt(position);                      // m/s
weather::Wetness wet = weather.wetness({x, z});                 // ground 0..1, standing water 0..1
float underRoof = renderer.stats().rainCover;                   // 0 open .. 1 covered (-1: not known yet)
rainSound.update({.precipitation = rain, .distanceToRain = nearest, .cover = std::max(underRoof, 0.0f)}, dt);
```

`Scene::clearWeather()` removes it (no clouds, no rain). A game links `quench_weather` for the queries alone
(headless, no GPU); `quench_render` links it publicly.

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

Queries: `sample(xz)`; `precipitation(point)` (the map's below the cloud base, 0 above; no roofs: the renderer's
rain occlusion map handles cover, [Rain](#rain)); `distanceToPrecipitation(xz, threshold, maxDistance)` (rings of 32 directions with a
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

## Rain

`RenderSettings::rain` (`Off` by default) draws the rain of the scene's weather where its precipitation falls, so a
single raining cloud rains on its own patch of ground and the sun shines beside it:

| Setting | |
|---|---|
| `rain` | `Off`, `Low`, `Medium`, `High` (below) |
| `rainShafts` | the shafts of rain under raining clouds seen from afar (on; they need clouds: they read the clouds' weather map) |
| `rainbow` | a rainbow in the shafts where the sun is behind the viewer (off) |

| Level | Streaks | Box around the camera | Splashes | Within | Shaft steps | Shafts marched at |
|---|---|---|---|---|---|---|
| `Low` | 8192 | 18 m | none | | 12 | a quarter of the render size per side |
| `Medium` | 24576 | 24 m | 1024 | 10 m | 20 | half |
| `High` | 49152 | 30 m | 3072 | 15 m | 32 | half |

`Renderer::effects().rain` and `.rainShafts` say what runs after the fallbacks; `RenderStats::rain`, `rainAtCamera`,
`rainStreaks`, `rainSplashes`, `rainOccluders`, `rainShafts`, `rainbow` and `rainCover` what a frame drew.
**Fallbacks by formats**, never by platform: the rain needs Rgba16Float targets it can sample and a depth format it
can sample (the shadow cascades'), else `Off` with a line in `Renderer::fallbacks()`; the shafts need clouds, else
off with a line; the path tracer's frame has no rain (a line). Every backend has the formats today.

**The passes** (timed ranges `rain_map`, `rain_shafts` and `rain`):

1. **The rain occlusion map**, before every pass that runs the surfaces: the shadow casters drawn into a 512² depth
   map over 128 m around the camera, an orthographic view along `Weather::rainVelocity` at the camera (the wind
   slants it), 200 m deep from 100 m above the camera, on its texel grid. A point is under cover where the highest
   surface along the rain is more than 0.4 m above it. **The cover over the camera** (`RenderStats::rainCover`,
   what the rain's sound takes): the texels within 0.75 m of the camera in last frame's map, copied in a command
   buffer of their own and read a few frames later without waiting; the share whose surface is more than 0.5 m
   above the camera.
2. **The weather maps** (surface sampler 14): one Rgba16Float texture of 512 x 1040 texels in three regions
   (`QUENCH_WEATHER_MAPS_*` in `quench/SceneUniforms.h`): the clouds' shadow (rows 0 to 511), the occlusion map (512
   to 1023) and the weather's wetness grid (the last rows' first 16 x 16 texels: ground wetness, standing water,
   precipitation), uploaded when `Weather::wetnessVersion()` changes.
3. **Wet surfaces** in the HDR pass and the G-buffer ([Wetness and puddles](#wetness-and-puddles)).
4. **The shafts** (with clouds), after the HDR pass and before TAA: marched at a quarter or half of the render size
   from 20 m to the scene, the cloud base or 30 km, in steps that grow with the square of their number. Each step
   reads the precipitation of the clouds' weather map at a mip as long as the step (a far step does not miss a
   shaft), denser in fall streaks (value noise at 220 and 70 m), thinning in the 150 m below the cloud base; an
   extinction of 0.0012 per metre at precipitation 1 (a visibility of about 2.5 km); single scattering of the sky's
   and the ground's light (dimmed where clouds are above the step) and of the sun's through the cloud shadow, with
   the rainbow (the primary bow 40.6 to 42.3 degrees from the antisolar point, the secondary 50.4 to 53.4 at a fifth
   of it, the whole at 0.06 of the sun's light). Jittered per frame (TAA blends the steps); blended into the HDR
   image by a depth-aware upsample (the four nearest texels' bilinear weights divided by how far their depth is from
   the pixel's), so a near edge does not take the veil of the distance behind it.
5. **The streaks and splashes**, after TAA and upscaling in the output-size image (TAA would smear them), tested
   against the scene's depth in the shader. Procedural: a fixed set of drops in a box around the camera, each falling
   with the rain's velocity from a place of its own and wrapped around the box, so nothing is simulated or stored; a
   drop is drawn where the precipitation there is above its own threshold and the occlusion map lets the rain reach
   it. A streak is the distance its drop falls in 0.04 s, 6 mm wide (at least a pixel; thinner ones fainter), faded
   toward the box's edge and within 0.3 m of the camera; lit by the sky's and the ground's light it refracts (as
   radiance) and the sun's it scatters forward (Henyey-Greenstein, g 0.7): rain against a dark wall and rain toward
   the sun shine. Splashes: rings of droplets for 0.25 s each at random places within the radius on the highest
   surface the occlusion map has. One instanced draw of six vertices per streak or splash (every instance runs;
   culled ones are degenerate).

Where the build differs from the agreed plan: the drops are procedural instances instead of compute particles with
an indirect draw (the same look without state, on every backend and in the iOS Simulator); the shafts are a pass of
their own after the HDR pass instead of a part of the cloud march (they veil the scene in front of geometry, which
the clouds, drawn behind it, do not).

## Wetness and puddles

- **The weather remembers how wet the ground is** (`quench_weather`, headless): `Weather::wetness(xz)` gives
  `Wetness{ground, water}`, each 0..1, for open ground. A grid of 16 x 16 cells of 64 m around a focus
  (`setWetnessFocus`: a game's camera or player; the grid moves in whole cells) wets and dries by the precipitation in
  each cell in steps of 0.25 s of weather time: the ground soaks in `Settings::wettingTime` (120 s of precipitation
  1, twice as long at 0.5) and dries in `dryingTime` (900 s); standing water fills in `puddleFillTime` (900 s) and
  drains in `puddleDryTime` (2400 s), so puddles outlast the wet ground. Precipitation below `kWettingThreshold`
  (0.02) neither wets nor keeps the ground from drying. Outside the grid, for the cells a move brings in and after
  `setTime`, an estimate from the last hours of the weather (48 samples, about 0.01 ms). Deterministic: the same
  calls in the same order give the same grid; `wetnessGrid()`, `wetnessOrigin()` and `wetnessVersion()` are what
  the renderer uploads.
- **On surfaces** (`quenchApplyRain` in `quench/Surface.hlsli`; the surface entries call it after the surface
  function, also for the G-buffer, so traced and screen-space reflections see the wet surface; not in ray hits):
  where the rain reaches (the occlusion map, read 0.3 m off the surface along its normal: walls see the open ground
  beside them, floors their roof), the ground's wetness times `MaterialDesc::wetness` (1 by default, 0: never wet),
  less on steep faces, darkens the albedo (to 0.55 when soaked, less where metallic) and lowers the roughness toward
  `max(0.12, 0.4 r)`. Up-facing parts of a material with `puddles` collect standing water in the hollows of a relief
  (`quenchPuddleHeight`: value noise at 6 and 1.5 m) up to 0.42 of it at full water, about a third of a flat surface:
  a dark mirror there (roughness 0.02, not metallic, the geometric normal). On standing water and on materials with
  `water` (a lake) the rings of the drops falling now (`quenchRipples`: two offset grids of 0.4 m cells, a drop every
  0.8 s in a share of the cells that grows with the rain, its ring spreading and fading; within 60 m of the camera)
  bend the normal, so they show in the sky cube's, SSR's and traced reflections.
- **Opt-out:** a surface file that defines `QUENCH_SURFACE_NO_RAIN` before including `quench/SurfaceEntries.hlsli`
  keeps its surface as its function makes it, or calls `quenchApplyRain` itself and changes what it gets; a material
  with wetness 0, no puddles and not water costs a branch.
- `FrameUniforms` has three members for it (`rainViewProj`; `rain`: the maps on, the occlusion map's depth range, the
  precipitation at the camera, the ripples; `rainGrid`: the wetness grid's place) and `MaterialUniforms` one
  (`weather`).

## The rain's sound

`audio::WeatherAudio` (in `quench_audio`, headless; [Audio](audio.md#weather-sounds)) mixes looping layers of rain
from what the weather does at the listener: light and heavy rain crossfaded at equal power by the precipitation,
distant rain as the nearest rain comes within 4 km (`distanceToPrecipitation`, a few times a second), and rain
drumming on a roof by the cover over the listener (`RenderStats::rainCover`, or a game's own test along
`rainVelocity`). The engine's defaults are procedural (`tools/assets/make_weather_sounds.py`, cooked by the CMake
function `quench_weather_sounds()`); a game names its own sound for any layer, or turns the object off and plays its
own sounds from the same numbers (`WeatherAudio::targetGains` gives the engine's mix rules). Wind and thunder come
with part 4.

## Phones and fallbacks

The demo's phone defaults (`App::mobile()`): volumetric at half size with the accumulation, 48 steps, one sky cube
face per frame, 60 frames per second. They come from the costs on the M4 Pro at a phone's size (below) and the
stage 11 ratio of about 3 between the iPhone 17 and the M4 Pro: about 4 ms on the iPhone 17, an estimate, over the
2 ms the roadmap set for the clouds; the 2D layer (about 1 ms by the same ratio) is the cheap setting. The rain on a
phone: `Low` without the shafts (so without the rainbow), 0.25 ms on the M4 Pro at the iPhone 17's size, about
0.7 ms on the phone by the same ratio, within the 1 ms set for the rain; `Low` with the shafts would be about 1.5 ms.
Not run on a phone yet: the defaults wait for that run.

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

**The rain** (`scenarios/rain_cost.txt`, 2026-11-07 23:10, the same way: the Rain here preset from the ground and
the storm preset from the air, half-size clouds, 3 s per setting): `rain_map`, `rain_shafts` and `rain`, and the HDR
pass's growth over rain off (the wet surfaces); "added" is their sum, ms.

| Setting | 1920 x 1080: map, shafts, drops, HDR | added | 1206 x 2622: map, shafts, drops, HDR | added |
|---|---|---|---|---|
| Ground, Low | 0.02, 0.18, 0.08, 0.07 | 0.35 | 0.02, 0.26, 0.12, 0.11 | 0.50 |
| Ground, Medium | 0.02, 0.44, 0.14, 0.08 | 0.68 | 0.02, 0.59, 0.17, 0.11 | 0.90 |
| Ground, High | 0.02, 0.60, 0.46, 0.09 | 1.17 | 0.02, 0.81, 0.49, 0.13 | 1.46 |
| Ground, Low without shafts (phone defaults) | 0.02, -, 0.09, 0.07 | 0.17 | 0.02, -, 0.12, 0.11 | 0.25 |
| Storm from the air, Low | 0.02, 0.23, 0.09, 0.04 | 0.37 | 0.02, 0.34, 0.13, 0.05 | 0.54 |
| Storm from the air, Medium | 0.02, 0.83, 0.14, 0.05 | 1.04 | 0.02, 1.23, 0.19, 0.06 | 1.50 |
| Storm from the air, High | 0.02, 1.20, 0.45, 0.07 | 1.73 | 0.02, 1.84, 0.48, 0.08 | 2.42 |

The shafts cost the most where rain fills the view (a storm seen from the air); at full size they took 1.6, 2.7 and
4.4 ms there at 1080p (the first runs, the same session), hence the reduced size. Their upsample is most of `Low`'s
shaft time. The drops cost by their count, not by the rain: every instance runs.

## Demo and tests

The demo's "Weather" section (14; [Engine demo](demo.md)): a plain 20 km wide with hills, towers and a lake seen
from a few hundred metres up; presets (fair weather, sun shower, towering, storm, overcast, cirrus, rain here,
downpour, passing shower), coverage, type, variation, precipitation, cirrus, a raining cumulonimbus as a cell, wind
speed and direction, change of shape, the weather's speed, the sun, Run day and Sunset, the quality (full size, half
size, 2D layer, off), steps, cloud shadows, the sky cube and its faces per frame, 60 Hz; the rain's level, shafts,
rainbow, sound and its volume; views from the air, from the ground, toward the cloud and from the shelter. On the
ground: wet paving with puddles, a pond, a shelter with a crate under it and one in the open, a dark block that the
rain shows against. Its batch parts: `weather` checks the cell's rain, the distance to it and the cover, the march's
size and the fallback to the layer, with three golden captures per backend (`demo_weather.png`,
`demo_weather_sunset.png`, `demo_weather_layer.png`, which show the shafts since part 2); `rain` checks the rain at
the camera, the streaks and splashes, the cover under the shelter, the wetness, the sound's layers and level, a
passing shower drying and its distant rain, and the rain off, with three captures (`demo_rain.png`: the streaks,
puddles and rings, the dry floor under the shelter; `demo_rain_rainbow.png`: toward the cloud with the rainbow;
`demo_rain_shelter.png`: from under the roof). Check mode items `weather.clouds`, `weather.rain` (in motion) and
`weather.rain_sound` (by ear) wait for a person.

Tests: `weather.*` (15 cases: the coverage's mean by setting, determinism, drift, evolution, seeds, type and
precipitation, a cell in a clear sky and the distance to it, cells that drift and stay, wind and cloud tops, the
grid against single samples, queries on the job system's threads, a sample's cost; the wetness building and drying,
under a passing shower, and the grid following its focus), also under Node.js and with the sanitizers;
`audio.weather_audio_gains`, `weather_audio_mix` (the layers by the strongest frequency of an offline mix, a game's
sound, a hot reload, the bus, silence and off) and `weather_default_sounds` (the default loops: levels, seams, where
their energy is); `gpu_device.texture_3d_from_layers` and `cube_faces` on every backend variant (a 3D texture
filled from the layers of a 2D array; a cube's faces drawn with `quench/Cube.hlsli`'s directions and sampled back,
with its mips); on every backend variant `render.renderer_clouds_shadow_the_ground` (off by default changes
nothing; a raining cell's shadow on the ground, 0.073 under it against 0.775 in the sun, in both modes; the sky
cube's faces; no clouds after `clearWeather`), `renderer_rain_wets_the_open_ground` (open ground 0.521 dry, 0.288
wet; under a roof 0.521 both; the cover 0 in the open and above 0.9 under the roof; the streaks; the puddles' rings
moving; no rain with the path tracer or without a weather) and `renderer_rain_shafts_from_afar` (none without
clouds; with them a shaft over 733 of 4096 pixels).

## Later parts

The design agreed for the rest of the stage (a plan: nothing below is built). Part 2 is built (above); where it
differs from its plan is said in [Rain](#rain).

### Part 3: snow

Flakes with flutter and wind (the rain's streaks with another motion and shape); accumulation on upward-facing
surfaces, none under cover (the same occlusion map), melting by the same clock as drying. Out of scope unless asked:
footprints.

### Part 4: wind

One wind field (direction, speed, gusts that travel with the air, local slowdowns behind terrain) in
`quench_weather`, extending `windAt`: the particles, the clouds' drift (already from the same wind), vegetation sway
(a surface shader's `QUENCH_SURFACE_VERTEX` reading the wind uniforms), forces on physics bodies (opt-in, through a
game's call per body), the wind sound and thunder (new layers of `WeatherAudio`).

### Phone budget for the whole stage

Targets for the iPhone 17 at 60 Hz (not measured): the clouds within about 2 ms, rain, splashes and ripples within
about 1 ms, the sky cube's turns within about 0.5 ms, the whole weather within about 4 ms; ten minutes at 60 Hz
without missed intervals with the thermal reaction on.

## Platforms

| Where | What ran |
|---|---|
| Mac, SDL GPU's Metal driver | every test, the demo's batch with its captures |
| Mac, native Metal backend | every test, the demo's batch (captures identical to SDL GPU's), the costs above |
| Mac, Vulkan through MoltenVK | every test with the Khronos validation layer, the demo's batch (captures within a mean of 0.04 of Metal's); needs the engine's SDL patch (`cmake/patches/SdlVulkan3d.cmake`: 3D textures 2D-array compatible only as color targets) |
| iOS Simulator | the test executables (`ios-sim-tests`) |
| WebAssembly under Node.js | `weather.*` (the headless module) and `audio.weather_audio_*` |
| iPhone | not run |

## Limitations

- Nobody has looked at the clouds or the rain in motion, on the Mac's screen or on a phone, or listened to the rain
  (check mode items `weather.clouds`, `weather.rain`, `weather.rain_sound`); the captures were looked at still, the
  sound measured.
- The traced effects see an overcast amount, not the clouds (above); the probe grid does not pick up single clouds.
- Cloud shadows are a 24 km map on the plane y = 0: a surface far above or below it reads the shadow of the column
  its sun ray crosses at y = 0, and beyond the map its edge holds.
- The storm's bases are somewhat smooth and sheet-like. Seen from the air a storm is a grey veil over the plain; the
  shafts of the 2D layer's clouds look like smoky blobs more than rain.
- Clouds beyond 64 km fade out (the weather map's reach); a camera above the cloud layer is not tuned (the march
  works, the look is not checked).
- Phone costs are estimates from the Mac; the phone defaults wait for a run on the iPhone 17.
- The rain: no roofs beyond 64 m of the camera (the occlusion map's reach: farther surfaces count as open) and
  nothing above 100 m over the camera counts as a roof; the wetness grid covers 1 km around its focus (beyond it an
  estimate without the rain's history); wetness knows no roofs (`Weather::wetness` is open ground; the renderer
  applies the occlusion map, a game asks its own physics); the cover is the camera's only (`RenderStats::rainCover`),
  a few frames late. Splashes are faint on bright reflections. The streaks do not refract the scene behind them.
  The ripples are the same rings on every puddle, not a simulation; no drips from roofs, no runoff, no wet
  footprints. Ray hits do not apply the rain (traced reflections and the probe grid see dry surfaces), the streaks
  are not in any reflection, and the path tracer's frame has no rain.
- The sound: one set of layers for the listener, not per surface (leaves, puddles, metal); the defaults are
  procedural noise, measured but not listened to.
