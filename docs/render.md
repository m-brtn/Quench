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
- **Meshes** use the engine vertex (`render::Vertex`, 48 bytes: position, normal, uv, linear color) and 32-bit
  indices, counter-clockwise front faces. `MeshBuilder.h` makes boxes, planes, spheres and tori. Bounds for culling
  are computed on upload.
- **Textures** are RGBA8 (`TextureData`, sRGB or linear), mipmaps generated on the GPU. Loading files is stage 6.
- **Materials** replace the games' fixed material enums: base color, emissive, roughness, metallic, alpha cutoff,
  blend mode (opaque, alpha blend, additive), double-sided, shadow casting, four textures and four `vec4` parameters
  for custom surfaces.
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

1. **Shadow cascades** of the sun (standard depth, `Depth32Float` texture array): the view is split between linear and
   logarithmic distances up to `shadowDistance`, each cascade snapped to its texels; casters are culled per cascade.
   Game `Shadow` passes run in each cascade after the scene's casters.
2. **HDR pass** (`Rgba16Float`, reversed depth: near 1, far 0, infinite far plane): opaque surfaces sorted by
   pipeline and material, then game `Opaque` passes, then the sky (atmosphere or gradient, sun disc, fog toward the
   horizon), then alpha-blended and additive surfaces back to front, then game `Transparent` passes.
3. **Motion vectors and TAA**: camera motion from the depth, moved instances drawn again into an `Rg16Float` target;
   TAA with a Halton (2, 3) jitter over 8 frames, the history clipped to the variance box of the 3 x 3 neighbourhood
   (YCoCg), then sharpening. Render scale below 1 renders the 3D
   part smaller; tone mapping scales it to the target.
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

### Fallbacks

`Renderer::effects()` is what runs after the device check; `RenderSettings` is what the game asked for. The choice is
made from what the device supports (`gpu::Device` format queries and `gpu::Caps`), never from the platform, and a
change is logged once:

| Effect | Needs | Fallback |
|---|---|---|
| Shadow cascades | a sampleable depth texture array | no shadows (`shadowCascades = 0`) |
| Casters in front of a cascade | `gpu::Caps::depthClamp` | without it they are clipped (pancaking off) |
| TAA | a sampleable scene depth and an `Rg16Float` target | FXAA |
| HDR target | `Rgba16Float` | `Rg11b10Ufloat` |

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
  `custom` vector, pixel position, front face. `QuenchSurface`: albedo, opacity, roughness, metallic, shading normal,
  emissive, occlusion.
- The material is `quenchMaterial` (`MaterialUniforms`: base color, emissive, roughness, metallic, cutoff, the texture
  mask, `params[4]`); its textures are `quenchTexture0..3` with `quenchSampler0..3`; `quenchHasTexture(n)` tells
  which are set (unset ones sample white).
- `QUENCH_SURFACE_VERTEX` names `void f(inout QuenchVertexIn v, InstanceData instance)`, run before the transform
  (wind, procedural motion). Build `surface_shadow_vs:vertex` too and pass it as `SurfaceDesc::shadowVertexShader`, so
  the shadows move with it. The demo's checker ground (`examples/demo/shaders/checker.hlsl`) and the test shader
  `tests/render/shaders/custom.hlsl` are examples.
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

`RenderSettings`: render scale (0.25..1), cascades (0..4), shadow map size, shadow distance, shadow filter (1, 4 or
12 taps), anti-aliasing (none, FXAA, TAA) and sharpening, bloom (strength, threshold), sky on or off and the clear
color, tone curve (ACES, AgX), vignette, dither. `setSettings` can be called every frame; the targets are recreated
only when a size, the shadow map size or the set of active effects changes.

## Statistics

`Renderer::stats()`: instances and draws in the HDR pass, culled instances, instances and draws in the shadow
cascades, moved instances (motion vectors), local lights used, the 3D resolution, pipelines created, and (stage 8)
skinned instances and vertices posed this frame and the skinning pass's GPU time (`skinningGpuMs`, -1 where
`Caps::gpuTiming` is false or before the first measurement). The demo shows them and its scenarios check them
(`draws`, `shadow_draws`, `lights`, `moved`, `render_w`, `skinned_instances`, `skinned_vertices`).

## Skinned meshes

Stage 8 ([Animation](animation.md#gpu-skinning)). A mesh with `MeshDesc::skin` (one `SkinVertex` per vertex: four
joint indices into the mesh's joints and 16-bit weights) and `jointCount` is skinned; its vertices are in the bind
pose. `Scene::setJointPalette(instance, palette, teleport)` gives an instance one matrix per joint (joint model
matrix times inverse bind matrix, as `animation::AnimationSystem::skinPalette` computes; `SceneAssets` does the same
for cooked models). Before the first palette an instance shows the bind pose.

Every frame the renderer runs one compute pass (`skin_cs` in `src/render/shaders/skin.hlsl`, 64 vertices per group,
linear blend skinning of positions and normals) over the skinned instances that are drawn in view or in a shadow
cascade, into each instance's range of a shared vertex buffer; the shadow, main and velocity passes then draw those
vertices like a rigid mesh, with the surface shaders unchanged. Two output buffers alternate, so the velocity pass
(`velocity_skin_vs`) has last frame's skinned positions and TAA gets motion vectors of the deformation; a palette
set with `teleport` has none. Culling uses bounds from the palette: per joint the sphere around the bind-pose
vertices it moves, transformed by the joint's matrix. The skinning runs in its own command buffer, so its fence gives
`skinningGpuMs` where the backend measures GPU time (Metal). A mesh with a bad skin (a joint index out of range, no
joints, a skin entry count other than the vertex count) is refused with an error by `Scene::addMesh`. The compute
pass needs `quench::gpu`'s compute support, which every SDL GPU backend has; there is no CPU skinning fallback.

## Limitations

- Alpha-tested and transparent materials cast solid shadows (no cut-out or colored shadows).
- The motion vectors use the instance transforms and skinning only: a `QUENCH_SURFACE_VERTEX` deformation does not
  reach TAA (moving vertices smear a little under TAA).
- Skinning: four joints per vertex, linear blend (no dual quaternions), no blend shapes; every drawn skinned
  instance is skinned every frame (no caching of unchanged poses, no skinning LOD); the output buffers grow with the
  skinned vertices drawn (two copies).
- No normal maps, no MSAA, no image-based lighting from a cubemap (the sky model is the environment), no shadows from
  local lights, at most 16 local lights per frame and no clustered or tiled light lists.
- One render per scene and frame (several views of one scene need a scene per view).
- Not in this stage, listed in the roadmap: Ashfall's screen-space effects (ambient occlusion, bounce light,
  reflections, sun shafts), particles, terrain, decals, GPU timing per pass, quality presets.
