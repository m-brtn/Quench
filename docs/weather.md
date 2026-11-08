# Weather

Stage 13 of the [Roadmap](roadmap.md) (requested by the user on 2026-11-07): rain, snow, sun and wind, clouds that
form, drift and change shape, light that follows them, rain under a single cloud in a blue sky, ripples in puddles
and weather sounds a game can replace. This page is the design of the whole stage and the description of what is
built. **Part 1, the weather map, the clouds and their light, is built** (2026-11-07): `quench_weather`, a headless
module, and the clouds in `quench_render`, with the demo's "Weather" section. **Part 2, rain, is built**
(2026-11-07): the wetness the weather remembers, the rain in `quench_render` (streaks, splashes, shafts, a rainbow,
the rain occlusion map, wet surfaces, puddles and ripples) and the rain's sound (`audio::WeatherAudio`). **Part 3,
snow, is built** (2026-11-08): the weather's temperature and snow share, the snow cover it remembers beside the
wetness, and in `quench_render` the flakes, the cover on surfaces and the snow's shafts; the rain's sound keeps quiet
where it snows. **Part 4, wind, is built** (2026-11-08): gusts and eddies in the weather's wind field that the rain,
the snow and a game's queries share, plants that sway with it (`quench/Wind.hlsli`), the wind's drag on chosen
physics bodies (`WindDrag`), lightning in storms that lights the clouds and the scene, and the wind's and the
thunder's sounds ([Wind](#wind)). Written for the engine, not moved from a game; neither game uses it. It ran on the
Mac (Metal, the native Metal backend and Vulkan through MoltenVK), in the iOS Simulator (the test executables) and
as WebAssembly under Node.js (the headless tests). The user looked at the clouds, the rain, the snow and the wind in
motion and listened to the rain, the wind and the thunder in the demo on the Mac and on the iPhone 17 on 2026-11-08
and reported them fine; the tests and the costs have not run on an iPhone, and the iPhone 18 Pro Max has run
nothing of it. All documentation: [index](README.md).

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
s.wind = {.direction = {0, 1}, .speed = 12, .gusts = 0.5f, .turbulence = 0.3f};
s.temperature = 15.0f;                    // degrees C at the ground: rain (below about 1 degree, snow)
wx::Weather weather(s);
weather.setTime(1200.0);                  // where the weather's clock starts (s)
// One raining cumulonimbus in a blue sky, drifting with the wind
wx::CellId shower = weather.addCell({.position = {2600, -10500}, .radius = 3200,
                                     .type = wx::CloudType::Cumulonimbus, .precipitation = 1});

quench::render::RenderSettings rs;        // clouds and rain are off by default
rs.clouds = quench::render::Clouds::Volumetric;
rs.cloudResolution = 0.5f;                // half size, accumulated over frames (cloudTemporal)
rs.rain = quench::render::Rain::Medium;   // streaks, splashes, wet surfaces; shafts with the clouds
rs.snow = quench::render::Snow::Medium;   // flakes, the snow cover on surfaces; where the weather is cold enough
rs.lightning = true;                      // storms' flashes light the clouds and the scene
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
float overhead = weather.coverAbove(camera, 6000.0f);           // the same, only the clouds above the point
glm::vec3 wind = weather.windAt(position);                      // m/s, with its gusts and eddies
weather::Wetness wet = weather.wetness({x, z});                 // ground, standing water, snow 0..1
float snowShare = weather.snowShare(listenerPosition);          // 0 rain .. 1 snow (by the temperature there)
float rainAway = weather.distanceToPrecipitation({x, z}, 0.1f, 8000.0f, weather::Precipitation::Rain);
float underRoof = renderer.stats().rainCover;                   // 0 open .. 1 covered (-1: not known yet)
rainSound.update({.precipitation = rain, .distanceToRain = rainAway, .cover = std::max(underRoof, 0.0f),
                  .snow = snowShare, .wind = glm::length(weather.windAt(listenerPosition))}, dt);
// Thunder: each flash once, when it appears; heard later by its distance
weather.strikes({x, z}, 25000.0f, lastTime, weather.time(), flashes);
for (const weather::Strike& f : flashes) rainSound.strike(glm::distance(f.position, {x, z}), f.brightness);
// The wind on chosen bodies (quench_physics), before each step
drag.apply(physics, [&](glm::vec3 p) { return weather.windAt(p); });
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
  `Settings::precipitation`. Whether it falls as rain or snow is the temperature's ([Snow](#snow)).
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
rain occlusion map handles cover, [Rain](#rain)); `snowShare(point)` and `temperature(point)` ([Snow](#snow));
`distanceToPrecipitation(xz, threshold, maxDistance, kind)` (`Precipitation::Any`, `Rain` or `Snow`: the
precipitation times the share of that kind; rings of 32 directions with a
bisection, at most 48 rings: 0.03 ms on the M4 Pro with rain 3.7 km away, about 0.3 ms when there is none; a few
times a second, not per object per frame); `cover(xz, radius)` (19 samples); `windAt(point)` (the setting's wind
at the cloud base, a logarithmic profile below it: about 0.3 of it at 2 m; with its gusts and eddies, [Wind](#wind));
`strikes(...)` (lightning, [Wind](#wind)); `cloudTop(type)`; `highestType()` (an
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
| `cloudShadowHeight` | the height of the plane the cloud shadow map lies on (0 m): only the clouds above it shade, so a game over a sea of clouds sets it to its ground's height |
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
   (47 m texels): the clouds' transmittance along the sun's direction above the plane y = `cloudShadowHeight` (16
   steps through the shape above it), or the layer's where the layer is above it. The two maps change slowly: each is drawn every other frame, in turns, and at once when
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
  `quenchCloudShadow`) where the sun's ray through them meets the plane y = `cloudShadowHeight` (0 by default; the
  clouds below the plane cast no shadow, since 2026-11-08), and multiply the sun's light by it,
  with shadow maps and with ray-traced sun shadows alike. Beyond its 24 km the map's edge holds.
- **How overcast it is:** `Weather::coverAbove` within 6 km of the camera (`RenderStats::cloudCover`): the cover of
  the clouds above it, all of it under the cloud base and none over the tops, so a camera flying over a sea of
  clouds sees the sun and the clear sky (since 2026-11-08; before, the clouds below dimmed them too). As the cover
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
| `Medium` | 24576 | 24 m | 1536 | 10 m | 20 | half |
| `High` | 49152 | 30 m | 4096 | 15 m | 32 | half |

`Renderer::effects().rain` and `.rainShafts` say what runs after the fallbacks; `RenderStats::rain`, `rainAtCamera`,
`rainStreaks`, `rainSplashes`, `rainOccluders`, `rainShafts`, `rainbow` and `rainCover` what a frame drew.
**Fallbacks by formats**, never by platform: the rain needs Rgba16Float targets it can sample and a depth format it
can sample (the shadow cascades'), else `Off` with a line in `Renderer::fallbacks()`; the shafts need clouds, else
off with a line; the path tracer's frame has no rain (a line). Every backend has the formats today.

**The passes** (timed ranges `rain_map`, `rain_shafts` and `rain`):

1. **The rain occlusion map**, before every pass that runs the surfaces: the shadow casters drawn into a 512² depth
   map over 128 m around the camera, an orthographic view along `Weather::rainVelocity` at the camera (the wind
   slants it), 200 m deep from 100 m above the camera, on its texel grid, without the cascades' slope bias (which
   sank its depth by decimetres under slanted rain and hid the splashes under the ground). A point is under cover
   where the highest surface along the rain is more than 0.4 m above it. **The cover over the camera** (`RenderStats::rainCover`,
   what the rain's sound takes): the texels within 0.75 m of the camera in last frame's map, copied in a command
   buffer of their own and read a few frames later without waiting; the share whose surface is more than 0.5 m
   above the camera.
2. **The weather maps** (surface sampler 14): one Rgba16Float texture of 512 x 1040 texels in three regions
   (`QUENCH_WEATHER_MAPS_*` in `quench/SceneUniforms.h`): the clouds' shadow (rows 0 to 511), the occlusion map (512
   to 1023) and the weather's wetness grid (the last rows' first 16 x 16 texels: ground wetness, standing water,
   precipitation, its rain share; the next 16 x 16 the snow cover), uploaded when `Weather::wetnessVersion()`
   changes.
3. **Wet surfaces** in the HDR pass and the G-buffer ([Wetness and puddles](#wetness-and-puddles)).
4. **The shafts** (with clouds), after the HDR pass and before TAA: marched at a quarter or half of the render size
   from 20 m to the scene, the cloud base or 30 km, in steps that grow with the square of their number. Each step
   reads the precipitation of the clouds' weather map at a mip as long as the step (a far step does not miss a
   shaft), denser in fall streaks (value noise at 220 and 70 m), thinning in the 150 m below the cloud base; an
   extinction of 0.0012 per metre at precipitation 1 (a visibility of about 2.5 km); single scattering of the sky's
   and the ground's light (from the share of the sky the clouds close at the camera, the radiance of their bases,
   `FrameUniforms::overcast`; dimmed where more cloud is above the step than above the camera) and of the sun's
   through the cloud shadow, with
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
   the sun shine. Splashes: crowns 15 cm high (give or take 30 %) for 0.25 s each at random places within the
   radius on the highest surface the occlusion map has, a rim of droplets rising and spreading and four droplets
   thrown up and out of it; twice the light the drops refract and 0.1 of the sun's irradiance (they glint where the
   puddles' mirror of the sky around them does not). One instanced draw of six vertices per streak or splash (every
   instance runs; culled ones are degenerate).

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
  bend the normal, so they show in the sky cube's, SSR's and traced reflections. Each drop has a size of its own
  (its ring's width, wavelength and reach, 0.6 to 1.4), and drops fall more densely and strike harder in some
  puddles than in others (value noise at 3 m: 0.55 to 1.45 times the density, 0.7 to 1.3 times the strength).
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
own sounds from the same numbers (`WeatherAudio::targetGains` gives the engine's mix rules). Snow is quiet
([Snow](#snow)); the wind and the thunder are layers of the same object ([Wind](#wind)).

## Snow

- **Rain or snow** (`quench_weather`): `Settings::temperature` (15 degrees C at the ground by default, the standard
  atmosphere's) falls by `lapseRate` (0.0065 per metre) with height; precipitation falls as snow at or below 0
  degrees (`QUENCH_SNOW_BELOW`), as rain from 2.5 (`QUENCH_RAIN_ABOVE`), as sleet, a mix, between. A cell's own
  `Cell::snow` (0..1; -1, the default: by the temperature) holds within about a third of its radius and blends out
  to its edge: a snow shower on a mild day. `snowShare(point)` is the share at a point (the shaders'
  `quenchWeatherSnow`, the same function in `WeatherField.h`), `temperature(point)` the temperature,
  `snowVelocity(point)` the flakes' velocity (the wind's, falling at `kSnowFallSpeed`, 1 m/s). With the default
  settings nothing below the default cloud base (1.5 km, 5.25 degrees there) is snow, so a scene that sets no
  temperature keeps part 2's rain (the snow's terms in the shaders reduce to it exactly).
- **The snow cover** (`Wetness::snow`, 0..1, in the wetness grid with its focus, estimate and determinism): it builds
  by the snowfall (the precipitation times its snow share) in `Settings::snowTime` (600 s of snowfall 1 to a full
  cover) and melts above 0 degrees in `meltTime` x 5 / (degrees above 0) (900 s at 5 degrees), its water wetting
  the ground (twice the snow melted) and filling the puddles (half of it); the ground does not dry while snow melts
  on it, and only rain wets it. Measured (`weather_snow_cover_builds_and_melts`): 300 s of snowfall 1 at -3
  degrees, cover 0.500; then 225 s at 5 degrees, cover 0.250, ground 0.500, standing water 0.125.
- **On surfaces** (`quenchApplySnow` in `quench/Surface.hlsli`, after `quenchApplyRain`, also for the G-buffer; not
  in ray hits): the cover (the grid's, sampled as the wetness is) times `MaterialDesc::snow` (1 by default; 0: never
  covered; water takes none), only where the snow reaches (the occlusion map, read 0.3 m off the surface: none under
  a roof) and on faces looking up (none on walls, less on slopes: `smoothstep(0.3, 0.8, N.y)`). A thin cover lies
  as a dusting with patches (value noise at 0.8 and 0.2 m) that close as it deepens; from 20 to 100 m away the
  patches give way to the share of the ground they cover. Where it lies the albedo turns to snow's (0.82, 0.85,
  0.9), the roughness to 0.7, the metal and the normal map go, emission and anisotropy fade. **Opt-out:**
  `QUENCH_SURFACE_NO_SNOW` before including `quench/SurfaceEntries.hlsli` (as `QUENCH_SURFACE_NO_RAIN`), or a
  material's `snow = 0`.
- **The flakes** (`RenderSettings::snow`: `Off` by default, `Low`, `Medium`, `High`), drawn after the streaks in the
  output-size image, are the rain's procedural instances with another motion and shape: each falls at the snow's
  velocity at the camera (its fall give or take 30 %), flutters on a loop of its own (8 to 30 cm, 1.2 to 2.6
  radians per second) and is a soft disc 2 cm across (give or take 40 %; at least 1.5 pixels, fainter below that)
  drawn out along its motion over 12 ms; lit as a white diffuse scatterer (0.85 of the sky's and the ground's
  light) and by 0.15 of the sun's irradiance through the cloud shadow; drawn where the snowfall there is above its
  own threshold and the occlusion map lets the snow reach it. `Low` 6144 flakes in a box 9 m across, `Medium`
  16384 in 12 m, `High` 32768 in 14 m. `RenderStats::snow`, `snowAtCamera`, `snowShareAtCamera` (both 0 while the
  snow is not drawn) and `snowFlakes`; the timed range `snow`.
- **The occlusion map under snow** looks along the snow's direction (mixed with the rain's by the share at the
  camera), at most 0.3 (horizontal over vertical, about 17 degrees) from the vertical: flakes drift almost sideways
  in a wind, which would leave a bare strip of metres behind every wall.
- **The snow's shafts** (`rainShafts`, the same pass): where the snow share at a step (by the temperature at its
  height) is above 0, its precipitation hides 3 to 5 times as much as rain's (in wide, smooth curtains, value noise
  at 220 m: a heavy snowfall's visibility is about 800 m) and sends 1.5 times as much of the sun's light toward the
  camera; under a snowfall they begin where the flakes end. With rain off and snow on (or the reverse) the other
  kind's share is left out.
- **The sound:** snow is quiet. `WeatherSoundState::snow` (the share at the listener) takes the snowfall out of the
  light and heavy rain and the roof (sleet at share 0.5 and precipitation 0.6 sounds as rain at 0.3), and a
  snowfall at the listener muffles the distant rain (down to 0.4 of it); `distanceToRain` should be the distance
  to rain only (`Precipitation::Rain`). No sound of its own: falling snow is close to silence (the wind's layers
  play in a snowfall as anywhere, [Wind](#wind)).
- **The grid follows the settings:** the precipitation and snow share the grid holds per cell (what the drops and
  flakes read) are sampled again at the next `update()`, also one of 0 s while the clock is stopped, after
  `setSettings` or a change of the cells; before 2026-11-08 they waited for the grid's next quarter second of weather
  time, so a scene that set a cold temperature with the clock stopped drew rain where the weather said snow.

Where the build differs from the agreed plan: the occlusion direction is clamped (above); the cover melts by the
temperature at y = 0 (the grid knows no terrain heights); the estimate after `setTime` assumes the present
temperature held in the past.

## Wind

**The field** (`quench_weather`, `shaders/quench/WeatherField.h`: `quenchWeatherWind`, the same function in C++ and
HLSL). `Settings::wind` holds the direction and the speed at the cloud base, with a logarithmic profile below it
(a roughness length of 0.1 m: open ground, grass; about 0.3 of it at 2 m, 0.48 at 10 m under a base at 1.5 km), and
two shares, 0 by default (a steady wind, as in parts 1 to 3):

- **Gusts** (`Wind::gusts`, 0..1): bands of faster and slower air across the wind, about 40 m along it and 120 m
  across, that change over about 8 s. At 1 the speed along the wind deviates by about 0.45 of the mean (measured
  over time at a point 0.459, over space at a moment 0.443).
- **Turbulence** (`Wind::turbulence`, 0..1): eddies about 10 m across that change over about 2 s and turn the wind
  every way. At 1 the speed along and across deviates by about 0.3 of the mean, upward by about 0.12 (measured
  0.287, 0.292 and 0.120).

Both travel with the air at 0.6 of the cloud base's wind (gusts of tens of metres near the ground move about that
fast) and scale with the mean wind at the point, so a calm stays calm at every height. The field gives three parts:
the mean wind, what the gusts and eddies add to it now (`windAt` is the sum) and how far they have moved the air
from where the mean wind alone would have taken it (`windOffset`, m; the rate of change of the offset is the gust,
within 0.0012 m/s of the numerical derivative where gusts reach 4.1 m/s). The offset is what a particle carried by
the wind adds to its mean path, so the renderer's drops and flakes need no state. The noise is value noise on
integer hashes (the eddies' three axes from one hash per lattice corner, split into three 10-bit values: a third of
the hashing); its time runs modulo 4096 s, over which it repeats, so that 32-bit floats keep it as exact after
hours as at the start (over 0.01 s the wind at 5 m changes by 0.027 m/s across the seam, by 0.037 elsewhere). The
field's place is the point less the distance the air has travelled, also a 32-bit float that grows with the wind
(the clouds' drift since part 1): at 3 km it rounds to about 0.1 mm, after an hour at 20 m/s (72 km) to about
4 mm, nothing to see, but a rate taken over one frame from two such evaluations is off by a few hundredths of a m/s.
`windAt` costs about 40 ns on the M4 Pro. `meanWindAt` is the profile alone; `rainVelocity` and `snowVelocity` (the
occlusion map's direction) carry the mean wind, not the gusts.

**Where it acts:**

- **The rain and the snow** (`quench_render`): each drop and flake is carried off its mean path by the offset at
  its place and its streak or blur turned by the gust there, so gusts sweep through the rain and eddies swirl the
  snow.
- **The clouds** drift with the mean wind at the cloud base, as before; gusts of tens of metres do not move them.
- **Plants** (`shaders/quench/Wind.hlsli`): `quenchWindAt(p)` is the field in a shader, from the frame uniforms
  (`FrameUniforms::windAir`, `windSpeed`, `windTime`; 0 without a weather). `quenchWindSway(p, n, instance, height,
  stiffness, flutter)` bends a vertex (object space) and its normal for a surface shader's `QUENCH_SURFACE_VERTEX`
  function: the plant's root is its object's origin, its stem the world's up; a vertex turns about the root by an
  angle that grows with its height above it, downwind as far as the wind at the plant's top pushes it (about
  0.08 radians per m/s at stiffness 1, grass; divided by the stiffness, about 2 for a bush and 4 or more for a
  tree's crown; at most 1.3 radians); gusts bend it further and cross a field as waves, eddies shake it, and its
  leaves shiver by `flutter` metres in a 10 m/s wind. `quenchWindSwayAbout` takes the root per vertex instead:
  a patch of grass in one instance, each blade turning about the ground below its vertex (`float3(p.x, 0, p.z)`).
  Built with `surface_shadow_vs` too, the shadows sway. **The motion vectors, the traced effects' acceleration
  structures and the rays see the plants unbent** (the sway is a vertex function of the surface entries, not a
  deformation of the mesh): TAA reprojects a swaying leaf as if it stood still (in the demo's gale it looks
  steady; a fast flutter could smear), and ray-traced shadows and reflections show the plant upright. Without a
  weather, in a calm or with no wind uniforms the vertex is returned as it is.
- **Physics** (`quench_physics`, `include/quench/physics/WindDrag.h`): `WindDrag` pushes the bodies a game adds to
  it, each with its area facing the wind (m²) and its drag coefficient: F = 1/2 ρ C_d A |w - v| (w - v), with w
  the wind at the body's centre of mass (a function the game passes: `Weather::windAt` for the weather's, gusts
  included) and v the body's velocity, as a force before each step. No torque, no lift, no shelter behind other
  bodies; off unless used (a body not added feels nothing). A sleeping body wakes when its drag is above 1 % of its
  weight. Measured (`physics.physics_wind_drag`): a 1 kg ball (0.1 m², C_d 1) at rest in a steady 10 m/s wind
  without gravity reaches 5.514 m/s after 2 s against the analytic 5.506; a ball not added stays; a body that left
  the world is skipped.
- **Lightning** (`Settings::lightning`, 1 by default; 0 none): `Weather::strikes(xz, radius, from, to, out)` lists
  the flashes within a radius over a span of weather time (at most a minute back). Only storms flash: a
  cumulonimbus raining heavily, in the field or as a cell. Candidates on a lattice of 2 km squares in the air's
  coordinates (the flashes drift with their storms) and whole seconds, each with a chance of 0.04 times the rate,
  kept by how stormy the weather is there and then; so a flash is a pure function of the weather and its time, and
  every query of a span gives the same flashes, however the span is cut. Measured: 2.3 flashes a minute from a
  raining cumulonimbus cell of 3.2 km radius at rate 1; a frame's query 0.001 to 0.002 ms. `Weather::flash(strike,
  time)` is its light: two to four return strokes 40 to 120 ms apart, each fading in 30 ms, and the channel's glow,
  dark again after 1.2 s.
- **The lightning's light** (`RenderSettings::lightning`, off by default, with clouds): the flashes within 30 km
  light the scene's ambient light from the sky (and a third of it from the ground), halving at 2 km, and glow in
  the clouds around the brightest of them, halving at 6 km, 800 m above the cloud base. No bolts are drawn; the sky
  cube's faces, drawn in turns, do not flash. `RenderStats::lightning` (the light at the camera, about 1.3 at the
  peak of a bright flash within a kilometre) and `lightningFlashes`. Measured
  (`render.renderer_lightning_lights_the_storm`, a flash 985 m away): the ground from 0.024 to 0.313, a cloud
  overhead from 0.146 to 1.331.
- **The sound** (`audio::WeatherAudio`, [Audio](audio.md#weather-sounds)): two wind layers, a light wind (a soft
  rush) and a strong one (a roar with a howl), from `WeatherSoundState::wind` (the speed at the listener,
  `|windAt|`, so gusts swell them): they rise with the square root of the speed from 1 to 20 m/s, crossfade at
  equal power from light to strong between 6 and 16 m/s and are halved under cover. Thunder:
  `WeatherAudio::strike(distance, brightness)` for each flash a game passes on once; it plays distance / 343 m/s
  later by `update()`'s clock, its gain 1 / (1 + distance / 1.5 km) times the brightness and 1 - 0.3 cover, the near
  clap (with its crack) giving way to the far rumble at equal power from 1 to 5 km, nothing beyond
  `WeatherSoundDesc::thunderReach` (25 km); at most 16 wait, the farthest dropped. The defaults are procedural
  (`tools/assets/make_weather_sounds.py`): `wind_light.wav` (RMS 0.05), `wind_strong.wav` (0.11), `thunder_near.wav`
  (0.10, 6 s) and `thunder_far.wav` (0.08, 8 s); the heavy rain's loop went from 0.2 to 0.13 so that heavy rain, a
  gale and near thunder together stay below full scale (the demo's windy storm clipped once at 0.2).

Where the build differs from the agreed plan: no slowdowns behind terrain or buildings (the field knows no
geometry; a game lowers the wind it passes on, as the sound's cover halves it); the plants' bend is a rigid turn
about the root with a height falloff, not a simulated stem; the gusts do not move the clouds.

## Phones and fallbacks

The demo's phone defaults (`App::mobile()`): volumetric at half size with the accumulation, 48 steps, one sky cube
face per frame, 60 frames per second. They come from the costs on the M4 Pro at a phone's size (below) and the
stage 11 ratio of about 3 between the iPhone 17 and the M4 Pro: about 4 ms on the iPhone 17, an estimate, over the
2 ms the roadmap set for the clouds; the 2D layer (about 1 ms by the same ratio) is the cheap setting. The rain on a
phone: `Low` without the shafts (so without the rainbow), 0.25 ms on the M4 Pro at the iPhone 17's size, about
0.7 ms on the phone by the same ratio, within the 1 ms set for the rain; `Low` with the shafts would be about 1.5 ms.
The snow on a phone: `Low` flakes beside that rain, without the shafts, 0.34 ms for a snowfall on the M4 Pro at
the iPhone 17's size, about 1 ms on the phone by the same ratio. The wind on a phone: the same levels with gusts
and eddies (0.16 ms for the `Low` drops in the windy storm at the iPhone 17's size, about 0.5 ms by the ratio); the
plants' sway costs nothing measurable on the Mac. Not measured on a phone yet (the user looked at the section on the
iPhone 17 on 2026-11-08 and reported it fine): the defaults wait for a measured run.

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

**The rain** (`scenarios/rain_cost.txt`, the same way: the Rain here preset from the ground and the storm preset
from the air, half-size clouds, the snow off, 3 s per setting; 2026-11-08 00:57, with the splashes of part 3's
polish): `rain_map`, `rain_shafts` and `rain`, and the HDR pass's growth over rain off (the wet surfaces); "added" is
their sum, ms.

| Setting | 1920 x 1080: map, shafts, drops, HDR | added | 1206 x 2622: map, shafts, drops, HDR | added |
|---|---|---|---|---|
| Ground, Low | 0.02, 0.19, 0.09, 0.09 | 0.38 | 0.02, 0.27, 0.12, 0.11 | 0.53 |
| Ground, Medium | 0.02, 0.49, 0.15, 0.09 | 0.75 | 0.02, 0.66, 0.19, 0.11 | 0.98 |
| Ground, High | 0.02, 0.69, 0.49, 0.11 | 1.30 | 0.02, 0.92, 0.52, 0.14 | 1.60 |
| Ground, Low without shafts (phone defaults) | 0.02, -, 0.09, 0.09 | 0.20 | 0.02, -, 0.12, 0.11 | 0.25 |
| Storm from the air, Low | 0.02, 0.24, 0.09, 0.06 | 0.41 | 0.02, 0.36, 0.13, 0.06 | 0.58 |
| Storm from the air, Medium | 0.02, 0.87, 0.15, 0.06 | 1.10 | 0.02, 1.36, 0.20, 0.06 | 1.64 |
| Storm from the air, High | 0.02, 1.30, 0.48, 0.09 | 1.88 | 0.02, 2.04, 0.50, 0.09 | 2.65 |

The first measurement (2026-11-07 23:10, part 2's 1024 and 3072 splashes) gave 0.35, 0.68 and 1.17 ms from the
ground and 0.37, 1.04 and 1.73 ms from the air at 1080p: the drops cost 0.01 to 0.03 ms more with the splashes'
counts, and the shafts and the HDR pass 3 to 8 % more in this session (not compared in one session). The shafts cost
the most where rain fills the view (a storm seen from the air); at full size they took 1.6, 2.7 and 4.4 ms there at
1080p (part 2's first runs), hence the reduced size. Their upsample is most of `Low`'s shaft time. The drops cost by
their count where rain falls near the camera (every instance runs) and nothing where none does.

**The snow** (`scenarios/snow_cost.txt`, 2026-11-08 01:03, the same way with the weather's clock stopped: the
Snowfall preset from the ground and the Snow showers preset from the sky view, one shower over the camera; the
rain's drawing off but for the phone defaults): `rain_map`, `rain_shafts`, the drops' range (`rain`: with no rain
near, the copy of the image the flakes are drawn over), `snow` and the HDR pass's growth over snow off (the covered
surfaces); "added" is their sum, ms. The HDR pass alone 0.69 (ground) and 0.46 ms (sky) at 1920 x 1080, 0.85 and
0.72 ms at 1206 x 2622.

| Setting | 1920 x 1080: map, shafts, drops, flakes, HDR | added | 1206 x 2622: map, shafts, drops, flakes, HDR | added |
|---|---|---|---|---|
| Ground, Low | 0.02, 0.20, 0.04, 0.04, 0.06 | 0.35 | 0.02, 0.27, 0.06, 0.04, 0.10 | 0.48 |
| Ground, Medium | 0.02, 0.58, 0.04, 0.07, 0.06 | 0.76 | 0.02, 0.71, 0.06, 0.07, 0.10 | 0.96 |
| Ground, High | 0.02, 0.78, 0.04, 0.11, 0.07 | 1.02 | 0.02, 0.98, 0.06, 0.12, 0.11 | 1.28 |
| Ground, Low without shafts (phone defaults) | 0.02, -, 0.04, 0.04, 0.14 | 0.23 | 0.02, -, 0.07, 0.04, 0.22 | 0.34 |
| Snow showers from the sky view, Low | 0.02, 0.19, 0.04, 0.04, 0.04 | 0.33 | 0.02, 0.30, 0.07, 0.04, 0.08 | 0.51 |
| Snow showers from the sky view, Medium | 0.02, 0.54, 0.04, 0.07, 0.04 | 0.71 | 0.02, 0.94, 0.07, 0.07, 0.08 | 1.18 |
| Snow showers from the sky view, High | 0.02, 0.76, 0.04, 0.11, 0.04 | 0.97 | 0.02, 1.36, 0.07, 0.12, 0.08 | 1.66 |

The snow's shafts cost more than the rain's under a snowfall (they begin nearer, where the flakes end). Their first
version took 2.21 ms at `Medium` and 3.71 at `High` from the ground at 1080p: the step's snow share took the
weather field's uniform block as a function argument, and the shader compilers copied the whole block at every
step; reading it in place took them to 0.58 ms. The flakes cost by their count (0.04 to 0.12 ms) where snow falls
near the camera, nothing where none does (nor the copy of the image).

**The wind** (`scenarios/wind_cost.txt`, 2026-11-08 02:30, the same way with the weather's clock stopped: the
meadow in the windy storm, 79 swaying instances of about 2300 tufts of grass, 12 bushes and 3 trees; the rain and
snow at `Medium`), ms:

| Range | 1920 x 1080 | 1206 x 2622 |
|---|---|---|
| HDR pass, the plants still (no wind) | 1.52 | 1.42 |
| HDR pass, the plants swaying in the gale | 1.52 | 1.46 |
| Drops (`rain`), steady wind / with gusts and eddies | 0.18 / 0.38 | 0.22 / 0.51 |
| Flakes (`snow`), steady wind / with gusts and eddies | 0.09 / 0.14 | 0.10 / 0.14 |
| Drops at `Low` with gusts (phone defaults) | 0.13 | 0.16 |

The sway costs nothing measurable (a run's noise in the shadows range is 0.1 ms). The gusts cost in the drops and
flakes, every instance evaluating the field at its place: 0.2 ms for 24576 drops at 1080p. The eddies' first noise
(three separate value noises per instance) took the drops with gusts to 0.61 ms and the flakes to 0.23; one hash
split into three values took them to 0.44 and 0.15. The lightning costs nothing on the GPU (a branch in the sky)
and 0.001 to 0.002 ms on the CPU for its query. The meadow's first build, one instance per tuft (2131 instances),
cost about 0.2 ms of frame CPU time in every view of the section (mean CPU median over the rain cost's nine
regions 1.17 ms against 0.97 without the meadow); as 64 patches of grass it costs nothing measurable (0.95 ms).

## Demo and tests

The demo's "Weather" section (14; [Engine demo](demo.md)): a plain 20 km wide with hills, towers and a lake seen
from a few hundred metres up; presets (fair weather, sun shower, towering, storm, overcast, cirrus, rain here,
downpour, passing shower, snowfall, thaw, snow showers, windy storm), coverage, type, variation, precipitation,
cirrus, a raining cumulonimbus as a cell, wind speed and direction, gusts, turbulence, change of shape, the
weather's speed, the temperature, the sun, Run day and Sunset, the quality (full size, half size, 2D layer, off),
steps, cloud shadows, the sky cube and its faces per frame, 60 Hz; the rain's and the snow's levels, shafts,
rainbow, lightning and its rate, sound and its volume; views from the air, from the ground, toward the cloud, from
the shelter, over the shelter and in the meadow. On the ground: wet paving with puddles, a pond, a shelter with a
crate under it and one in the open, a dark block that the rain shows against; beside it a meadow of grass, bushes
and trees on the demo's plant surface (`examples/demo/shaders/plants.hlsl`, `quenchWindSway` and
`quenchWindSwayAbout`). The windy storm: a gale of 22 m/s from the west-southwest (about 10.5 m/s at 10 m) with
gusts 0.6 and turbulence 0.4 under a thunderstorm cell over the meadow, its lightning four times as frequent, the
lightning on. The Physics section's wind (a slider and gusts) blows the light balls it drops with `WindDrag`. Its batch
parts: `weather` checks the cell's rain, the distance to it and the cover, the march's
size and the fallback to the layer, with three golden captures per backend (`demo_weather.png`,
`demo_weather_sunset.png`, `demo_weather_layer.png`, which show the shafts since part 2); `rain` checks the rain at
the camera, the streaks and splashes, the cover under the shelter, the wetness, the sound's layers and level, a
passing shower drying and its distant rain, and the rain off, with three captures (`demo_rain.png`: the streaks,
puddles and rings, the dry floor under the shelter; `demo_rain_rainbow.png`: toward the cloud with the rainbow;
`demo_rain_shelter.png`: from under the roof); `snow` checks the snowfall's share, the flakes, the full cover with
no wetness, the shelter's cover and the silence, the cover building over the shelter from bare ground (0.26 after
5 s at 60 times the weather's speed) with the floor under the roof bare, its thaw into wet ground and puddles (cover
0.18, wetness 0.16, standing water 0.04) and the snow showers' shafts, with four captures (`demo_snow.png`: flakes
and the white ground, the shelter's floor bare; `demo_snow_building.png`: from over the shelter, the first patches
on the ground, the roof and the crates; `demo_snow_thaw.png`: patches left on wet paving; `demo_snow_sky.png`:
the snow showers' white curtains); `wind` checks the meadow's plants, the calm's silence, the windy storm's mean
and gusting wind at 10 m and its sound layers, a flash's light and its thunder heard after the sound's delay,
thunder after thunder without clipping at six times the preset's lightning, and the lightning off, with four
captures (`demo_wind_calm.png`: the meadow upright under a low sun; `demo_wind.png` and `demo_wind_gust.png`: the
grass and the crowns bent downwind in a lull and two seconds later, the rain slanting; `demo_wind_flash.png`: five
milliseconds after a flash within 3 km); `physics_wind` checks the wind near the floor (4.8 m/s at a 20 m/s
setting) and three light balls blown 2.7 m downwind in 3 s. Check mode items `weather.clouds`, `weather.rain`,
`weather.snow`, `weather.wind` (in motion), `weather.rain_sound` and `weather.wind_sound` (by ear) are for a
person; the user looked at the section and listened to it on the Mac and the iPhone 17 on 2026-11-08 and reported it
fine.

Tests: `weather.*` (21 cases: the coverage's mean by setting, determinism, drift, evolution, seeds, type and
precipitation, a cell in a clear sky and the distance to it, cells that drift and stay, wind and cloud tops, the
grid against single samples, queries on the job system's threads, a sample's cost; the wetness building and drying,
under a passing shower, and the grid following its focus; rain or snow by the temperature and a cell's own share,
the snow cover building and melting, the distance to rain or to snow; the gusts' and the turbulence's deviations,
the offset against the gusts and the period's seam, lightning only in storms and the same however a span is cut),
also under Node.js and with the sanitizers; `physics.physics_wind_drag`; `audio.weather_audio_gains` (the wind's
layers and the thunder's rule too), `weather_audio_mix` (the layers by the strongest frequency of an offline mix,
the wind's layers, thunder after its delay and the farthest dropped, a game's sound, a hot reload, the bus, silence
and off) and `weather_default_sounds` (the default loops and the thunder: levels, seams, where their energy is); `gpu_device.texture_3d_from_layers` and `cube_faces` on every backend variant (a 3D texture
filled from the layers of a 2D array; a cube's faces drawn with `quench/Cube.hlsli`'s directions and sampled back,
with its mips); on every backend variant `render.renderer_clouds_shadow_the_ground` (off by default changes
nothing; a raining cell's shadow on the ground, 0.073 under it against 0.775 in the sun, in both modes; the sky
cube's faces; no clouds after `clearWeather`), `renderer_rain_wets_the_open_ground` (open ground 0.521 dry, 0.288
wet; under a roof 0.521 both; the cover 0 in the open and above 0.9 under the roof; the streaks; the puddles' rings
moving; no rain with the path tracer or without a weather), `renderer_rain_shafts_from_afar` (none without
clouds; with them a shaft over 737 of 4096 pixels), `renderer_snow_covers_the_open_ground` (open ground 0.318 bare,
0.875 under snow; under a roof 0.318 both; a material with `snow = 0` bare; flakes over 451 of 4096 pixels; no
streaks where it snows), `renderer_snow_shafts_from_afar` (the mean change by a shaft: rain 6.4, snow 14.6,
snow with only the rain drawn 0.16), `renderer_wind_sways_plants` (a stem upright without a weather and in a calm,
bent downwind either way the wind blows, and bent about its own foot when rooted below each vertex) and
`renderer_lightning_lights_the_storm` (above).

## Later parts

All four parts are built (above); where they differ from their plans is said in [Rain](#rain), [Snow](#snow) and
[Wind](#wind). Footprints in the snow are out of scope unless asked. The user looked at it and listened to it on
the Mac and the iPhone 17 on 2026-11-08 (fine); what waits is the costs on a phone and the iPhone 18 Pro Max
([Limitations](#limitations)).

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
| WebAssembly under Node.js | `weather.*` (the headless module, the snow's and the wind's cases too), `audio.weather_audio_*` and `physics.physics_wind_drag` |
| iPhone 17 | the demo's Weather section by hand (the user, 2026-11-08, the demo build of `b91aff0`): looked at in motion and listened to, "yes, everything is fine"; no tests, no costs measured |
| iPhone 18 Pro Max | not run |

## Limitations

- Looked at and listened to once, as a whole: the user looked at the clouds, the rain, the snow and the wind in
  motion and listened to the rain, the wind and the thunder on the Mac and on the iPhone 17 on 2026-11-08 ("yes,
  everything is fine"), a report on the section as a whole, not item by item (check mode items `weather.clouds`,
  `weather.rain`, `weather.snow`, `weather.wind`, `weather.rain_sound`, `weather.wind_sound`); the captures were
  looked at still, the sound measured. The Physics section's wind was not named in the report.
- The wind: the plants sway in their vertex shader only (motion vectors, acceleration structures and rays see them
  upright); no slowdowns behind terrain or buildings; no bolts are drawn and the sky cube does not flash; thunder is
  one sound per flash, not echoes from the terrain; `WindDrag` has no torque, lift or shelter.
- The traced effects see an overcast amount, not the clouds (above); the probe grid does not pick up single clouds.
- Cloud shadows are a 24 km map on one plane (`cloudShadowHeight`, 0 by default): a surface far above or below it
  reads the shadow of the column its sun ray crosses at that height, so ground at very different heights (a valley
  and a mountain top, isles at several heights) shares one plane; beyond the map its edge holds.
- The storm's bases are somewhat smooth and sheet-like. Inside a storm's rain (the Storm preset from the air) the
  view is an even veil of heavy rain with its streaks, about a kilometre of visibility, without curtains or gaps at
  that range; until 2026-11-08 the veil was lit by the sky's ambient term, pi times the clouds' bases, and shone as
  a pale grey fog brighter than the clouds above it (now darker, with the streaks and the ground showing). The
  shafts of the 2D layer's clouds look like smoky blobs more than rain.
- Clouds beyond 64 km fade out (the weather map's reach). A camera above the cloud base (since 2026-11-08): its
  light counts only the clouds above it (`Weather::coverAbove`) and the volumetric clouds below the horizon are
  drawn (a sea of clouds under the camera); the 2D layer is still drawn only above the horizon. The clouds' shadow
  map lies on the plane y = 0 unless the game sets `cloudShadowHeight` (since 2026-11-08): with 0, surfaces above
  the clouds are shaded as if the clouds were above them, so a game over a sea of clouds sets it to its ground's
  height.
- Phone costs are estimates from the Mac; the phone defaults wait for a measured run on the iPhone 17.
- The rain: no roofs beyond 64 m of the camera (the occlusion map's reach: farther surfaces count as open) and
  nothing above 100 m over the camera counts as a roof; the wetness grid covers 1 km around its focus (beyond it an
  estimate without the rain's history); wetness knows no roofs (`Weather::wetness` is open ground; the renderer
  applies the occlusion map, a game asks its own physics); the cover is the camera's only (`RenderStats::rainCover`),
  a few frames late. The streaks do not refract the scene behind them. The ripples are rings of a procedural rain,
  not a simulation; no drips from roofs, no runoff, no wet footprints. Ray hits do not apply the rain (traced reflections and the probe grid see dry surfaces), the streaks
  are not in any reflection, and the path tracer's frame has no rain.
- The snow: the cover is the weather's open ground, as the wetness is, a whitening of the surface, not geometry (no
  depth, no drifts against walls, no footprints); beyond 64 m of the camera floors under roofs whiten too (the
  occlusion map's reach); the occlusion map looks at most 17 degrees off the vertical, so slanted snow reaches less
  far under the edges of roofs than it would; the grid melts by the temperature at y = 0 and its estimate after
  `setTime` assumes the present temperature held. Ray hits and the path tracer see no snow (as with the rain).
  Sleet is a mix of the rain's and the snow's drawing, not a particle of its own. A sunbeam through a gap in the
  clouds shows in the snow's shafts as a bright line.
- The sound: one set of layers for the listener, not per surface (leaves, puddles, metal); the defaults are
  procedural noise, measured, and listened to by the user on the Mac and the iPhone 17 (2026-11-08: fine).
