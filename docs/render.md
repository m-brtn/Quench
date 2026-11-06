# Render module

`quench_render` (stage 5 of the [Roadmap](roadmap.md)): a retained scene and the standard frame drawn from it. Public
headers in `include/quench/render/`, sources in `src/render/`, the shader library in `shaders/quench/`. Verified on
the Mac with Metal and with Vulkan through MoltenVK, and in the iOS Simulator; not yet run on an iPhone. The engine
demo (`examples/demo`, [Engine demo](demo.md)) shows every part of it.

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
  point and spot lights light surfaces without shadows; per frame the 16 nearest to the camera are used. A light with
  zero color or intensity is off.
- **Environment**: sky colors, atmosphere model or gradient, sun disc, ambient, height fog, exposure.
  `render::daylightFor(towardSun)` gives a sun color and an environment for any sun elevation (night, twilight,
  sunrise, day: TrailForge's time-of-day palette).
- **Units**: meters, linear HDR. A directional light's `color * intensity` is the irradiance on a surface facing it;
  a point or spot light's is the irradiance at 1 m (inverse square with a smooth window to zero at `range`).

The scene is retained so that later backends can build from it (ray tracing: acceleration structures from the meshes
and instances, stage 9). One `render()` per scene and frame: motion vectors are measured from the scene's previous
render, and switching cameras or scenes needs `View::cut`.

## The frame

`Renderer::render` records, in this order:

0. **Ray tracing**, where ray-traced shadows or ambient occlusion run (below): the acceleration structures of the
   shadow casters, a depth prepass of the opaque surfaces, a full-screen pass that traces from each pixel toward the
   sun and over its hemisphere, and a depth-aware blur. The cascades of step 1 are then not drawn.
1. **Shadow cascades** of the sun (standard depth, `Depth32Float` texture array): the view is split between linear and
   logarithmic distances up to `shadowDistance`, each cascade snapped to its texels; casters are culled per cascade.
   Solid casters write depth with their position only; cut-out ones run their surface and discard where it is cut
   out. Game `Shadow` passes run in each cascade after the scene's casters. A cascade with translucent casters then
   gets a second pass into its transmittance map (below).
2. **HDR pass** (`Rgba16Float`, reversed depth: near 1, far 0, infinite far plane): opaque surfaces sorted by
   pipeline and material, then game `Opaque` passes, then the sky (atmosphere or gradient, sun disc, fog toward the
   horizon), then alpha-blended and additive surfaces back to front, then game `Transparent` passes.
3. **Motion vectors and TAA**: camera motion from the depth, moved instances drawn again into an `Rg16Float` target;
   TAA with a Halton (2, 3) jitter over 8 frames, the history clipped to the variance box of the 3 x 3 neighbourhood
   (YCoCg), then sharpening. Render scale below 1 renders the 3D part smaller; tone mapping scales it to the target,
   or MetalFX does (below), whose temporal upscaler takes the place of TAA's resolve.
4. Game `PostHdr` passes on the anti-aliased HDR image.
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

### Ray-traced shadows and ambient occlusion

`RenderSettings::sunShadows = SunShadows::RayTraced` and `ambientOcclusion = AmbientOcclusion::RayTraced` run where
`gpu::Caps::rayTracing` holds and the build has the ray query shaders (`cmake/QuenchDxc.cmake`): today the native
Metal backend on a GPU with ray tracing, run on the Mac's M4 Pro only (the iPhone not yet). Elsewhere the frame
takes shadow maps and no occlusion, with one log line that says why (`Renderer::effects()` tells which runs). The
default stays shadow maps until the iPhone measurement ([Roadmap](roadmap.md#stage-9-native-metal-backend-and-ray-tracing)).

- **Acceleration structures** (`src/render/RayTracing.cpp`, timed range `accel`): every instance that casts shadows is
  in the top level, which is rebuilt every frame (instance mask 1: opaque and cut-out, 2: translucent). One bottom
  level per mesh, built when a caster first uses it and kept with the mesh. A skinned instance within
  `shadowDistance` of the camera has a bottom level of its own over the skinning pass's output, refitted every frame
  and built again after 60 refits (the counts are staggered, so a crowd made in one frame does not rebuild in one
  frame). Cut-out and translucent instances are non-opaque: a table per instance (offsets into a copy of their
  meshes' positions, uvs and colors; the material's color, cutoff and texture 0, at most 8 textures) lets the shader
  read the texel at a candidate hit. Bottom levels are not compacted.
- **Rays** (`prepass`, then `raytrace` with `rays` and `denoise` in it): the opaque surfaces' depth at the render size
  (cut-out ones discard where they are cut out), then `rt_shadow_fs` (`src/render/shaders/raytrace.hlsl`): per pixel
  the position from the depth and a normal from its neighbours' depths, `shadowRays` rays (1..8, default 2) over the
  sun's disc of `shadowSoftness` times `Environment::sunAngularRadius` (a Vogel disc, rotated per pixel and frame),
  and `occlusionRays` rays (default 2), cosine-weighted over the hemisphere, up to `occlusionRadius` (1 m): occlusion
  is `1 - occlusionStrength * mean(1 - distance / radius)`. A cut-out hit below its cutoff is passed through; a
  translucent one multiplies the light by its transmittance as the transmittance map does
  (`quenchTransmittanceOf`, each instance once per ray); translucent instances are not in the occlusion rays. The
  result is an `Rgba8Unorm` mask: rgb the sun's transmittance, alpha the occlusion.
- **Denoise** (`rt_denoise_fs`): depth-aware box blurs, 3 x 3 for soft shadows (none for hard ones, softness 0) and
  5 x 5 for the occlusion; a neighbour whose view depth differs by more than 2 % does not count. TAA's history then
  averages the per-frame rotations. Hard shadows without occlusion skip the pass. Decided without the user
  (2026-11-06): a spatial blur plus TAA, no temporal filter of its own, because TAA already accumulates and the blur
  costs 0.4 to 1.0 ms at 1080p ([Performance](performance.md#measurements)).
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
- Without the capability: bilinear, with the anti-aliasing the settings ask for. After MetalFX the game's `PostHdr`
  passes get the target-size image and no depth (`PassContext::depth` is empty).
- Test (`render_metal_native.renderer_upscaling`, against native TAA at full size): spatial from 0.5 mean 1.63 (2.03 % of the
  pixels over 16), temporal mean 1.15 (3.81 %).

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
| Ray-traced ambient occlusion | the same | none |
| Spatial upscaling (MetalFX) | `gpu::Caps::spatialUpscaling`, render scale below 1 | bilinear in tone mapping |
| Temporal upscaling (MetalFX) | `gpu::Caps::temporalUpscaling` and TAA's inputs | bilinear, with TAA or FXAA |

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
quench_add_shaders(my_game NAME my_surface SOURCE shaders/checker.hlsl ENTRIES surface_vs:vertex surface_fs:fragment)
```

```cpp
const render::SurfaceId checker = *renderer.addSurface({
    .vertexShader = quench::shaders::my_surface.find("surface_vs"),
    .fragmentShader = quench::shaders::my_surface.find("surface_fs"), .name = "checker"});
scene.addMaterial({.surface = checker, .params = {...}});
```

- `QuenchSurfaceInput`: world position, normal (flipped on back faces), view direction, uv, color, the instance's
  `custom` vector, pixel position, front face, tangent (world space; w the bitangent sign, turned around by a
  mirroring transform). `quenchNormalFromMap(i, texel)` turns a normal map texel into a world normal. `QuenchSurface`: albedo, opacity, roughness, metallic, shading normal,
  emissive, occlusion.
- The material is `quenchMaterial` (`MaterialUniforms`: base color, emissive, roughness, metallic, cutoff, the texture
  mask, `params[4]`); its textures are `quenchTexture0..3` with `quenchSampler0..3`; `quenchHasTexture(n)` tells
  which are set (unset ones sample white).
- `QUENCH_SURFACE_VERTEX` names `void f(inout QuenchVertexIn v, InstanceData instance)`, run before the transform
  (wind, procedural motion). Build `surface_shadow_vs:vertex` too and pass it as `SurfaceDesc::shadowVertexShader`, so
  the shadows move with it. The demo's checker ground (`examples/demo/shaders/checker.hlsl`) and the test shader
  `tests/render/shaders/custom.hlsl` are examples.
- A surface used by cut-out or translucent materials also builds `surface_shadow_fs:fragment` and
  `surface_transmittance_fs:fragment` and passes them as `SurfaceDesc::shadowFragmentShader` and
  `transmittanceShader`, so its shadows have its own holes and opacity. Without them those shadows use the standard
  surface's opacity (base color alpha, texture 0 alpha, vertex alpha). In these entries `QuenchSurfaceInput::view`
  points toward the sun and `pixel` is the shadow map texel; `surface_vs` projects with the cascade there
  (`DrawUniforms::info.z`).
- `addSurface` creates the opaque pipeline at once, so a shader that does not fit the interface fails there with an
  error, not at the first draw.

### Shader library and shared uniforms

`shaders/quench/` is included by engine and game shaders: `Common.hlsli` (constants, hashes, noise), `Brdf.hlsli`,
`Shadows.hlsli`, `Sky.hlsli`, `Fog.hlsli`, `Tonemap.hlsli`, `Fullscreen.hlsli`, `Frame.hlsli` (the frame uniforms),
`Bindings.hlsli` (the `QUENCH_UNIFORM` / `QUENCH_TEXTURE` / `QUENCH_SAMPLER` / `QUENCH_STORAGE` register macros for
SDL GPU's spaces), `Surface.hlsli` and `SurfaceEntries.hlsli`. `SceneUniforms.h` defines the uniform structs once for
C++ (`quench::shader::FrameUniforms`, ...) and HLSL through `Shared.h`: only 16-byte members (`vec4`, `mat4`), so
the C++, HLSL, std140 and MSL layouts agree (`pushUniforms` checks at compile time that a block is a multiple of 16
bytes).

## Settings

`RenderSettings`: render scale (0.25..1) and upscaling (bilinear, MetalFX spatial or temporal), sun shadows (shadow
maps or ray traced), cascades (0..4), shadow map size, shadow distance, shadow filter (1, 4 or 12 taps), translucent
shadows (transmittance map or dithered), shadow rays and softness, ambient occlusion (none or ray traced) with its
rays, radius and strength, anti-aliasing (none, FXAA, TAA) and sharpening,
bloom (strength, threshold), sky on or off and the clear color, tone curve (ACES, AgX), vignette, dither.
`setSettings` can be called every frame; the targets are recreated only when a size, the shadow map size or the set
of active effects changes.

## Statistics

`Renderer::stats()`: instances and draws in the HDR pass, culled instances, instances and draws in the shadow
cascades and in their transmittance maps, moved instances (motion vectors), local lights used, the 3D resolution,
pipelines created, and (stage 8) skinned instances and vertices posed this frame and the skinning pass's GPU time
(`skinningGpuMs`, -1 where `Caps::gpuTiming` is false or before the first measurement), and (stage 9) the top
level's instances and the bottom levels built and refitted this frame (`rayInstances`, `rayBuilds`, `rayRefits`).
The demo shows them and its scenarios check them (`draws`, `shadow_draws`, `transmittance_draws`, `lights`, `moved`,
`render_w`, `skinned_instances`, `skinned_vertices`, `ray_instances`, `ray_builds`, `ray_refits`).

GPU time per pass (stage 9, where `gpu::Caps::gpuPassTiming`: the native Metal backend): `Renderer::render` wraps its
passes in timed ranges of the command buffer it records into, `upload` (the instance and light buffers), `accel`,
`prepass` and `raytrace` (ray tracing; `rays` and `denoise` inside the last), `shadows` (the cascades and their
transmittance maps), `hdr` (opaque, sky, transparent and the game's passes in them), `taa` (motion vectors and the
resolve), `upscale` (MetalFX), `bloom` and `tonemap` (with FXAA and the overlay passes). A range whose effect is off
records nothing; the game's `PostHdr` passes are in no range of the renderer. The app reads them per frame
(`app::FrameStats::gpuPasses`, inside its `game` range); the skinning pass has its own command buffer and stays in
`skinningGpuMs`. [Performance](performance.md#gpu-time-per-pass) has the numbers.

## Skinned meshes

Stage 8 ([Animation](animation.md#gpu-skinning)). A mesh with `MeshDesc::skin` (one `SkinVertex` per vertex: four
joint indices into the mesh's joints and 16-bit weights) and `jointCount` is skinned; its vertices are in the bind
pose. `Scene::setJointPalette(instance, palette, teleport)` gives an instance one matrix per joint (joint model
matrix times inverse bind matrix, as `animation::AnimationSystem::skinPalette` computes; `SceneAssets` does the same
for cooked models). Before the first palette an instance shows the bind pose.

Every frame the renderer runs one compute pass (`skin_cs` in `src/render/shaders/skin.hlsl`, 64 vertices per group,
linear blend skinning of positions and normals) over the skinned instances that are drawn in view or in a shadow
cascade, into each instance's range of a shared vertex buffer; the shadow, main and velocity passes then draw those
vertices like a rigid mesh, with the surface shaders unchanged (also the cut-out and translucent shadow casters,
which draw them with their surface's `surface_vs`). Two output buffers alternate, so the velocity pass
(`velocity_skin_vs`) has last frame's skinned positions and TAA gets motion vectors of the deformation; a palette
set with `teleport` has none. Culling uses bounds from the palette: per joint the sphere around the bind-pose
vertices it moves, transformed by the joint's matrix. The skinning runs in its own command buffer, so its fence gives
`skinningGpuMs` where the backend measures GPU time (Metal). A mesh with a bad skin (a joint index out of range, no
joints, a skin entry count other than the vertex count) is refused with an error by `Scene::addMesh`. The compute
pass needs `quench::gpu`'s compute support, which every SDL GPU backend has; there is no CPU skinning fallback.

## Limitations

- Ray tracing: the sun only (local lights cast no shadows of any kind), no reflections or indirect light yet. The
  rays start from a normal made from the depth, not the shading normal, offset by 0.2 % of the view distance. Rays
  read the standard surface's opacity (base color, texture 0 and vertex alpha; at most 8 textures), not a custom
  surface's `surface_shadow_fs`, and `QUENCH_SURFACE_VERTEX` deformations are not in the acceleration structures.
  Skinned casters further than `shadowDistance` cast no ray-traced shadow. The top level is rebuilt every frame and
  bottom levels are not compacted (not cheap: a GPU round trip per structure and swapping it under live top levels;
  decided without the user, 2026-11-06). At 1 ray per pixel without TAA the soft shadows and the occlusion are noisy.
- MetalFX's temporal scaler cost about 2.1 ms of GPU time at a 1080p output in the demo (vsync, GPU clocks not
  fixed), so a half-size frame with it was not much cheaper than a native one with TAA
  ([Performance](performance.md#measurements)).

- Translucent shadows: a caster is one layer (its faces toward the sun; double-sided materials both), with no
  thickness, absorption along the path, refraction or caustics. A receiver between two translucent casters that share
  a map texel takes both tints; within 4 map texels behind a caster it takes only part of the tint (none within 2).
  The map has half the shadow map's resolution, so these shadows are softer than solid ones. Local lights cast no
  shadows of any kind.
- Cut-out and translucent casters run their surface function in the cascades' passes (cost per shadow texel); a
  custom surface without `surface_shadow_fs` / `surface_transmittance_fs` is cut by the standard opacity instead of
  its own.
- The motion vectors use the instance transforms and skinning only: a `QUENCH_SURFACE_VERTEX` deformation does not
  reach TAA (moving vertices smear a little under TAA), and a moving cut-out object's motion also covers its holes
  (the background seen through them may smear under TAA).
- Skinning: four joints per vertex, linear blend (no dual quaternions), no blend shapes; every drawn skinned
  instance is skinned every frame (no caching of unchanged poses, no skinning LOD); the output buffers grow with the
  skinned vertices drawn (two copies).
- No MSAA, no image-based lighting from a cubemap (the sky model is the environment), no shadows from
  local lights, at most 16 local lights per frame and no clustered or tiled light lists.
- One render per scene and frame (several views of one scene need a scene per view).
- Not in this stage, listed in the roadmap: Ashfall's screen-space effects (ambient occlusion, bounce light,
  reflections, sun shafts), particles, terrain, decals, quality presets. GPU timing per pass exists only on the
  native Metal backend (SDL GPU has no timestamps inside a command buffer).
