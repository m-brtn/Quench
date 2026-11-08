# Render module

`quench_render` (stage 5 of the [Roadmap](roadmap.md)): a retained scene and the standard frame drawn from it. Public
headers in `include/quench/render/`, sources in `src/render/`, the shader library in `shaders/quench/`. Verified on
the Mac with Metal and with Vulkan through MoltenVK, and in the iOS Simulator; on the user's iPhones the stage 10
code ran the device tests and the demo's benchmarks (2026-11-06), the stage 11 effects and the stage 12 area lights
(the demo's tunnel) ran on the iPhone 17 only, measured and captured, not looked at on its screen
([Performance](performance.md#the-tunnel-on-the-iphone)). The engine demo (`examples/demo`, [Engine demo](demo.md))
shows every part of it.

The code comes from TrailForge (`b97883e`: `src/render/Renderer.*`, `shaders/scene.hlsl`, `TimeOfDay.*`), generalized:
no terrain, vehicles, trees or other game layers, and no fixed material enum. Where TrailForge and Ashfall (`2d5db57`,
`shaders/scene.hlsl`) had identical shader functions, the comment says so.

## Using it

```cpp
#include <quench/render/MeshBuilder.h>
#include <quench/render/Renderer.h>

using namespace quench;

auto renderer = render::Renderer::create(device);          // Result<Renderer>; the engine pipelines are created here
render::Scene scene(device);                                // meshes, textures, materials, instances, lights
const render::MeshData sphere = render::sphereMesh(0.5f);
const render::MeshId mesh = *scene.addMesh(sphere.desc("sphere"));
const render::MaterialId red = scene.addMaterial({.baseColor = {0.8f, 0.1f, 0.1f, 1.0f}, .roughness = 0.3f});
const render::InstanceId ball = scene.addInstance({.mesh = mesh, .material = red, .transform = model});
const render::Daylight day = render::daylightFor(towardSun);
scene.addLight({.type = render::LightType::Directional, .color = day.sunColor, .direction = -towardSun});
scene.setEnvironment(day.environment);

// Every frame (Game::render): move what moved, then draw into the frame's target.
scene.setTransform(ball, model);
renderer->render(scene, {.view = camera, .fovY = fov, .nearPlane = 0.1f, .time = t}, frame.commands, frame.target);
```

- **Handles** (`MeshId`, `TextureId`, `MaterialId`, `InstanceId`, `LightId`, `SurfaceId`, `PassId`) are plain values
  with a generation: a removed handle stays invalid when its slot is reused (`contains()` tells). Using a stale or
  null handle is a programming error (`QUENCH_ASSERT`; ignored in release builds).
- **Meshes** use the engine vertex (`render::Vertex`, 64 bytes: position, normal, uv, linear color, tangent) and
  32-bit indices, counter-clockwise front faces. The tangent is glTF's (xyz along +u, w the sign of the bitangent
  `w * cross(normal, tangent)`, which points toward -v): MikkTSpace's frame, the one normal map bakers use.
  `MeshBuilder.h` makes boxes, planes, spheres and tori with their tangents, and `computeTangents` gives any other
  procedural mesh MikkTSpace tangents (it may split vertices on mirrored uv seams). Bounds for culling are computed
  on upload.
- **Textures** are RGBA8 (`TextureData`, sRGB or linear), mipmaps generated on the GPU. Loading files is stage 6.
- **Materials** replace the games' fixed material enums: base color, emissive, roughness, metallic, alpha cutoff,
  blend mode (opaque, alpha blend, additive), double-sided, shadow mode, four textures and four `vec4` parameters
  for custom surfaces. The standard surface reads texture 0 as base color (sRGB), 1 as glTF's metallic-roughness,
  2 as a tangent-space normal map (linear, glTF's convention: +y toward -v) and 3 as emissive (sRGB, multiplied with
  the material's emissive). The shadow mode (`ShadowMode`) is `Auto` by default: opaque materials cast solid shadows,
  alpha-blended ones translucent shadows, additive ones (light) none; `None`, `Solid` and `Translucent` override it.
  The alpha cutoff cuts the shadow too.
- **Instances** carry a transform, a color, a `vec4` for the surface shader, visibility and shadow casting. The
  previous transform is kept for motion vectors; `setTransform(..., teleport = true)` drops it (a respawn).
- **Lights**: the first directional light is the sun (cascaded shadows, the sun disc in the sky). Further directional,
  point, spot and area lights (rect, disk, tube; [Area lights](#area-lights)) light surfaces without shadows; per
  frame the 16 nearest to the camera are used, at most 8 of them area lights. A light with zero color or intensity
  is off.
- **Environment**: sky colors, atmosphere model or gradient, sun disc, ambient, height fog, exposure.
  `render::daylightFor(towardSun)` gives a sun color and an environment for any sun elevation (night, twilight,
  sunrise, day: TrailForge's time-of-day palette).
- **Units**: meters, linear HDR. A directional light's `color * intensity` is the irradiance on a surface facing it;
  a point or spot light's is the irradiance at 1 m (inverse square with a smooth window to zero at `range`); an area
  light's is the radiance of its surface, the value an emissive material of the same look would have.

The scene is retained so that later backends can build from it (ray tracing: acceleration structures from the meshes
and instances, stage 9). One `render()` per scene and frame: motion vectors are measured from the scene's previous
render, and switching cameras or scenes needs `View::cut`. A cut (also one the renderer makes itself: a new size, a
camera that jumped more than 6 m or turned more than about 25 degrees) restarts the TAA and upscaler jitter, the
shaders' per-frame noise and the probe rays' rotation, so a frame depends only on the frames since the last cut, not
on how many were rendered before it (in a scripted run, the frames spent waiting for loads in real time).

## The frame

`Renderer::render` records, in this order:

0. **Ray tracing**, where a traced effect runs (below): the acceleration structures, a prepass of the opaque
   surfaces (depth; with reflections, indirect light or the denoising upscaler also a G-buffer of normal, roughness,
   albedo and metallic), the motion vectors when the temporal denoiser needs them, then full-screen passes that
   trace the sun's and the local lights' shadows and the ambient occlusion, update the probe grid, trace the
   reflections and the indirect light, each followed by its filter. With ray-traced sun shadows the cascades of step
   1 are not drawn. The path tracer replaces steps 0 to 3 (below). Then screen-space ambient occlusion (GTAO,
   below) and screen-space reflections (SSR, below) where they run, after a prepass of their own when nothing is
   traced (with SSR also the G-buffer); after the HDR pass SSR copies the image for the next frame. Then, with a
   weather on the scene and `clouds` on, the clouds' passes (below): the weather map, the cloud shadow map, the
   volumetric march and its accumulation, the sky cube's faces; with `rain` or `snow` on, the rain occlusion map and
   the weather maps' rain regions (below).
1. **Shadow cascades** of the sun (standard depth, `Depth32Float` texture array): the view is split between linear and
   logarithmic distances up to `shadowDistance`, each cascade snapped to its texels; casters are culled per cascade.
   Solid casters write depth with their position only; cut-out ones run their surface and discard where it is cut
   out. Game `Shadow` passes run in each cascade after the scene's casters. A cascade with translucent casters then
   gets a second pass into its transmittance map (below).
2. **HDR pass** (`Rgba16Float`, reversed depth: near 1, far 0, infinite far plane): opaque surfaces sorted by
   pipeline and material, then game `Opaque` passes, then the sky (atmosphere or gradient, sun disc, fog toward the
   horizon; with clouds, the sky with the clouds over it), then alpha-blended and additive surfaces back to front, then game `Transparent` passes.
   With rain or snow and clouds, the shafts of rain and snow and the rainbow over the HDR image.
3. **Motion vectors and TAA**: camera motion from the depth, moved instances drawn again into an `Rg16Float` target
   (cut-out ones only where the depth is their own, so their holes keep what is behind them);
   TAA with a Halton (2, 3) jitter over 8 frames, the history clipped to the variance box of the 3 x 3 neighbourhood
   (YCoCg), then sharpening. Render scale below 1 renders the 3D part smaller; tone mapping scales it to the target,
   or MetalFX does (below), whose temporal upscaler takes the place of TAA's resolve.
4. With rain, its streaks and splashes, with snow its flakes, on the anti-aliased (and upscaled) HDR image; then
   game `PostHdr` passes.
5. **Bloom**: a threshold with a soft knee, a chain of downsamples and upsamples.
6. **Tone mapping** into the target (ACES or AgX, exposure, vignette, dither; sRGB and UNORM targets get the same
   display values; HDR output below), FXAA when selected, then game `Overlay` passes. The app draws ImGui after the
   game.

Shading (`quench/Surface.hlsli`): GGX specular and Lambert diffuse for the sun and the local lights, the sky as
ambient and as a reflection that blurs toward the ambient with roughness, height fog (TrailForge's `shadeSplitEnv`
without its game terms).

### Sky and sun

The sky is TrailForge's: single scattering in a flat exponential atmosphere (Rayleigh and Mie, Kasten-Young air mass),
scaled to the palette's zenith brightness and mixed with the horizon-zenith gradient, which also stands alone as the
simpler model (`Environment::atmosphere = 0`). The sun disc (`Environment::sunDisc`, `sunAngularRadius`) is added to
the sky radiance, the sky being light scattered in front of the sun, with at least 4 times the sky's luminance at the
sun's centre (`QUENCH_SUN_OVER_SKY` in `Sky.hlsli`). At a low sun the scattering sky's Mie peak is brighter than the
palette's disc (computed from the shader: 0.14 times at 5 degrees elevation, 0.57 at 10, equal at 14); TrailForge's
disc replaced the sky and showed as a darker circle in the glow there.

In SDR, below about 15 degrees the glow within 5 to 8 degrees of the sun is also above the tone curve's white point
at the palette's exposure, so disc and glow are both white and the disc is not visible as a disc; from about 20
degrees it is (25 degrees: disc 255, sky next to it 251 to 253). In HDR output the disc stays above the glow
(`render.renderer_sun_disc_outshines_its_glow`: sun at 9 degrees, headroom 8, disc 7.95 against at most 5.72 in the
glow; in SDR the disc is never darker than the glow). There is no auto exposure yet (TrailForge has one), which would
lower the exposure in front of a bright sky.

### Clouds

Since 2026-11-07 (stage 13, part 1; [Weather](weather.md)): `Scene::setWeather(const weather::Weather&)` gives the
scene a weather (a copy, set again after each `Weather::update`; `clearWeather()` removes it), and
`RenderSettings::clouds` draws its clouds: `Off` (the default), `Layer` (a 2D layer at the cloud base, the fallback
and the cheap setting) or `Volumetric` (raymarched cumulus, towering cumulus, cumulonimbus, stratus and cirrus after
Nubis, at `cloudResolution` of the render size, accumulated over frames with `cloudTemporal`, `cloudSteps` along a
ray). `cloudShadows` shades the sun's light on surfaces through a 24 km cloud shadow map around the camera, with
shadow maps and with ray-traced sun shadows; `skyCubeSize` and `skyCubeFacesPerFrame` keep a cube of the sky with its
clouds that surfaces reflect (and SSR's misses). As the cover around the camera closes, the sky's ambient light
moves toward a deck's grey, the ground's ambient light and the fog follow. The traced effects' misses see the sky
blended toward that grey by the cover, not single clouds. Timed ranges `cloud_noise` (once), `weather` (the weather
and shadow maps, drawn in turns), `clouds` (the march and its accumulation) and `sky_cube`. `FrameUniforms`
`cloudShadow`, `skyCube` and `overcast`, surface samplers 14 (the shadow map) and 15 (the cube); both are bound in
the HDR pass (placeholders without clouds), and `quench/Clouds.hlsli` reads them. Test
`renderer_clouds_shadow_the_ground` on every backend variant; the costs in [Weather](weather.md#cost).

### Rain

Since 2026-11-07 (stage 13, part 2; [Weather](weather.md#rain)): `RenderSettings::rain` (`Off` by default, `Low`,
`Medium`, `High`) draws the rain of the scene's weather where its precipitation falls. A **rain occlusion map** (the
shadow casters drawn along the rain into a 512² depth map over 128 m around the camera, without the cascades' slope
bias; timed range `rain_map`) keeps the rain and the wetness from under roofs and gives the cover over the camera (`RenderStats::rainCover`, read
back a few frames late). **Surfaces** get wet where the rain reaches them, by the weather's remembered wetness
(`quench/Rain.hlsli`, `quenchApplyRain` in the surface entries and the G-buffer entry): darker and glossier,
standing water in the hollows of materials with `MaterialDesc::puddles`, rings of drops on it and on `water`
materials; `MaterialDesc::wetness` (1 by default) scales it, `QUENCH_SURFACE_NO_RAIN` opts a surface file out.
**Streaks and splashes** (procedural instances around the camera; range `rain`) are drawn after TAA and upscaling
and tested against the depth in the shader. With clouds, `rainShafts` (on by default) marches the **shafts** of
rain under raining clouds at a quarter or half of the render size after the HDR pass (range `rain_shafts`), and
`rainbow` (off) adds a rainbow to them. Surface sampler 14 holds the **weather maps** since: one Rgba16Float texture
of the clouds' shadow, the rain occlusion map and the wetness grid (`QUENCH_WEATHER_MAPS_*` in
`quench/SceneUniforms.h`); `FrameUniforms` `rainViewProj`, `rain`, `rainGrid` and `MaterialUniforms::weather`. Tests
`renderer_rain_wets_the_open_ground` and `renderer_rain_shafts_from_afar` on every backend variant; the costs in
[Weather](weather.md#cost).

### Snow

Since 2026-11-08 (stage 13, part 3; [Weather](weather.md#snow)): where the weather's temperature makes its
precipitation snow, `RenderSettings::snow` (`Off` by default, `Low`, `Medium`, `High`) draws **flakes** that flutter
and drift with the wind (procedural instances around the camera after the rain's streaks; range `snow`), and the
weather's remembered **snow cover** whitens surfaces that face up where the snow reaches them (`quenchApplySnow` in
the surface entries and the G-buffer entry, after `quenchApplyRain`; patches that close as it deepens).
`MaterialDesc::snow` (1 by default) scales it, `QUENCH_SURFACE_NO_SNOW` opts a surface file out. The rain's
occlusion map, its shafts (denser and whiter where it snows) and the weather maps (the snow cover beside the
wetness grid) serve both; the rain is not drawn where it snows, nor the flakes where it rains. `FrameUniforms::snow`
and `MaterialUniforms::weather.w`; `RenderStats::snow`, `snowAtCamera`, `snowShareAtCamera`, `snowFlakes`. Tests
`renderer_snow_covers_the_open_ground` and `renderer_snow_shafts_from_afar` on every backend variant; the costs in
[Weather](weather.md#cost).

### Wind and lightning

Since 2026-11-08 (stage 13, part 4; [Weather](weather.md#wind)): the weather's wind with its gusts and eddies
(`weather::Wind::gusts`, `turbulence`) carries the rain's drops and the snow's flakes off their mean paths and turns
their streaks, in the same field a game's `Weather::windAt` gives. `FrameUniforms` `windAir`, `windSpeed` and
`windTime` hold it for any shader (0 without a weather): `quench/Wind.hlsli` has `quenchWindAt(p)` and
`quenchWindSway` / `quenchWindSwayAbout`, which bend a plant's vertices and normals downwind in a surface shader's
`QUENCH_SURFACE_VERTEX` function (build `surface_shadow_vs` too, so that its shadows sway); the motion vectors, the
acceleration structures and the rays see the plants unbent. `RenderSettings::lightning` (off by default, with
clouds) lights the clouds around each of the weather's flashes within 30 km (`Weather::strikes`) and the scene's
ambient light by their distance; no bolts are drawn, and the thunder is the game's (`audio::WeatherAudio::strike`).
`RenderStats::lightning` and `lightningFlashes`. Tests `renderer_wind_sways_plants` and
`renderer_lightning_lights_the_storm` on every backend variant; the costs in [Weather](weather.md#cost).

### Tone mapping and HDR output

Exposure and vignette scale the scene before the tone curve, as a lens does: applied after it, the vignette lowers the
white point toward the edges, and a clipped sun near the top of the frame came out darker than the less darkened sky
below it. ACES is Narkowicz's fit with a pre-exposure of 0.6; AgX is Sobotka's with the "punchy" look.

`render(..., output)` takes a `gpu::DisplayOutput` (the app passes `Frame::output`): with
`SwapchainComposition::HdrExtendedLinear` and an `Rgba16Float` target the renderer writes linear BT.709 values with
paper white at `output.paperWhite` and at most `output.headroom` times it. The HDR curve is the SDR curve while the
largest channel stays below a knee (display value 0.6, `render::kHdrKnee`); above it an exponential shoulder starts
with the SDR curve's slope at the knee and approaches the headroom, and the color is scaled by the shoulder over the
SDR curve, so hue and saturation stay those of the SDR image. Headroom 1 gives the SDR image (the renderer test: at
most 2 levels apart, below the knee identical within 0.002). `include/quench/render/ToneMap.h` has the same curves on
the CPU (`toneMapSdr`, `toneMapHdr`, `hdrKnee`) for tests and for content set relative to the knee (the demo's
panels). Any other output into a float target, and `Hdr10` (PQ), give SDR values with one warning: the PQ encoding is
not written, because Apple's EDR path is extended linear and a PQ reference white cannot be checked without an HDR
display. FXAA in HDR finds edges on display values clipped at paper white. How the app chooses the output, draws the UI
and captures HDR frames: [App shell](app.md#hdr-output).

### Cut-out and translucent shadows

Cut-out casters (`alphaCutoff > 0`) are drawn into the cascades with their surface's `surface_vs` and
`surface_shadow_fs`, which runs the surface function and discards below the cutoff, as `surface_fs` does: texture
alpha, base color alpha, vertex alpha or an opacity the surface computes. Solid casters without a cutoff keep the
position-only pass.

Translucent casters (`ShadowMode::Translucent`, the default of alpha-blended materials) are not in the depth. Each
cascade that has them gets a transmittance map: one layer of an `Rgba16Float` texture array at half the shadow map
size, cleared to white, made when a frame first has translucent casters (24 MiB at the default 2048 and 3 cascades,
half the depth array's 48 MiB; none in scenes without them). `surface_transmittance_fs` writes the light the surface
lets through, `(1 - a) * lerp(1, hue, a)` with `a` its opacity and `hue` its albedo over the brightest channel (a
grey surface passes `1 - a` in every channel, a colored one tints toward its color as far as it is opaque), blended
by multiplying, so overlapping casters multiply; alpha keeps `1 - depth` of the caster nearest to the sun (blend
`Max`).
A receiver lit by the sun takes the map's color from 2 map texels behind that caster on (in shadow depth; fully from
4), so the caster's own surface and anything between it and the sun stay untinted; the sun's direct light is
multiplied by it and the ambient darkens by its luminance, as in a solid shadow. A receiver in the solid shadow does
not read the map. The map is read with 4 bilinear taps (1 with `shadowFilter = 0`), the depth with one `GatherAlpha`.

Why a map and not dithering alone: the map gives a smooth, colored shadow whose darkness follows the opacity
(`render.renderer_translucent_shadows_follow_opacity`: opacity 0.25, 0.5 and 0.75 pass 0.749, 0.504 and 0.249 of the
light, a red caster of opacity 0.5 passes 0.504 / 0.280 / 0.280 for the expected 0.5 / 0.278 / 0.278), costs a pass
only in cascades with translucent casters, and opaque-only frames shade as before (their captures are bit for bit the
same). The fallback (`TranslucentShadows::Dithered`, also selectable) dithers the opacity into the depth map with a
4 x 4 ordered pattern and lets the shadow filter smooth it: grey and grainy, overlapping casters shadow like the most
opaque one (their patterns coincide); the same test measures 0.504 at opacity 0.5.

### Area lights

Stage 12. `LightType::Rect`, `Disk` and `Tube`: `position` is the center, `direction` the normal of a rect or disk
(the way it shines), `tangent` the direction of a rect's width or a tube's axis; `LightDesc::size` is a rect's width
and height, a disk's radius (x) or a tube's length and radius. `twoSided` makes a rect or disk shine from both faces;
a tube shines all around. `color * intensity` is the radiance of the surface, `range` the distance from the shape
where the light fades to zero.

- **The shape is drawn.** The scene keeps a mesh, a material and an instance per area light (outside the scene's
  counts and handles) and moves them with `setLight`: an emissive surface of the light's radiance, so what is seen
  matches what lights, in the raster and in the path tracer. The back of a one-sided rect or disk is black and
  occludes; a tube is an open cylinder. The shapes cast no shadows of their own light (visibility rays skip them).
- **Raster: linearly transformed cosines** (Heitz et al. 2016) for the diffuse and the GGX specular light, with the
  fitted 64 x 64 tables of `selfshadow/ltc_code` (BSD-2-Clause, [Dependencies](dependencies.md#added-in-stage-12);
  turned into `src/render/LtcTable.inc` by `tools/make_ltc_table.py`), a 2-layer `Rgba32Float` texture read with
  manual bilinear loads (fragment sampler 12 of the surfaces, 6 of the ray hits). A rect is integrated as a polygon
  clipped to the horizon, a disk as an ellipse (Heitz and Hill 2017), a tube as a line integral times its diameter
  (no end caps). `quench/AreaLights.hlsli` holds the integrals; `quenchDirectLight`'s callers in `Surface.hlsli`
  (the standard surface and custom ones built on it) get them without changes. This runs on every backend.
- **Ray-traced shadows**: area lights with `castsShadows` are among the `localLightShadows` (at most 4, the nearest
  first): one ray per pixel and light toward a uniform point of the shape (a second point when the first is behind
  the surface or on the dark side), so the penumbra widens away from the caster; the temporal denoiser filters it like
  the other local lights. The visibility multiplies the LTC light (the light's mean visibility over its area, not
  weighted by where on the shape the light comes from). Without ray tracing they are unshadowed, or since
  2026-11-07 (0.11.0) take shadow maps soft by their shape where `localShadowMapSize` is set
  ([below](#local-light-shadow-maps)).
- **Path tracer**: next-event estimation picks one of the lights at random and an area light's point uniformly over
  its area (a tube over its facing half), and BRDF samples that hit a shape count with multiple importance sampling
  (power heuristic), so small bright lamps and large dim panels both converge.
- **Ray hits** (reflections, indirect light, the probe grid) light their hits with the area lights' LTC; rays do not
  see the shapes' emission (in the real-time frame the LTC covers it).
- **Tests** (all three backends, the traced ones on the native backend): `renderer_area_lights_match_their_integrals`
  against a CPU integral of the BRDF over each light's surface: rect -2.3 %, disk -1.2 %, tube -4.7 % (no end caps);
  a rect facing the camera shows 0.498 for its radiance 0.5. `renderer_path_tracer_area_lights` (1024 samples): rect
  +0.4 %, disk -2.4 %, tube +0.6 %. `renderer_ray_traced_area_light_shadows` (a 1 m box under a 1 m square): lit
  0.000 in the umbra, 0.326 in the penumbra, 0.961 in the open. `renderer_area_light_shapes_follow_their_lights`:
  the shapes move, turn, change sides and type and go with their light.

### Anisotropic GGX

Stage 12. `MaterialDesc::anisotropy` (0..1) and `anisotropyRotation` (radians), the model of
`KHR_materials_anisotropy`: the roughness along the direction is `mix(alpha, 1, anisotropy^2)`, across it `alpha`
(Burley's anisotropic GGX with the height-correlated Smith term). The direction is the vertex tangent (+u) turned by
the rotation toward the bitangent, so it needs vertex tangents (MikkTSpace from `quench_cook` or
`computeTangents`); `MaterialUniforms::shading` carries the strength and the rotation's cosine and sine. The same
lobe in the raster (`quenchDirectLight`, `QuenchSurface::anisotropy` and `anisotropyDirection`), in the ray hits
and in the path tracer, which samples the anisotropic visible normals. The sky's reflection, the traced reflections
and the area lights' LTC use a bent normal and the mean roughness instead (approximations). Test
`renderer_anisotropic_highlight_stretches`: a point light's highlight on brushed metal (roughness 0.3, anisotropy
0.8) is 6 x 6 pixels isotropic, 58 x 6 along the tangent and 6 x 57 turned by 90 degrees; the path tracer 58 x 6 and
6 x 58.

### Ray-traced shadows and ambient occlusion

`RenderSettings::sunShadows = SunShadows::RayTraced` and `ambientOcclusion = AmbientOcclusion::RayTraced` run where
`gpu::Caps::rayTracing` holds and the build has the ray query shaders (`cmake/QuenchDxc.cmake`): today the native
Metal backend (the default on Apple platforms since 2026-11-07) on a GPU with ray tracing: the Mac's M4 Pro and the
user's iPhone (stage 9's shadows and occlusion measured there on 2026-11-06: 8.0 ms GPU p50 at the native 1320 x
2868, over the 7.5 ms budget). Elsewhere the frame takes shadow maps and no occlusion (GTAO with
`screenSpaceFallbacks`, [below](#screen-space-ambient-occlusion)), with one log line that says why
(`Renderer::effects()` tells which runs). The default stays shadow maps, also on the native backend, until the user
has looked at them on the iPhone
([Roadmap](roadmap.md#stage-9-native-metal-backend-and-ray-tracing)).

- **Acceleration structures** (`src/render/RayTracing.cpp`, timed range `accel`): every instance that casts shadows is
  in the top level (instance mask 1: opaque and cut-out, 2: translucent). The top level is created refittable and
  refitted while the same bottom levels come in the same order (only transforms changed); it is built again when an
  instance comes or goes, a bottom level was replaced, and after 30 refits. One bottom level per mesh, built when a
  caster first uses it and kept with the mesh; the frame after the build (when its size is known) it is compacted
  into a copy of that size (`gpu::AccelerationStructurePass::queryCompactedSize`, `copyAndCompact`), once. A skinned
  instance within `shadowDistance` of the camera has a bottom level of its own over the skinning pass's output,
  refitted every frame and built again after 60 refits (the counts are staggered, so a crowd made in one frame does
  not rebuild in one frame); those are not compacted. A table per instance (`RayInstanceData`: offsets into a copy
  of the meshes' vertices and indices, the material's values and texture layers) lets the shaders read the surface
  at a hit: cut-out and translucent casters for the shadow rays, every instance for the hit passes (below).
  `render.renderer_ray_tracing_refit_and_compaction`: two boxes built (17536 bytes with the tables), compacted the
  next frame (16800), then the top level refitted; the refitted image against a fresh build mean 0.000.
- **Rays** (`prepass`, then `raytrace` with `rays` and `denoise` in it): the opaque surfaces' depth at the render size
  (cut-out ones discard where they are cut out), then `rt_shadow_fs` (`src/render/shaders/raytrace.hlsl`): per pixel
  the position from the depth and a normal from its neighbours' depths, `shadowRays` rays (1..8, default 2) over the
  sun's disc of `shadowSoftness` times `Environment::sunAngularRadius` (a Vogel disc, rotated per pixel and frame),
  and `occlusionRays` rays (default 2), cosine-weighted over the hemisphere, up to `occlusionRadius` (1 m): occlusion
  is `1 - occlusionStrength * mean(1 - distance / radius)`. A cut-out hit below its cutoff is passed through; a
  translucent one multiplies the light by its transmittance as the transmittance map does
  (`quenchTransmittanceOf`, each instance once per ray); translucent instances are not in the occlusion rays. The
  result is an `Rgba8Unorm` mask: rgb the sun's transmittance, alpha the occlusion.
- **Denoise**: `RayDenoiser::Temporal` (the default since stage 11) or `Spatial` (below, "Denoising"). Hard shadows
  without occlusion skip the sun's filter with either denoiser (its history starts again when they turn soft); the
  temporal filter cost about 0.8 ms at 1080p on the M4 Pro there before (docs/performance.md).
- **Shading**: surfaces read the mask at their pixel (`quenchRayMask`, fragment texture and sampler 6 of
  `Surface.hlsli`; `FrameUniforms::options.w`: bit 1 sun, bit 2 occlusion): the sun's direct light is multiplied by
  its rgb, the ambient occlusion by its alpha. Game surfaces built on `Surface.hlsli` get both without changes.
- **Tests** (`render_metal_native.*`, 960 x 540 or smaller targets): the same scene with rays and with maps, hard
  shadows (`renderer_ray_traced_shadows_match_shadow_maps`): 317 shadowed and 598 lit pixels in both, images mean
  0.93 apart (1.56 % of the pixels over 16 levels: the maps' filtered edges); after the caster moves, the shadow moves
  and no bottom level is built again. Cut-out fence: mean 2.42, 2.61 %; translucent glass at opacity 0.5, the light
  behind it: grey 0.504 (maps 0.497), red 0.504 / 0.280 / 0.280 (maps 0.497 / 0.280 / 0.280). A skinned caster: the
  first frame builds 2 bottom levels, the next ones refit 1, and the shadow has the posed hole. Occlusion: 0.822 of
  the open ground's light next to a box, 1.000 away from it and on its top.

### Ray-traced lighting

Stage 11. Each effect is a setting with a fallback chosen by `gpu::Caps` (the same needs as ray-traced shadows), off
by default until measured on the iPhone, and each runs from the prepass at the render size:

- **G-buffer and hits.** With reflections, indirect light or the denoising upscaler the prepass also writes the
  shading normal and roughness (`Rgba16Float`) and the albedo and metallic (`Rgba8Unorm`) of the opaque surfaces, from
  the surface's `surface_gbuffer_fs` (`SurfaceDesc::gbufferShader`; without it the standard surface's material values
  and maps). A ray that hits reads the instance's triangle and material from the tables (`RayHit.hlsli`): the
  interpolated normal and uv, the base color, metallic-roughness, normal and emissive textures as layers of two
  256 x 256 texture arrays (every material texture once, scaled down, no mip levels, at most 64 layers), and shades
  it with the standard surface's BRDF: the emissive light, the sun with a shadow ray, the local lights without shadow
  rays, and the probe grid or the sky's ambient light. Its albedo, roughness, metallic, normal, emissive and
  anisotropy are the standard surface's from the material's values, or, for a surface whose hits are in the game's
  `quench_add_ray_hits()` set (since 2026-11-07, 0.11.0; "Surface shaders" below), its own function's. The
  normal map (texture 2) is read in the raster's frame (`quenchNormalFromMap`: the vertex tangent made perpendicular
  to the interpolated normal of the side the ray hit, the bitangent's sign turned by a mirroring transform); a mapped
  normal that faces away from the ray's origin is dropped for the interpolated one. Since 2026-11-07 (0.10.0)
  the tracing passes (reflections, indirect light, probe rays, the path tracer) have a variant whose hits read
  normal maps (`raylight.hlsl`'s `_maps` entries, `QUENCH_RT_NORMAL_MAPS`), used in frames where an instance in the
  top level has one: the tangent frame cost the path tracer 0.2 to 0.5 ms (2 to 4 %) on the M4 Pro even in a scene
  without normal maps. Test `renderer_ray_hits_read_normal_maps`: a wall whose map turns its normal toward the sun
  is 4.07 times as bright as a plain one in the raster, 3.99 times in its mirror image (traced reflections) and
  3.80 times in the path tracer (1.00 and 0.97 without maps in the hits; 0.71 with the bitangent's sign flipped).
- **Local light shadows** (`localLightShadows`, 0..4, default 0): the nearest point and spot lights with
  `castsShadows` get one shadow ray per pixel each toward a random point of their sphere (`LightDesc::sourceRadius`,
  0: hard), cut-out and translucent casters as for the sun; the result is one channel per light of a second mask,
  filtered like the sun's (fragment sampler 9, `options.w` bit 4). Without ray tracing they are unshadowed, or since
  2026-11-07 they take shadow maps where `localShadowMapSize` is set (point and spot lights since 0.10.0, area
  lights after it; below, "Local light shadow maps"). The channels go to the lights in order of distance each frame: a light that
  takes a channel another light (or none) had starts from that channel's history, counted in
  `RenderStats::localShadowChanges` (in the demo's moving tunnel 4 to 10 times in 34 s, without a visible trace:
  [Performance](performance.md#the-tunnel-in-motion)). Test `renderer_ray_traced_local_light_shadows`: behind a box
  0, in the open 552 (unshadowed 552 in both); no change while the light keeps its channel, one when the shadows
  come back.
- **Reflections** (`Reflections::RayTraced`, default `Sky`): one ray per pixel along a GGX visible-normal sample of
  the pixel's roughness up to `reflectionRoughness` (0.5), faded into the sky's reflection above it; the hit is
  shaded as above, a miss sees the sky. The surface takes it in place of the sky's reflection (sampler 7, bit 8).
  Without ray tracing the sky's reflection, or since 2026-11-07 screen-space reflections with
  `screenSpaceFallbacks` ([below](#screen-space-reflections)). Test `renderer_ray_traced_reflections`: a mirror floor shows an emissive box's red texture where the sky's
  reflection was (247 7 14 against 66 105 198), with one material texture in the hit arrays.
- **Probe grid** (`IndirectLight::Probes`): 16 x 8 x 16 probes `probeSpacing` apart (2 m) at the cell centers of a
  world-aligned grid around `View::probeCenter` (default the camera; a top-down camera sets the point it looks at).
  Every frame each probe traces 64 rays (a spherical Fibonacci set turned per frame), and two passes blend them into
  its octahedral irradiance map (8 x 8 texels with the border, stored as irradiance / pi) and distance map (16 x 16,
  mean and mean square), so light does not leak through walls (DDGI: Majercik et al. 2019). Hits are lit by the
  previous frame's grid, so light bounces once more per frame. The grid scrolls with the camera without copying:
  probes that stay keep their history, new ones start from their first frame. Surfaces read it with trilinear
  weights, visibility and a normal bias (`quench/Probes.hlsli`, samplers 10 and 11, bit 32) in place of
  `Environment::skyAmbient` and `groundAmbient`; beyond the grid they fade to them.
- **Probe updates in turns** (`RenderSettings::probeUpdatePeriod`, 1, 2, 4 or 8, default 1; 0.10.0): the grid
  traces 1 / n of its probes per frame, each probe once in n frames, for phones where the grid's 2048 probes cost
  about 3 ms at any render scale. A probe's turn is a sub-lattice of the grid (2 turns a 3D checkerboard, 8 every
  other probe along each axis), so every frame's probes are spread evenly; the probe ray target's rows are ordered by
  turn and a frame traces one band of them (a scissor). The irradiance and distance passes still run over every
  probe: the others keep their values. The hysteresis stays 0.97 per update, so light follows a change n times more
  slowly in frames. Validity is the irradiance alpha: a probe that comes into a scrolling grid and is not in this
  frame's turn gets alpha 0 and is left out of the shading (`Probes.hlsli` multiplies its weight by the alpha) until
  it is traced, instead of showing the light of the cell its slot held before or black; a grid that moves by more than
  a cell along an axis (a jump) traces every probe that frame, as a restart does. `RenderStats::probes` is the probes
  traced this frame (2048 / n). With n = 1 the frame is the one before, bit for bit (the demo's ray tracing and
  tunnel captures, mean difference 0.0000). Test `renderer_probe_grid_updates_in_turns` (native backend): a tile lit
  only by the grid (from a sunlit wall, in a black roof's shadow) holds its light in turns (within 5 %), gets
  halfway to the light of a doubled or halved sun in 18, 38, 68 and 156 frames with 1, 2, 4 and 8 turns (about n
  times 18), keeps its light in the frame a grid scroll brings new probes next to it in every turn (at least 0.2627
  against 0.2637 before; with the new probes taken as black 0.1259), and is lit at once after a jump (2048 probes
  traced).
- **Probe cascades** (`RenderSettings::probeCascades`, 1 to `kMaxProbeCascades` = 4, default 1; 2026-11-07): grids
  of the same 16 x 8 x 16 probes around the same point, each twice the spacing of the one before, so the probes are
  dense near `View::probeCenter` and the grid reaches far (three cascades from 1 m cover 64 x 32 x 64 m). Each
  cascade has its own rows of the atlases and of the probe ray target, scrolls on its own and is traced in the same
  turns. A surface takes the finest cascade whose 8 probes surround it, blended into the next one over the last cell
  before that cascade's edge (`Probes.hlsli`); a cascade whose probes around the point have no value (all left out,
  or not traced yet) passes the point on to the next one, and beyond the last one surfaces have the sky's ambient
  light, as beyond one grid. The sampling bias and the distance cap double with the spacing. One cascade runs its own path in
  the shading, without the cascade loop. `RenderStats::probes` counts every cascade's traced probes.
- **Probe relocation** (`probeRelocation`, default off; 2026-11-07): each traced probe moves inside its cell out of
  geometry (Majercik et al. 2021, "Scaling Probe-Based Real-Time Dynamic Global Illumination for Production"): a
  probe whose rays see more than a quarter back faces is inside and moves through the nearest one, a probe nearer than
  0.2 spacings to a surface moves away from it, otherwise it moves back toward the middle of its cell; a move that
  would take it further than 0.45 spacings from the middle is not made. A probe data pass (`rt_probe_data_fs`, a
  texel per probe, before the irradiance and distance passes) keeps each probe's offset and state; the irradiance
  atlas gets a copy of it in rows below its tiles, which the shading and the next frame's rays read (the rays start
  from the moved probe, the shading weighs the probe where it is). New probes and a restart start from the middle.
- **Probe classification** (`probeClassification`, default off; 2026-11-07): a probe whose rays have hit no front
  face within 1.5 spacings of its cell's middle (along every axis), or that is inside geometry, for 8 updates in a
  row is left out: it traces only every fourth ray (to notice a surface coming near), its irradiance alpha is 0 so it
  lights nothing, and its distance map is the cap; a quarter-traced probe that finds a surface traces fully again from
  the next update, its first update from its quarter of the rays. Probes in open air above a scene and inside walls
  are left out. `RenderStats::probes` still counts them as traced.
- **Tests** (native backend): `renderer_probe_cascades`: the tile strip of the turns test, 12 m from the grid's
  center, is outside a 1 m grid (0.0334, its direct light without the grid also 0.0334) and lit by the second, 2 m
  cascade (0.2826); near the center 2 cascades give 0.3552 against 0.3556 with one, 2 cascades in 4 turns hold within
  5 % (1024 probes traced), and classification changes it by 0.5 % (0.3568), 3 cascades with classification far away
  by 2.8 % (0.2747). The existing probe tests give the numbers they gave before these options. `renderer_probe_relocation`: two corridors 0.8 m wide between grey walls 0.9 m thick, their white
  floors in the walls' shadow and lit only by the other wall's sunlit face; the 1 m grid puts corridor A's probes in
  the walls and the ground, corridor B's in its middle. A's floor is 0.0000 without relocation and 0.2380 with it, B's
  0.2716; with classification on top A 0.2431 (the means of 8 frames after 250).
- **Per-pixel indirect light** (`IndirectLight::RayTraced`): one cosine-weighted ray per pixel, its hit lit as above
  with the probe grid (which runs too); contact detail the grid is too coarse for (sampler 8, bit 16).
- **Tests.** `renderer_indirect_light_bounces`: the ground next to a red wall turns red with the grid and the
  per-pixel rays (red over green 1.287 and 1.324, sky light 1.000; far from the wall 1.072, 1.023).
  `renderer_indirect_light_furnace`: under a uniform sky of radiance 1 with ambient 1, an open floor of albedo 0.8
  reads 0.823 with the sky's light, 0.806 with the grid and 0.823 with the per-pixel rays; a sphere on it 0.823,
  0.772 and 0.803 (it also sees the darker floor). So the grid has the scale of the ambient values. A scene lit by the
  grid under the daylight palette (`render::daylightFor`) is brighter than with its `skyAmbient`, because the drawn
  sky, with the glow around the sun, carries more light than that stylized value; the path tracer agrees with the
  grid there (the demo's ray tracing section, looked at).

Every hit's radiance is clipped at 64 (a rare bright hit would stay as a speck in the denoiser's history). The
passes' GPU costs at 1080p are in [Performance](performance.md#measurements).

### Local light shadow maps

Since 2026-11-07 (0.10.0; area lights since the same day, 0.11.0). Where `localLightShadows` cannot be ray
traced (SDL GPU, the default off Apple platforms, and the native backend without `gpu::Caps::rayTracing`),
`RenderSettings::localShadowMapSize` (0, the default: off; else 64..4096 texels per side) gives the same lights
shadow maps: the nearest `localLightShadows` (at most 4) point, spot and area lights with `castsShadows`, chosen as
for the rays. `ActiveEffects::localShadowMaps` is that count; `RenderStats::localShadowLights`, `localShadowMaps`,
`localShadowInstances` and `localShadowDraws` say what a frame drew.

- **Maps.** One layer of a depth texture array (the cascades' format, standard depth) per map: a spot light whose
  outer cone is under 60 degrees takes one perspective map over its cone, a wider spot and a point light six, the
  faces of a cube (+x, -x, +y, -y, +z, -z), each 3 texels wider than 90 degrees so that the filter does not read
  past a face's edge. Near plane: the light's `sourceRadius`, at least 1 % of its range; far plane: the range. The
  array is made for the frame's count of maps (at most 24) and kept while that count and the size hold: the demo's
  two lamps at 512 are 7 layers, 7 MB with `Depth32Float`. An area light takes a cube of maps in its own frame (x
  its tangent, y its normal, for a tube a direction across its axis): the point light's faces and up vectors in
  that frame, five for a one-sided rect or disk (none looks behind it); far plane the range plus the shape's radius,
  near plane 1 % of it (`QUENCH_AREA_SHADOW_NEAR`); `LocalLight::cone.w` is its first layer + 1. The tunnel's four
  nearest lamps at 512 are 21 layers, 21 MB.
- **Casters** (timed range `local_shadows`, after the cascades): every instance with `castsShadows` whose bounds meet
  a map's frustum, drawn with the cascades' shadow pipelines and depth bias. Cut-out casters keep their holes
  (`surface_shadow_fs`), translucent ones are dithered into the depth (no transmittance map for local lights),
  skinned instances are posed. Game passes of `PassStage::Shadow` draw into the cascades only.
- **Lookup.** `surface_local_shadows_fs` (`SurfaceDesc::localShadowFragmentShader`: `surface_fs` with
  `QUENCH_SURFACE_LOCAL_SHADOWS` 1) reads each mapped light's map once per pixel before the light loop
  (`quenchLocalShadows` in `Surface.hlsli`, `quenchLocalShadow` in `Shadows.hlsli`): the point moved along its normal
  by 1.5 texels at its depth, four bilinear comparisons two texels apart (`Gather`, 4 x 4 texels), into the light's
  channel as the traced shadows have (`LocalLight::cone.y`; `shape`: x the first layer + 1, y the spot map's
  tangent of half its angle or 0 for six maps, z the near plane). Fragment texture and sampler 13,
  `FrameUniforms::localShadows` (texels, their inverse, the normal offset, the cube faces' tangent). The renderer
  draws with that variant in frames with maps only, so other frames pay nothing for it (4 sections measured within
  0.5 % of before on the native backend, [Performance](performance.md#local-light-shadow-maps)); a surface without
  it leaves its local lights unshadowed in those frames. The standard surface and the demo's checker have it.
- **In screen space where an area light has maps.** Their soft filter is too costly per surface fragment (the
  tunnel's four lamps took about 7.6 ms more of a 1080p frame on the M4 Pro read that way, 1.4 ms in the screen
  pass), so in such frames every map is read once per 2 x 2 pixels (`ScreenSpace::resolveLocalShadows`,
  `src/render/shaders/localshadow.hlsl`): a depth prepass of its own (as GTAO's, timed range `prepass`), then
  `local_shadow_fs` at the top-left pixel of each block, at its depth's position with its normal from the depth,
  writes every mapped light's shadow into its channel, and `local_shadow_blur_fs` averages the 3 x 3 texels around
  each pixel's own at full resolution (weighted by how well their depth continues the pixel's and by distance) into
  an `Rgba8Unorm` target (timed range `local_filter`). Opaque surfaces read it as they read traced local shadows
  (sampler 9, `options.w` bit 4) with their usual variants; transparent ones are drawn with
  `surface_local_shadows_fs`, which reads the point and spot lights' maps and leaves area lights unshadowed. Without
  the screen-space pipelines the frame's local lights stay unshadowed.
- **Area lights' penumbra** (`quenchAreaShadow` in `Shadows.hlsli`): percentage-closer soft shadows (Fernando 2005)
  with the shape's own points as the kernel. A search of 8 `Gather` taps over the region where blockers of the
  shape's light could be (from P's own direction outward, quadratically, within about 37 degrees of it) finds the
  blockers' mean depth d (none: lit; all: shadowed); then 8 bilinear taps toward points of the shape (uniform over a
  rect's or disk's area, over the half of a tube that faces P) seen through a blocker at d say which of them P sees.
  Depths are measured from the shape's plane (a tube's axis), so a tap toward point p runs along q + p (z_R / d - 1),
  exact for one flat blocker parallel to the shape. Each point counts with its diffuse light at P (the cosines at
  both ends over the squared distance; the ratio of Heitz et al. 2018), so the far ends of the tunnel's 25 m floor
  strips add little to a wall's shadow. A tap's reference depth is where its direction meets P's plane (no acne on
  slanted receivers); the taps turn per pixel by interleaved gradient noise, per frame under TAA, and the blur
  evens that out.
- **Cost** on the M4 Pro at 1080p (SDL GPU's Metal driver, offscreen): the demo's ray tracing section's two lamps (a
  point light and a spot light, 7 maps) add 0.3 to 0.4 ms to the frame and about 0.1 ms of CPU time; the map size
  barely matters (256 to 1024). The tunnel's four nearest lamps (two floor strips, a ceiling square, a tube: 21 maps
  at 512), on the native backend with its rays switched off in a local build (offscreen, 1920 x 1080 output): 0.92 ms
  of GPU time more per frame at the section's 0.67 render scale (maps 0.23, prepass 0.02, screen pass 0.70 ms;
  `hdr` unchanged), 1.7 ms at the full size (maps 0.37, screen pass 1.38 ms)
  ([Performance](performance.md#area-light-shadow-maps)).
- **Test** `renderer_local_light_shadow_maps` (SDL GPU's Metal driver and Vulkan through MoltenVK; on the Mac's native
  backend the same scene with traced shadows): a point light and a spot light over small casters; behind them 0
  where the open ground has 411, 687 (under a plate, the -y face), 381 and 375; lit points beside them as without
  shadows; nine open ground points, among them the edge of two cube faces and the corner of three, 0 apart from the
  unshadowed image (no acne, no seams). One light with shadows takes the nearer spot light's one map; size 0 leaves
  them unshadowed and says so in `fallbacks()`. `renderer_area_light_shadow_maps` (the same backends; the native
  backend traces the same scene): a 1 m rect light 3 m over a box 1.5 m up (five maps) leaves 0.007 of its light
  under the box (traced 0.000) and 0.393 1.3 m beside it (traced 0.326; a flat blocker there lets about 40 % through),
  six open ground points within 0.025 of the unshadowed image (traced 0.021); a 1 m tube standing beside a post
  (six maps) leaves 0.051 behind the post (traced 0.089) and 1.000 beside it; a spot light next to a rect with maps
  (the screen pass) leaves 0.085 behind the post and 1.000 beside it.
- **Limits.** Point and spot lights' shadows are as hard as the 4 x 4 filter (`sourceRadius` does not soften them
  as it does the rays); a caster closer to the light than the near plane is lost; in `surface_shadow_fs` drawn into
  these maps `QuenchSurfaceInput::view` still points toward the sun. An area light's penumbra is an estimate: one
  mean blocker depth per pixel (blockers at different depths blend), the maps see the casters from the shape's
  center only (a long light's shadows of casters near its ends are approximate), and the blur softens contact edges
  by about a pixel; transparent surfaces get no area light shadows.

### Screen-space ambient occlusion

Since 2026-11-07 (0.10.0). `AmbientOcclusion::ScreenSpace`, or `AmbientOcclusion::RayTraced`
with `RenderSettings::screenSpaceFallbacks` (off by default) where the rays are not available (SDL GPU, the
default off Apple platforms, and the native backend without `gpu::Caps::rayTracing`): GTAO from the depth of the
opaque surfaces. It needs a sampleable depth and `Rg16Float` targets; else none, with a line in `fallbacks()`.
Where the device traces, `RayTraced` stays traced; `ScreenSpace` set by the game runs GTAO there too.
`RenderStats::screenOcclusion` says whether it ran, `screenPrepassDraws` the draws of its own prepass.

- **Depth.** Where something is traced, the ray tracing prepass's depth; else a depth-only prepass of its own (the
  opaque batches with the prepass pipelines, cut-out holes kept; timed range `prepass`).
- **GTAO** (`ss_gtao_fs` in `src/render/shaders/screenspace.hlsl`, timed range `gtao`): adapted from XeGTAO's main
  pass (MIT, Intel; GameTechDev/XeGTAO `a5b1686`; Jimenez et al. 2016, "Practical Real-Time Strategies for Accurate
  Indirect Occlusion"), at half resolution (the top-left pixel of every 2 x 2 block, as Ashfall's screen-space
  effects): `occlusionRays` slices per pixel (1..8, default 2), each with 4 steps to either side, out to
  `occlusionRadius` times XeGTAO's 1.457 and at most 20 % of the render height on the screen (no depth MIP chain:
  far samples would only miss the cache), XeGTAO's falloff and final power 2.2, normals from the depth, positions
  along camera rays (`ScreenSpaceUniforms`, camera-relative). The noise is a 4 x 4 tile that changes every frame
  under TAA, which averages it, and stays fixed otherwise. The term and its depth go into an `Rg16Float` target.
- **Blur and upsample** (`ss_gtao_blur_fs`, full resolution): the 4 x 4 term texels around the pixel (one period of
  the noise tile), each weighted by how well its depth continues the pixel's depth gradient (a reversed depth is
  linear over a plane on the screen; tolerance 2 % of the view depth), so other surfaces do not bleed in; then
  `occlusionStrength`. The result goes into alpha of the mask the surfaces read for traced occlusion (sampler 6,
  `FrameUniforms::options.w` bit 2): with ray-traced sun shadows the traced mask, its rgb kept by the pipeline's
  write mask, else a mask of its own. Surfaces built on `Surface.hlsli` take it without changes.
- **Cost** on the M4 Pro at 1080p (offscreen, [Performance](performance.md#screen-space-ambient-occlusion)): native
  backend `gtao` 0.41 ms in the demo's shadows section (its own prepass 0.03 ms), 0.17 ms in the ray tracing
  section, where it replaces the occlusion rays (`rays` 2.30 to 1.70 ms), 0.25 ms in the tunnel; SDL GPU's Metal
  driver frame p50 1.33 to 1.79 ms (shadows section) and 1.58 to 2.00 ms (ray tracing section).
- **Test** `renderer_screen_space_ambient_occlusion` (64 x 64, a box on the ground under ambient light only; SDL
  GPU's Metal driver, Vulkan through MoltenVK, the native backend): 0.940 of the light without it next to the box
  (the rays: 0.822), 1.000 on open ground and on the box's top; the fallback from `RayTraced` the same image and
  named in `fallbacks()`; on the native backend with traced sun shadows the shadow stays (0.965 of the frame
  without GTAO, open ground 1.000, next to the box 0.930) and no prepass of its own is drawn.
- **Limits.** Only what is on the screen occludes: nothing behind the camera, off the screen or hidden behind a
  nearer surface, and a thin object seen edge on occludes as if it were thick behind. Half resolution and the
  8 x 8 pixel footprint soften the contact (0.940 against the rays' 0.822 in the test's small image). No bent
  normals, no temporal filter of its own, no occlusion of translucent surfaces.

### Screen-space reflections

Since 2026-11-07 (0.10.0). `Reflections::ScreenSpace`, or `Reflections::RayTraced` with
`RenderSettings::screenSpaceFallbacks` (off by default; the same flag as GTAO's) where the rays are not available
(SDL GPU, the default off Apple platforms, and the native backend without `gpu::Caps::rayTracing`): reflections of
what the previous frame showed. They need a sampleable depth and `Rgba16Float` targets; else the sky's reflection,
with a line in `fallbacks()`. Where the device traces, `RayTraced` stays traced; `ScreenSpace` set by the game runs
SSR there too. `RenderStats::screenReflections` says whether it ran: not in a frame without a previous image (the
first one, after a cut or a resize, or after a frame without SSR), which keeps the sky's reflection.

- **G-buffer.** Where something is traced, the ray tracing prepass's depth and G-buffer; else a prepass of its own
  (depth with the G-buffer's normal, roughness, albedo and metallic: the opaque batches with the G-buffer pipelines,
  `surface_gbuffer_fs` where a surface has one; timed range `prepass`, its draws in `screenPrepassDraws`), shared
  with GTAO.
- **March** (`ss_reflect_fs` in `src/render/shaders/screenspace.hlsl`, timed range `ssr`): moved from Ashfall's
  `ssr_fs` (`shaders/scene.hlsl` at Ashfall `2d5db57`, the same author) and generalized. At half resolution (the
  top-left pixel of every 2 x 2 block), up to `reflectionRoughness`: one ray along a GGX visible-normal sample of
  the G-buffer's normal and roughness (as the traced reflections; Ashfall: the mirror direction), camera-relative,
  48 steps from 0.5 % of the view depth, each 1.1 times the last (about 4.8 view depths), stepped linearly in clip
  space. The hit is the first depth the ray passes behind by less than 3 % of that depth or two steps' depth, and
  above the tangent plane of the ray's origin (within a pixel of a grazing floor the ray seems behind the floor
  itself), refined by 5 bisections. Its confidence fades for rays toward the camera (what they would hit is not on
  the screen), hits on the back of a surface, near the screen's border, near the march's end and where the hit was
  outside the previous frame's image. Its color is the previous frame's HDR image (before TAA) at the hit's position
  in that frame (the previous view-projection, moved by the camera's movement), clipped at 64 as the traced
  reflections' radiance.
- **Resolve** (`ss_reflect_resolve_fs`, full resolution, `Rgba16Float`): the 4 x 4 term texels around the pixel,
  weighted by the depth gradient as GTAO's blur (tolerance 2 % of the view depth) and by a Gaussian of 0.8 + 6 x
  roughness pixels; the colors also by 1 / (1 + brightness) (Karis's average), so a single ray that finds a lamp does
  not stay a bright square. Surfaces built on `Surface.hlsli` take rgb in place of the sky's reflection as the traced
  reflections (sampler 7, `FrameUniforms::options.w` bit 64 beside bit 8), weighted by its alpha, the confidence: the
  sky's reflection where the screen has nothing.
- **History.** After the HDR pass a copy of the image (timed range `ssr history`), kept with the camera-relative
  view-projection and the camera's position; dropped when SSR is off.
- **Cost** on the M4 Pro at 1080p (offscreen, [Performance](performance.md#screen-space-reflections)): native backend
  `ssr` 0.12 ms in the materials section, 0.18 ms in the shadows section (their own prepass 0.09 and 0.08 ms, the
  copy 0.08 to 0.18 ms), 0.09 ms in the ray tracing section in place of the traced reflections (0.51 ms), 0.70 ms in
  the tunnel in place of the traced ones (3.65 ms; its 1286 x 724 render size); SDL GPU's Metal driver frame average
  1.66 to 1.91 ms (materials), 1.34 to 1.65 ms (shadows) and 3.34 to 5.23 ms in the tunnel (the march about 1.2 ms at
  half of 1080p).
- **Test** `renderer_screen_space_reflections` (64 x 64, a mirror floor before an emissive box; SDL GPU's Metal
  driver, Vulkan through MoltenVK, the native backend): the first frame has no previous image and shows the sky's
  reflection (66 105 198), the second the box's red (247 9 14, the traced reflections' 247 7 14), the open floor
  still the sky; the fallback from `RayTraced` the same and named in `fallbacks()`; on the native backend with traced
  sun shadows SSR reads the traced prepass's G-buffer and draws no prepass of its own.
- **Limits.** Only what the previous frame showed is reflected: nothing behind the camera, off the screen or
  hidden, so a mirror the camera looks straight at shows the sky, and an object moving fast is reflected a frame
  late. One ray per 2 x 2 pixels and a spatial resolve only: rough surfaces near bright lights keep a fine grain (the
  tunnel's lower walls in captures on both backends, finer under TAA than under MetalFX's temporal upscaling). Surfaces are taken to be 3 % of their depth thick, so a thin
  object can be hit through. Translucent surfaces keep the sky's reflection. Cost grows with the render size: about
  2 ms on the M4 Pro at 1080p in the tunnel on SDL GPU.

### Denoising

The traced signals (the sun's visibility and the occlusion, the local light shadows, the reflections, the indirect
light) are noisy at one or two rays per pixel. `RenderSettings::rayDenoiser`:

- `Temporal` (the default; needs the motion vectors, else `Spatial`): per signal a history at the render size,
  reprojected with the motion vectors and dropped where the previous depth disagrees by more than 5 %, clamped to the
  mean plus or minus 1.25 to 2.5 standard deviations of the new 3 x 3 neighbourhood, blended with a history length
  of up to 16 frames (shadows and occlusion), 24 (reflections, shorter on rough ones) or 32 (indirect light), then
  two a-trous passes (steps 1 and 2) weighted by the surface's plane (`TemporalFilter.cpp`, `raydenoise.hlsl`;
  after SVGF, Schied et al. 2017, without its variance estimate). Anti-lag: where the neighbourhood's mean is further
  from the history than its noise explains, the history is cut short, so a moved shadow is there in the next frame
  even where it is a few pixels thin and the clamp alone lets the old value through. A camera cut drops the history.
- The per-pixel indirect light (since 2026-11-07) also keeps each pixel's standard deviation over its history (an
  exponentially weighted variance with the history's weights, `rt_temporal_deviation_fs`) and clamps and cuts with
  no less than it, and takes a third a-trous pass (step 4). At one ray per pixel the 9 new samples around a pixel
  often agree by chance (all of them see the sky, or none does: a floor under a roof, the ground under a fence), and
  their spread alone collapsed then: the clamp and the anti-lag pulled the history to their mean, so the light never
  settled (grain all the time, also in a still scene) and its mean came out darker. The other signals keep the
  filter as it was: their own changes (a shadow edge that moves with the sun) widen a pixel's deviation too, and the
  floor let a moving sun shadow trail more (measured in the demo; [Performance](performance.md#the-denoiser-in-a-still-scene)).
- `Spatial`: the stage 9 blurs for the sun and the occlusion (3 x 3 and 5 x 5, depth-aware) and the a-trous passes
  alone for the rest; TAA's history then averages the frames.
- `Upscaling::TemporalDenoised` (below) skips both: MetalFX denoises the raw signals.

Test `renderer_temporal_ray_denoiser` (one ray against eight): temporal mean 0.02 (0.00 % of the pixels over 16),
spatial 0.50 (1.17 %); frame to frame 0.01 against 0.27. `renderer_ray_tracing_refit_and_compaction`: a moved
shadow about 7 pixels across reads 57 in the first frame after the move (settled 30, lit 579; 399 before the
anti-lag). `renderer_indirect_light_settles` (native backend): a floor under a black roof, lit only by the sky past
its edges, reads 0.1752 with the temporal denoiser after 64 frames and changes 0.3 % from frame to frame; the spatial
filter's mean over 64 frames is 0.1778. With the indirect light's filter as it was before 2026-11-07: 0.0244
against 0.0987, 9.8 % from frame to frame.

### Path tracer

`RenderSettings::pathTracer` (native Metal backend; else the real-time frame): a reference to compare the real-time
effects with, not for play. Every frame adds one path per pixel through a random point of the pixel to a running mean
(`Rgba32Float`) until `pathSamples` (1024); a change of the camera, the environment, the lights, an instance's
transform, a skinned pose or a material, or of `pathBounces` (4) starts it again. At each hit: the emissive light,
the sun (a random point of its disc, with a shadow ray), one local light chosen at random (with a shadow ray), then
the next direction from the standard surface's BRDF (GGX visible normals or a cosine lobe by the Fresnel weight),
Russian roulette after the second bounce; translucent surfaces are crossed with the probability of their
transparency. The mean replaces the HDR pass and TAA (no game passes before `PostHdr`); bloom and tone mapping
follow. MetalFX's temporal scalers have nothing to resolve then: they become the spatial one below render scale 1,
or bilinear. Hits on custom surfaces use the material's values, or their own function where they have ray hits
(the demo's checker ground has them). Test
`renderer_path_tracer_furnace`: a white sphere under a uniform sky of radiance 1 reads 0.992 in the middle, 0.962
half way out and 0.863 at the silhouette pixel (sky 0.999): the BRDF's Fresnel and single-scattering GGX lose a few
percent toward grazing views, as in the raster path; the mean restarts after a move and after a new pose, not after
the same pose set again.

### Upscaling

`RenderSettings::upscaling`, with `renderScale` (the native Metal backend has MetalFX: `gpu::Caps::spatialUpscaling`,
`temporalUpscaling`; `gpu::CommandBuffer::upscale`, timed range `upscale`):

- `Bilinear` (the default): tone mapping scales the smaller image up, as before.
- `Spatial`: below scale 1, MetalFX's spatial scaler takes the anti-aliased HDR image (TAA or none) to the target size
  before bloom and tone mapping.
- `Temporal`: MetalFX's temporal scaler takes the jittered HDR image, the depth and the motion vectors in place of
  TAA's resolve, also at scale 1 (anti-aliasing only). The jitter goes in as the image's shift in input pixels,
  `(-jx, +jy)` of the projection's offset, and the motion scale as `(-width, -height)`; of the four sign choices this
  one came out closest to the native frame in the test.
- `TemporalDenoised` (stage 11, `gpu::Caps::denoisedUpscaling`: MetalFX's temporal denoised scaler, from macOS 26
  and iOS 26 on GPUs that support it; the M4 Pro and the A20 Pro do): the HDR image with the traced signals
  unfiltered, its depth and motion, and the G-buffer turned into diffuse albedo (albedo times one minus metallic),
  specular albedo (the split-sum reflectance of the roughness and the view angle), the normal and the roughness
  (`rt_denoise_inputs_fs`), denoised and scaled up in one pass in place of TAA and the engine's denoiser; the
  exposure goes in as `UpscaleDesc::exposure`. It needs something traced and TAA's inputs; otherwise it falls back
  to `Temporal` with the engine's denoiser, or to bilinear. A frame without its inputs (the path tracer just turned
  off) is scaled by a blit and the scaler's history reset.
- Without the capability: bilinear, with the anti-aliasing the settings ask for. After MetalFX the game's `PostHdr`
  passes get the target-size image and no depth (`PassContext::depth` is empty).
- Test (`render_metal_native.renderer_upscaling`, against native TAA at full size): spatial from 0.5 mean 1.63 (2.03 % of the
  pixels over 16), temporal mean 1.15 (3.81 %). `renderer_denoised_upscaling` (soft shadows and occlusion at one
  ray and half size against eight rays at full size): the denoising scaler mean 1.08 (1.27 % over 16), frame to
  frame 0.11; the temporal scaler with the engine's temporal denoiser 1.45 (1.81 %), 0.01.

### Fallbacks

`Renderer::effects()` is what runs after the device check; `RenderSettings` is what the game asked for. The choice is
made from what the device supports (`gpu::Device` format queries and `gpu::Caps`), never from the platform, and a
change is logged once. `Renderer::fallbacks()` keeps those lines for the current settings, one per effect that
fell back, with its reason ("ray-traced shadows: the device has no ray tracing (gpu::Caps::rayTracing); shadow
maps"), so a game can show them (the demo's "Frame and fallbacks" panel); it is empty when every effect runs as set:

| Effect | Needs | Fallback |
|---|---|---|
| Shadow cascades | a sampleable depth texture array | no shadows (`shadowCascades = 0`) |
| Casters in front of a cascade | `gpu::Caps::depthClamp` | without it they are clipped (pancaking off) |
| Translucent shadows (transmittance) | a renderable, sampleable `Rgba16Float` array | dithered into the depth |
| TAA | a sampleable scene depth and an `Rg16Float` target | FXAA |
| HDR target | `Rgba16Float` | `Rg11b10Ufloat` |
| Ray-traced sun shadows | `gpu::Caps::rayTracing`, the ray query shaders, a sampleable depth | shadow cascades |
| Ray-traced ambient occlusion | the same | GTAO with `screenSpaceFallbacks`, else none |
| Screen-space ambient occlusion (GTAO) | a sampleable depth, `Rg16Float` targets | none |
| Ray-traced local light shadows (also area lights) | the same | shadow maps if `localShadowMapSize` is set and a depth array is sampleable (area lights' soft, read in a screen-space pass); else unshadowed |
| Ray-traced reflections | the same and the hit shaders | SSR with `screenSpaceFallbacks`, else the sky's reflection |
| Screen-space reflections (SSR) | a sampleable depth, `Rgba16Float` targets | the sky's reflection |
| Probe grid, per-pixel indirect light | the same and the hit shaders | `skyAmbient` and `groundAmbient` |
| Path tracer | the same and the hit shaders | the real-time frame |
| Temporal ray denoiser | a sampleable depth and an `Rg16Float` target (the motion vectors) | spatial |
| Spatial upscaling (MetalFX) | `gpu::Caps::spatialUpscaling`, render scale below 1 | bilinear in tone mapping |
| Temporal upscaling (MetalFX) | `gpu::Caps::temporalUpscaling` and TAA's inputs | bilinear, with TAA or FXAA |
| Denoising upscaling (MetalFX) | `gpu::Caps::denoisedUpscaling`, a traced effect, TAA's inputs | temporal upscaling with the engine's denoiser, then bilinear |
| Volumetric clouds | a sampled `Rgba8Unorm` 3D texture, a renderable `Rgba8Unorm` 2D array (its layers copied into the 3D noise), `Rgba16Float` targets | the 2D cloud layer |
| Sky cube | a renderable, sampleable `Rgba16Float` cube | the sky's reflection without clouds |
| Rain | renderable, sampleable `Rgba16Float` 2D textures and a sampleable depth format (the cascades'); not in the path tracer's frame | no rain |
| Snow | the same as the rain | no snow |
| Rain shafts | the rain or the snow, and clouds (they read the clouds' weather map) | none |

Shadow lookups use `Gather` on the depth texture and compare in the shader (1, 4 or 12 Poisson taps, each a bilinear
2 x 2 comparison) instead of a comparison sampler: MoltenVK reports no `mutableComparisonSamplers`, and SDL GPU's
comparison sampler then fails Vulkan validation (`VUID-VkDescriptorImageInfo-mutableComparisonSamplers-04450`). The
result is the same filtering on Metal and Vulkan.

### Game passes

`addPass(stage, name, fn)` adds a callback that runs every frame until `removePass`, in the order added within a
stage. It gets a `PassContext`: the command buffer, the open render pass (none in `PostHdr`), the frame uniforms
(already pushed to uniform slot 0 of both stages; read them through `quench/Frame.hlsli`), the color and depth
textures with their formats and sizes, and the cascade index in `Shadow`. Pipelines of a pass must use
`PassContext::colorFormat` and `depthFormat`; the demo's light discs cache one pipeline per format pair.

## Surface shaders

A game's material shading is a surface function in HLSL, lit by the engine:

```hlsl
#include "quench/Surface.hlsli"

#if defined(QUENCH_STAGE_FRAGMENT) // materials, textures and lighting exist in the fragment stage only
QuenchSurface checker(QuenchSurfaceInput i) {
    QuenchSurface s = quenchStandardSurface(i); // base color, textures, roughness, metallic, emissive
    s.albedo *= fmod(floor(i.world.x) + floor(i.world.z), 2.0) == 0.0 ? 1.0 : 0.5;
    return s;
}
#endif

#define QUENCH_SURFACE checker
#include "quench/SurfaceEntries.hlsli"
```

```cmake
quench_add_shaders(my_game NAME my_surface SOURCE shaders/checker.hlsl
    ENTRIES surface_vs:vertex surface_fs:fragment surface_basic_fs:fragment surface_local_shadows_fs:fragment)
```

```cpp
const render::SurfaceId checker = *renderer.addSurface({
    .vertexShader = quench::shaders::my_surface.find("surface_vs"),
    .fragmentShader = quench::shaders::my_surface.find("surface_fs"),
    .basicFragmentShader = quench::shaders::my_surface.find("surface_basic_fs"),
    .localShadowFragmentShader = quench::shaders::my_surface.find("surface_local_shadows_fs"), .name = "checker"});
scene.addMaterial({.surface = checker, .params = {...}});
```

- `QuenchSurfaceInput`: world position, normal (flipped on back faces), view direction, uv, color, the instance's
  `custom` vector, pixel position, front face, tangent (world space; w the bitangent sign, turned around by a
  mirroring transform). `quenchNormalFromMap(i, texel)` turns a normal map texel into a world normal. `QuenchSurface`:
  albedo, opacity, roughness, metallic, shading normal, emissive, occlusion, anisotropy and its direction (stage 12;
  a surface that fills `QuenchSurface` itself instead of starting from `quenchStandardSurface` sets `anisotropy` to
  0).
- The material is `quenchMaterial` (`MaterialUniforms`: base color, emissive, roughness, metallic, cutoff, the texture
  mask, `params[4]`); its textures are `quenchTexture0..3` with `quenchSampler0..3`; `quenchHasTexture(n)` tells
  which are set (unset ones sample white).
- With rain (since 2026-11-07, [Rain](#rain)) the entries `surface_fs`, `surface_basic_fs`,
  `surface_local_shadows_fs` and `surface_gbuffer_fs` call `quenchApplyRain(s, i)` after the surface function: the
  material's `weather` (`MaterialDesc::wetness`, `puddles`, `water`) makes the surface wet where the rain reaches it.
  A file that defines `QUENCH_SURFACE_NO_RAIN` before including `SurfaceEntries.hlsli` keeps its surface dry (a
  surface under glass, a screen, an effect); one that wants other rules calls `quenchApplyRain` itself in its
  function and defines it. With snow (since 2026-11-08, [Snow](#snow)) they then call `quenchApplySnow(s, i)`: the
  material's snow (`MaterialDesc::snow`) lays the weather's snow cover on faces looking up where the snow reaches;
  `QUENCH_SURFACE_NO_SNOW` keeps it bare the same way.
- `QUENCH_SURFACE_VERTEX` names `void f(inout QuenchVertexIn v, InstanceData instance)`, run before the transform
  (wind, procedural motion). Build `surface_shadow_vs:vertex` too and pass it as `SurfaceDesc::shadowVertexShader`, so
  the shadows move with it. The demo's checker ground (`examples/demo/shaders/checker.hlsl`) and the test shader
  `tests/render/shaders/custom.hlsl` are examples; the demo's plants (`examples/demo/shaders/plants.hlsl`) sway in
  the weather's wind with `quenchWindSway` from `quench/Wind.hlsli` ([Wind and lightning](#wind-and-lightning)).
- A surface used by cut-out or translucent materials also builds `surface_shadow_fs:fragment` and
  `surface_transmittance_fs:fragment` and passes them as `SurfaceDesc::shadowFragmentShader` and
  `transmittanceShader`, so its shadows have its own holes and opacity. Without them those shadows use the standard
  surface's opacity (base color alpha, texture 0 alpha, vertex alpha). In these entries `QuenchSurfaceInput::view`
  points toward the sun and `pixel` is the shadow map texel; `surface_vs` projects with the cascade there
  (`DrawUniforms::info.z`).
- `surface_basic_fs:fragment`, passed as `SurfaceDesc::basicFragmentShader` (since 2026-11-06, after stage 12), is
  `surface_fs` compiled without the area lights, the anisotropic lobe and the area light shapes (`QUENCH_SURFACE_FULL`
  is 0 in it). The renderer draws with it in frames without area lights, for materials without anisotropy that are
  not an area light's shape; elsewhere with `surface_fs`. The standard surface and the demo's checker have it.
  Without it a surface draws everything with `surface_fs` and pays for those paths in every pixel (5 to 8% of the M4
  Pro's unpaced frame rate at 1080p in the demo's sections; with it within 0.6% of before stage 12;
  [Performance](performance.md#the-tunnel)).
- `surface_local_shadows_fs:fragment`, passed as `SurfaceDesc::localShadowFragmentShader` (since 2026-11-07, after
  0.9.0), is `surface_fs` with the local lights' shadow maps (`QUENCH_SURFACE_LOCAL_SHADOWS` is 1 in it; "Local light
  shadow maps" above). The renderer draws every material of the surface with it in frames with such maps (where an
  area light has maps, its transparent materials only: the opaque ones read the screen pass), so `surface_fs` and
  `surface_basic_fs` carry no code for them. Without it the surface's local lights stay unshadowed
  in those frames (but for its opaque materials in frames with the screen pass).
- `addSurface` creates the opaque pipeline at once (and the basic one), so a shader that does not fit the interface
  fails there with an error, not at the first draw.

### Ray hits of surfaces

Since 2026-11-07 (0.11.0) a surface's function can also shade its ray hits: the traced reflections, the indirect
light, the probe grid's rays and the path tracer, where the hits otherwise take the standard surface from the
material's values ("Ray-traced lighting" above). The game lists its surface files in one set; the engine's tracing
entries are compiled again with them at build time, and the hit calls the function of the instance's surface through
a switch over the list:

```cmake
quench_add_ray_hits(my_game NAME my_hits SURFACES shaders/checker.hlsl shaders/foliage.hlsl)
```

```cpp
const render::SurfaceId checker = *renderer.addSurface({
    .vertexShader = quench::shaders::my_surface.find("surface_vs"),
    .fragmentShader = quench::shaders::my_surface.find("surface_fs"),
    .rayHitShaders = &quench::shaders::my_hits, .rayHit = 1, // its number in SURFACES, from 1
    .name = "checker"});
```

- `quench_add_ray_hits()` (`cmake/QuenchShaders.cmake`) writes a source that includes `src/render/shaders/raylight.hlsl`
  with `QUENCH_RT_SURFACES` 1 and a generated `QuenchRaySurfaces.hlsli` with every listed file in a namespace of its
  own and `quenchRaySurface(n, input)`, and compiles the 8 tracing entries (with and without normal maps) through
  `quench_add_shaders()` as ray query entries (DXC; no entries without it, then the hits take the standard surface).
  Every listed file is compiled into the same source: functions and globals may share names across files, macros may
  not (each file's `QUENCH_SURFACE` and `QUENCH_SURFACE_VERTEX` are undefined after it).
- In the hits `QUENCH_SURFACE_HIT` is 1 (0 elsewhere). `Surface.hlsli` then declares no resources and no lighting
  (`quenchShade`, `quenchDirectLight`, `quenchAmbient`), `SurfaceEntries.hlsli` no entries, and the trace shader
  provides `quenchMaterial` (the material's base color, emissive, roughness, metallic, cutoff, texture mask, params
  and anisotropy, filled per hit) and `quenchTexture0..3` with `quenchSampler0..3`: the textures' 256 x 256 copies in
  the hit arrays, every `Sample`, `SampleLevel`, `SampleBias` and `SampleGrad` reading their level 0 (white where a
  texture is not in the arrays; `quenchHasTexture` is false then). `ddx`, `ddy` and `fwidth` are 0 (a hit has no
  pixel footprint: the checker's filter becomes the plain checker). `QuenchSurfaceInput` is built as the raster
  builds it: the hit's position, the interpolated normal of the side the ray hit, the tangent, `view` toward the ray's
  origin, uv, the vertex color times the instance's color, the instance's `custom` vector, `pixel` 0 and `front`. A
  surface that declares resources of its own keeps them out with `#if !QUENCH_SURFACE_HIT`.
- The function's albedo, opacity, roughness, metallic, normal (dropped when it faces away from the ray's origin),
  emissive and anisotropy replace the standard surface's; `occlusion` is not used. Shadow and occlusion rays still
  read the standard surface's opacity (cut-out and translucent casters), and `QUENCH_SURFACE_VERTEX` deformations are
  not in the acceleration structures.
- A renderer takes one set (`addSurface` refuses a second one and a `rayHit` without `rayHitShaders`). The set's
  pipelines (6 for the tracing passes, 2 for the path tracer) are made in the first frame that traces an instance of
  such a surface; where they fail (logged once) the engine's are used. The other frames use the engine's own
  pipelines; `RenderStats::rayHitSurfaces` counts the instances whose hits run their function.
- The demo's checker grounds have their hits (`examples/demo/CMakeLists.txt`, `demo_hits`): the ray tracing
  section's mirror and gold spheres show the ground's cells instead of its mean color (`demo_raytracing_mirror_native`,
  [Demo](demo.md#checks)). Test
  `renderer_ray_hits_run_surface_functions`: the test surface `flat` (no lighting, its emissive from `params[0]`,
  green; the material's base color red) on two boxes over a mirror, the left one with its hits in the set: the
  raster shows both green (15 243 15), the mirror the left one green (16 243 16) and the right one red from the
  material's values (200 20 20), the path tracer the same (15 243 15 and 208 17 17), on the native Metal backend.

### Shader library and shared uniforms

`shaders/quench/` is included by engine and game shaders: `Common.hlsli` (constants, hashes, noise), `Brdf.hlsli`,
`Shadows.hlsli`, `Sky.hlsli`, `Fog.hlsli`, `Tonemap.hlsli`, `Fullscreen.hlsli`, `Frame.hlsli` (the frame uniforms),
`Clouds.hlsli` (the cloud shadow, the cloudy sky and the sky cube's reflection), `Rain.hlsli` (where the rain
reaches, the wetness grid, the puddles' relief and the rain's rings), `Cube.hlsli` (a cube face's texel
directions), `WeatherField.h` (the weather field, also compiled as C++ by `quench_weather`),
`Bindings.hlsli` (the `QUENCH_UNIFORM` / `QUENCH_TEXTURE` / `QUENCH_SAMPLER` / `QUENCH_STORAGE` register macros for
SDL GPU's spaces), `Surface.hlsli` and `SurfaceEntries.hlsli`. `SceneUniforms.h` defines the uniform structs once for
C++ (`quench::shader::FrameUniforms`, ...) and HLSL through `Shared.h`: only 16-byte members (`vec4`, `mat4`), so
the C++, HLSL, std140 and MSL layouts agree (`pushUniforms` checks at compile time that a block is a multiple of 16
bytes).

## Settings

`RenderSettings`: render scale (0.25..1) and upscaling (bilinear, MetalFX spatial, temporal or temporal denoised),
sun shadows (shadow maps or ray traced), cascades (0..4), shadow map size, shadow distance, shadow filter (1, 4 or
12 taps), translucent shadows (transmittance map or dithered), shadow rays and softness, ambient occlusion (none, ray
traced or screen space) with its rays, radius and strength, the screen-space fallbacks of the traced effects
(`screenSpaceFallbacks`), local light shadows (0..4) and the size of their shadow maps where
they are not traced (`localShadowMapSize`, 0: none), reflections (sky, ray traced or screen space) and
their roughness limit, indirect light (sky, probe grid or ray traced), the probe spacing, the probe updates
(every probe per frame, 1/2, 1/4 or 1/8), the probe cascades (1..4), probe relocation and classification, the ray
denoiser
(spatial or temporal), the path tracer with its bounces and samples, anti-aliasing (none, FXAA, TAA) and sharpening,
bloom (strength, threshold), sky on or off and the clear color, the clouds (off, 2D layer, volumetric) with their
resolution, accumulation, steps and shadows and the sky cube's size and faces per frame, the rain (off, low, medium,
high) with its shafts and rainbow, the snow (off, low, medium, high), the lightning (off, on), tone curve (ACES, AgX),
vignette, dither.
`setSettings` can be called every frame; the targets are recreated only when a size, the shadow map size or the set
of active effects changes.

## Statistics

`Renderer::stats()`: instances and draws in the HDR pass, culled instances, instances and draws in the shadow
cascades and in their transmittance maps, moved instances (motion vectors), local lights used, the 3D resolution,
pipelines created, and (stage 8) skinned instances and vertices posed this frame and the skinning pass's GPU time
(`skinningGpuMs`, -1 where `Caps::gpuTiming` is false or before the first measurement), and (stage 9) the top
level's instances and the bottom levels built and refitted this frame (`rayInstances`, `rayBuilds`, `rayRefits`),
and (stage 11) the bottom levels compacted this frame (`rayCompactions`), whether the top level was refitted
(`rayTopRefit`), the GPU memory of the acceleration structures, tables, hit texture arrays and probe atlases
(`rayMemoryBytes`), the material textures in the hit arrays (`hitTextures`), the probes traced (`probes`) and the
path tracer's samples (`pathSamples`), and (stage 12) the area lights among the frame's lights (`areaLights`) and
the lights that took another light's shadow channel (`localShadowChanges`), and (2026-11-07, 0.10.0) the
lights with shadow maps, their maps, the instances and draws in them (`localShadowLights`, `localShadowMaps`,
`localShadowInstances`, `localShadowDraws`), whether GTAO ran and the draws of its prepass (`screenOcclusion`,
`screenPrepassDraws`), whether SSR ran (`screenReflections`), and (stage 13) the clouds drawn, the march's size, how
overcast it is around the camera and the sky cube's faces drawn (`clouds`, `cloudWidth`, `cloudHeight`,
`cloudCover`, `skyCubeFaces`), and the rain drawn, the precipitation at the camera, the streaks, splashes and
objects in the rain map, whether the shafts and the rainbow were drawn and the cover over the camera (`rain`,
`rainAtCamera`, `rainStreaks`, `rainSplashes`, `rainOccluders`, `rainShafts`, `rainbow`, `rainCover`), and the
snow drawn, the snowfall and the snow's share of the precipitation at the camera and the flakes (`snow`,
`snowAtCamera`, `snowShareAtCamera`, `snowFlakes`), and the lightning's light at the camera and the flashes lit
(`lightning`, `lightningFlashes`).
The demo shows them and its scenarios check them (`draws`, `shadow_draws`, `transmittance_draws`, `lights`,
`area_lights`, `local_shadow_changes`, `local_map_lights`, `local_maps`, `local_map_draws`, `moved`,
`render_w`, `skinned_instances`, `skinned_vertices`, `ray_instances`, `ray_builds`, `ray_refits`,
`ray_compactions`, `ray_top_refit`, `ray_memory_mb`, `hit_textures`, `probes`, `path_samples`, `screen_occlusion`,
`screen_prepass_draws`, `screen_reflections`).

GPU time per pass (stage 9, where `gpu::Caps::gpuPassTiming`: the native Metal backend): `Renderer::render` wraps its
passes in timed ranges of the command buffer it records into, `upload` (the instance and light buffers), `accel`
(with `blas`, the bottom levels' builds, refits and compactions, and `tlas`, the top level, inside it),
`prepass` (with the G-buffer, the motion vectors for the temporal denoiser and the denoising upscaler's inputs) and
`raytrace` (ray tracing; `rays`, `denoise`, `probes`, `reflections` and `indirect` inside it), `path` (the path
tracer), `gtao` (screen-space ambient occlusion, after its own `prepass` where nothing is traced), `ssr` (screen-space
reflections: the march and the resolve), `cloud_noise` (once), `weather`, `clouds` and `sky_cube` (the clouds),
`rain_map` (the rain occlusion map and the weather maps' rain regions), `rain_shafts` (after `hdr`), `rain` (the
streaks and splashes, after `taa` or `upscale`) and `snow` (the flakes, after `rain`),
`shadows` (the
cascades and their
transmittance maps), `local_shadows` (the local lights' shadow maps), `local_filter` (their screen pass where an area
light has maps, after its own `prepass`), `hdr` (opaque, sky, transparent and the game's passes in them), `taa` (motion vectors and the
resolve), `upscale` (MetalFX), `bloom` and `tonemap` (with FXAA and the overlay passes), `ssr history` (after
`hdr`). A range whose effect is off
records nothing; the game's `PostHdr` passes are in no range of the renderer. The app reads them per frame
(`app::FrameStats::gpuPasses`, inside its `game` range); the skinning pass has its own command buffer and stays in
`skinningGpuMs`. [Performance](performance.md#gpu-time-per-pass) has the numbers.

## Skinned meshes

Stage 8 ([Animation](animation.md#gpu-skinning)). A mesh with `MeshDesc::skin` (one `SkinVertex` per vertex: four
joint indices into the mesh's joints and 16-bit weights) and `jointCount` is skinned; its vertices are in the bind
pose. `Scene::setJointPalette(instance, palette, teleport)` gives an instance one matrix per joint (joint model
matrix times inverse bind matrix, as `animation::AnimationSystem::skinPalette` computes; `SceneAssets` does the same
for cooked models). Before the first palette an instance shows the bind pose.

Every frame the renderer runs one compute pass (`skin_cs` in `src/render/shaders/skin.hlsl`, `skin_morph_cs` for
blend shapes, 64 vertices per group,
linear blend or dual quaternion skinning of positions, normals and tangents) over the skinned instances that are drawn in view or in a shadow
cascade, into each instance's range of a shared vertex buffer; the shadow, main and velocity passes then draw those
vertices like a rigid mesh, with the surface shaders unchanged (also the cut-out and translucent shadow casters,
which draw them with their surface's `surface_vs`). Two output buffers alternate, so the velocity pass
(`velocity_skin_vs`) has last frame's skinned positions and TAA gets motion vectors of the deformation; a palette
set with `teleport` has none. Culling uses bounds from the palette: per joint the sphere around the bind-pose
vertices it moves, transformed by the joint's matrix. The skinning runs in its own command buffer, so its fence gives
`skinningGpuMs` where the backend measures GPU time (Metal). A mesh with a bad skin (a joint index out of range, no
joints, a skin entry count other than the vertex count) is refused with an error by `Scene::addMesh`. The compute
pass needs `quench::gpu`'s compute support, which every SDL GPU backend has; there is no CPU skinning fallback.

An unchanged pose is not skinned again (since 2026-11-07): `setJointPalette` with the matrices the instance already
has is ignored (no motion either), and an instance whose palette did not change and that was skinned in the frame
before gets its vertices by a copy from the frame before's buffer, in a copy pass after the compute pass
(`RenderStats::skinnedReused`). Animation LOD's held poses are such palettes
([Animation](animation.md#animation-lod)); with a crowd of 260 held at every 4th frame the skinning pass took 0.46
instead of 1.75 ms on the M4 Pro. `render.renderer_reuses_unchanged_skins` checks that the image from copied
vertices is the one from skinned vertices, also when an instance's range in the buffer moves.

Dual quaternion skinning (since 2026-11-07) is chosen per instance: `InstanceDesc::skinning` (`Skinning::Linear`,
the default, or `Skinning::DualQuaternion`) or `Scene::setSkinning(instance, skinning)`; `SceneAssets::setSkinning`
sets it per skin of a cooked model. The renderer writes such an instance's palette as dual quaternions (three float4
per joint: the rotation, the dual part and the uniform scale, converted on the CPU), and `SkinUniforms::counts.w` is
1 for them. A twisted or bent limb keeps its volume (no "candy wrapper"); a joint's non-uniform scale or shear is
lost. Its culling bounds grow by the largest joint sphere, since a vertex can move out of the joints' hull on an arc
([Animation](animation.md#gpu-skinning)).

### Blend shapes

Blend shapes (morph targets, since 2026-11-07) are a mesh's: `MeshDesc::morphs` (`MorphVertex`: position, normal
and tangent offsets, one per vertex and target, the first target's first) and `morphTargetCount` (at most 256),
checked by `Scene::addMesh`. An instance's weights come from `Scene::setMorphWeights(instance, weights)` (one per
target, fewer: the rest 0; all 0 until set); `SceneAssets::setMorphWeights` sets a cooked model's by its target
names ([Assets](assets.md#quench_cook)). They are applied in the skinning pass, before the joints: a vertex moves by
the sum of the weighted position offsets, its normal is the normalized sum of its normal and the weighted normal
offsets, and its tangent the sum of its tangent and the weighted tangent offsets, made perpendicular to the new
normal and normalized (w kept); then the vertex is skinned, or written as it is for a mesh without joints, which
goes through the same pass. Any weight is allowed (negative, above 1). Only the targets whose weight is not 0 are
read: they are listed per instance as (target, weight) rows in front of its palette rows
(`SkinUniforms::morph`: their count, the first row, 1 when the mesh has joints), so with every weight 0 the
vertices are the mesh's own, bit for bit. The blend shapes have an entry of their own, `skin_morph_cs`, for
instances with weights and meshes without joints; skinned instances without weights keep `skin_cs`, which with
the blend shapes inside it was 10% slower for them ([Performance](performance.md#blend-shapes)). The culling bounds grow by the sum of each weight's magnitude times its
target's largest position offset (times the largest joint scale for skinned meshes). A change of weights moves the
instance like a changed palette (TAA motion vectors), unchanged weights are copied from the frame before, and ray
tracing builds and refits the instance's bottom level from the deformed vertices.

Checked by `render.renderer_blend_shapes` (a rigid box through a normal map: no weights and weights back at 0
bit for bit the image of the mesh without blend shapes; three sets of weights, also negative and above 1, the image
of the mesh morphed on the CPU by the formula above: 0 pixels more than 2 levels apart, while 892 to 1138 of the
4096 pixels differ from the unmorphed box; a shape that moves the box into view counts for culling),
`renderer_blend_shapes_before_skinning` (linear and dual quaternion: 0 pixels apart from the mesh morphed on the
CPU and then skinned, 169 differ from the unmorphed bars), `renderer_blend_shape_motion_vectors` and
`renderer_ray_traced_blend_shape_shadows` (the shadow follows the weight; native Metal backend), on Metal, Vulkan
through MoltenVK and the native Metal backend, and `scene_assets_blend_shapes`.

## Limitations

- Ray tracing: the shadow and occlusion rays start from a normal made from the depth, not the shading normal, offset
  by 0.2 % of the view distance. Rays read the standard surface's opacity (base color, texture 0 and vertex alpha)
  and hits the standard surface's shading from the material's values and textures (256 x 256 copies, at most 64;
  normal maps without a strength, as in the raster), or a surface's own function where its hits are in a
  `quench_add_ray_hits()` set (textures at level 0, no derivatives), never `surface_shadow_fs`; `QUENCH_SURFACE_VERTEX`
  deformations are not in the acceleration structures. Skinned casters further than `shadowDistance` are not in
  them. Hits are lit by local lights without shadow rays. Reflections are one bounce (a mirror in a mirror shows the
  second one lit, not reflecting); rough surfaces above `reflectionRoughness` keep the sky's reflection (SSR has the same cut-off). The probe grid's cascades are around one point (the finest one does not follow
  what is near the camera when `View::probeCenter` is elsewhere), each of the same counts; it is a few frames
  behind changes of light (n times more with `probeUpdatePeriod` n: halfway in about 18 n frames, within 10 % in
  about 70 n). Without `probeRelocation` its probes stay in walls (a probe inside a wall is dark and its
  neighbours' visibility hides it); relocation is a heuristic from 64 rays a frame (a thin wall between rays can be
  missed, and the offset moves a little from frame to frame as the rays turn), and a probe that cannot get out
  within 0.45 spacings stays. Classification leaves out a probe after 8 updates without a surface near it, so a
  surface that appears in open air gets the grid's light only after the left-out probes' quarter of rays find it.
  Turns are a fixed round-robin, not chosen by priority (a probe near the camera or in changing
  light is not traced sooner), and the hysteresis does not shorten with n. The temporal denoiser has no variance
  guidance in its blur and no disocclusion boost: newly visible areas are noisy for a few frames. The per-pixel
  indirect light's deviation floor makes it follow a sudden change of light (the sun jumping by 60 degrees) a little
  more slowly in the first 8 frames than before (its 99th percentile error 12 levels at frame 8 against 9; about
  equal at 16 frames, lower from 32 on), and a light change smaller than one ray's noise is followed over the
  history's 32 frames; its third a-trous pass blurs the indirect light over about 15 pixels on one plane. Without ray tracing local lights are unshadowed unless
  `localShadowMapSize` is set; the point and spot lights' shadow maps are hard (a 4 x 4 filter, not softened by
  `sourceRadius`), the area lights' soft by an estimate (one mean blocker depth per pixel) and missing on transparent
  surfaces, and all of them dither translucent casters. The path tracer has no refraction, and its translucency is
  a coin flip.
- Area lights: a tube's LTC has no end caps (-4.7 % against the integral for a 1.2 m tube of 5 cm); the traced
  shadow is the light's mean visibility, not weighted over the shape; anisotropic surfaces take the LTC with a bent
  normal and the mean roughness; rays do not see the shapes' emission in the real-time frame; at most 8 area lights
  per frame. The path tracer's light samples use the Karis G term and its BRDF samples the exact Smith G1 (-2.4 %
  on the disk test).
- MetalFX's denoising scaler was measured and compared with references in tests and in demo captures on the Mac;
  in motion it has not been looked at by a person yet.
- MetalFX's temporal scaler cost about 2.1 ms of GPU time at a 1080p output in the demo (vsync, GPU clocks not
  fixed), so a half-size frame with it was not much cheaper than a native one with TAA
  ([Performance](performance.md#measurements)).

- Translucent shadows: a caster is one layer (its faces toward the sun; double-sided materials both), with no
  thickness, absorption along the path, refraction or caustics. A receiver between two translucent casters that share
  a map texel takes both tints; within 4 map texels behind a caster it takes only part of the tint (none within 2).
  The map has half the shadow map's resolution, so these shadows are softer than solid ones.
- Cut-out and translucent casters run their surface function in the cascades' passes (cost per shadow texel); a
  custom surface without `surface_shadow_fs` / `surface_transmittance_fs` is cut by the standard opacity instead of
  its own.
- The motion vectors use the instance transforms and skinning only: a `QUENCH_SURFACE_VERTEX` deformation does not
  reach TAA (moving vertices smear a little under TAA). A moving cut-out object writes its motion only where the
  frame's depth is its own (within 0.1% of the distance), so its holes keep the motion of what is behind them; a
  surface less than 0.1% of the distance behind a hole takes the object's motion. A moving alpha-blended object's
  motion covers what is seen through it.
- Skinning: four joints per vertex, linear blend or dual quaternions (without non-uniform scale or shear); blend
  shapes with dense offsets (every target read in full for every vertex, no sparse or compressed targets, no
  per-target bounds); every drawn skinned
  instance whose palette changed is skinned every frame (an unchanged one is copied from the frame before; no
  skinning LOD); the output buffers grow with the skinned vertices drawn (two copies).
- Clouds ([Weather](weather.md#limitations)): the traced effects see an overcast amount, not single clouds; the
  cloud shadow map covers 24 km on the plane y = 0; looked at in motion by the user in the demo on the Mac and the
  iPhone 17 (2026-11-08: fine), their cost on a phone not measured.
- No MSAA, no image-based lighting from a cubemap (the sky model is the environment; with clouds the sky cube is the
  sky reflection's, not the ambient light's), shadows of local and area
  lights only by rays or shadow maps (at most 4), at most 16 local lights per frame (8 of them area lights) and no clustered or
  tiled light lists.
- One render per scene and frame (several views of one scene need a scene per view).
- Not in this stage, listed in the roadmap: Ashfall's other screen-space effects (bounce light, sun shafts) as
  fallbacks for devices without ray tracing (GTAO and SSR are there since 2026-11-07), particles, terrain, decals, quality presets. GPU timing per pass exists only on the
  native Metal backend (SDL GPU has no timestamps inside a command buffer).
