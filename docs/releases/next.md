# Quench (next)

Changes since [0.11.0](v0.11.0.md) (tag `v0.11.0`), not released yet: what the first game on the engine (Cloudsong,
grassy isles above a sea of clouds) needed from it.

## What changed

### Particles and trails (stage 14)

- **A new headless module, `quench_particles`, and particles and trails in the renderer**
  ([Particles](../particles.md)). `Scene::addParticles(particles::ParticleDesc, transform)` registers a system:
  spawns by rate, per metre the emitter moves, bursts and points a game gives, in a point, sphere, box, disc or
  along a path, deterministic and frame-rate independent; gravity, drag, curl noise and the weather's wind; size and
  HDR color over the life, twinkle, additive or alpha (lit) blending, camera-facing or velocity-stretched quads,
  procedural sprites, a soft fade against the scene. `Scene::addTrail(particles::TrailDesc)` and `moveTrail` draw a
  smooth camera-facing ribbon behind a point the game moves, thinning and fading toward the tail, with a glow at the
  head and sparkles shed into a system. `particles::Path` is a centripetal Catmull-Rom curve for emitters and games.
- The renderer steps systems by compute (`RenderSettings::particles = Gpu`, the default) where
  `gpu::Caps::computeShaders`, else on the CPU with the same functions (`shaders/quench/ParticleField.h`), and draws
  them after TAA and upscaling, before bloom; not in the ray tracing structures, off in the path tracer's frames.
  `RenderSettings::particleDensity`, `RenderStats::particleSystems`, `particles`, `particleSlots`,
  `particleSpawns`, `trails`, `trailVertices`; GPU ranges `particles_sim` and `particles`.
- Trails fade out where they point along the view (the sine of their angle to it from 0.3 down to 0.08): seen from
  behind a point flying away from the camera, a wide strip no longer fans out into a starburst of thin quads round
  the point or folds into wedges where it bends (found on Cloudsong's cape).
- The demo's section 15, Particles: light streams along closed paths with braided heads, a sparkle field, fireflies,
  a campfire with sparks and smoke at the blue hour. In the compact layout the menu sheet shows its buttons above the
  list of sections (a phone in landscape reaches them without scrolling).

### Weather and clouds

- **A camera above the clouds** ([Weather](../weather.md)). `Weather::coverAbove(point, radius)`: the cover of the
  clouds above a point (each sample's coverage times the share of its clouds' height above it), the same as `cover`
  at and under the cloud base, 0 over the tops. The renderer dims the sun and greys the sky's light by it, so a
  camera flying over a sea of clouds sees the sun and the clear sky (before, the clouds below dimmed them too). The
  sky pass draws the volumetric clouds below the horizon when the camera is above the cloud base, and the 2D layer,
  seen from above its height, is drawn below the horizon and lit on its tops. Under the cloud base nothing changes.
- **The clouds' shadow on high ground** ([Weather](../weather.md), "Light under the clouds").
  `RenderSettings::cloudShadowHeight` (0 m by default): the cloud shadow map lies on that plane and holds only the
  clouds above it, so ground above a sea of clouds is no longer shaded by the clouds below it. With 0 nothing
  changes.
- **The rain, the clouds in motion and the shafts** (a user report on the demo's Weather section; [Weather](../weather.md)).
  The rain's streaks are a sixtieth of a second of a drop's path, a drop's size through a lens, soft, at most 0.9
  opaque, at least 1.5 px wide, lit by the clouds' bases instead of the sky's ambient term (pi times as bright under
  overcast), denser near the camera; the splashes 6 cm for 0.12 s. The volumetric clouds' accumulation reads the
  march's noise at a new place every frame, its history through a Catmull-Rom filter and this frame through a tent:
  no hatching or smear while the view turns. The shafts of rain and snow end on the ground plane where a ray misses
  the scene, fade their noise and the clouds' shadow with the step, move their noise every frame and are smoothed at
  their reduced size: no dotted rays or dotted band at the horizon. `quenchCloudShadow` fades to the mean light over
  its map's outer twentieth instead of clamping. The shafts cost up to about 11 % more (the snow's 10 to 15 %), the
  clouds 2 to 4 %.

### Rendering

- **View rays far from the world's origin.** `quenchViewRay` (the sky, the clouds, games' shaders), the clouds' march
  and reprojection rays and the rain shafts' rays are read at depth 0 of the renderer's infinite reversed projection,
  where the inverse view-projection gives a direction without the camera's translation. Before, a point on the near
  plane minus the camera's position lost the direction's precision away from the origin (about 2 mrad at 2.2 km,
  different for every TAA jitter offset and every sub-millimetre move of the camera): over a sea of clouds the
  clouds below shook up and down, and the clouds' temporal accumulation multiplied the error. Near the origin the
  image is the same apart from the volumetric clouds, which sat about a pixel off before.

### App

- **ImGui ID conflicts in scripted runs.** Two ImGui items with the same ID in one frame (a label repeated in one
  window) were reported by ImGui only under the mouse. `quench_imgui` is built with `IMGUI_ENABLE_TEST_ENGINE` and
  the engine's hooks, which the app turns on in scripted runs and builds with assertions: each shared ID is logged
  once as an error and counted in the scenario value `imgui_id_conflicts` ([App](../app.md)). The demo's Weather
  panel had one (its Cirrus preset and slider) and its batches now expect none.

## What was verified

- Particles: `particles.*` 15 cases (also as WebAssembly under Node.js); `render.renderer_particles_*`,
  `renderer_trails_*`, `renderer_particle_slots_are_limited` 9 cases on SDL GPU's Metal driver, the native backend
  and Vulkan through MoltenVK (the GPU and the CPU simulation 0 levels apart for a glow, a mean of 0.000 for 120
  moving particles); the demo's `particles` part 26 checks on the three backends, `demo_particles.png` and
  `demo_particles_fire.png` the same images on SDL GPU's Metal driver and the native backend, Vulkan through MoltenVK
  within a mean of 0.0008. Cost on the M4 Pro, offscreen, native backend, 1080p: 0.119 ms of compute and 0.389 ms of
  draws for 23488 slots (about 7000 particles alive) and 20 trails. The gates: see the devlog of 2026-11-08.
- The rain, clouds and shafts after the user's report, offscreen on the Mac (native Metal and SDL GPU's Metal driver,
  the same images): the clouds' motion error (a frame a few frames into a turn against a still frame of the same
  view) 0.89 to 0.42 from the ground and 1.09 to 0.25 over the storm; the shafts without dots or band in single
  frames and TAA stills; 22 of the demo's golden images replaced after looking at each pair (Metal and Vulkan
  through MoltenVK). The ImGui check found the Weather panel's conflict before its fix and none after.
  `tools/check.sh full` passed (the new game's step on a second run, after the disk had run full).
- `weather.weather_cell_in_clear_sky`: `coverAbove` 1.000 under the base (equal to `cover`), 0.500 halfway up the
  cell, 0 over its top. `tools/check.sh quick weather render demo` 260 of 260 (the demo's captures, Metal and the
  native backend, unchanged), `step wasm` and `step new-game` passed.
- A camera over the clouds was looked at in captures of the game, volumetric and 2D layer (Mac, native Metal).
- `render.renderer_cloud_shadow_height` (and on Vulkan through MoltenVK and the native backend): a raining cell's
  shadow on a ground plane at 9500 m, over the clouds' top, is 0.000 of the sunlit 0.775 with the plane at 0 and
  0.775 with the plane at the ground's height; at 300 m, under the cell, 0.073 with the plane at 300 m.
  `tools/check.sh quick render demo` 241 of 241 (the demo's captures unchanged).
- `render.renderer_view_rays_far_from_the_origin` (SDL GPU's Metal driver, the native backend, Vulkan through
  MoltenVK): a low sun seen from 20 km matches the one seen from the origin (largest channel difference 2, mean
  0.011; before the fix 108 and 1.744); clouds seen from 3 km do not change when the camera rises by 1 mm (the 2D
  layer 2 and 0.001, before 45 and 0.650; the volumetric clouds 1 and 0.006, before 8 and 0.131). In the game,
  standing still at 2.2 km, the cloud band's mean frame-to-frame difference fell from 0.511 to 0.100 (Light,
  2560x1440) and its vertical shift from 0.261 px at most to 0.075. The demo's `demo_weather.png`,
  `demo_weather_sunset.png` and `demo_snow_sky.png` were replaced (the clouds about 1 px lower in the frame; mean
  differences 0.59, 0.58 and 0.66 against the old images); `tools/check.sh quick render demo hello` 254 of 254.

## What was not verified

- Particles: nobody has looked at them in motion or on the screen; nothing of them ran on a phone (no phone costs).
- Nothing of it on a phone. The shadow map has one plane: ground far above or below it is still shaded by the
  column its sun ray crosses at that height.
- The rain, clouds and shafts after the report were looked at in offscreen captures on the Mac only (still and
  after a few frames of motion), not on the screen, in motion or on a phone; their costs on the Mac only.
- The view ray fix was measured offscreen on the Mac; nobody has looked at it on the screen yet. The engine still
  renders in world coordinates: positions near the camera keep a float's precision at their distance from the
  origin (0.24 mm at 2.2 km), so worlds of tens of kilometres would want camera-relative rendering.

## Changes for games

Particles: `quench_render` links `quench_particles` (nothing to add); `render/Scene.h` includes
`quench/particles/Particles.h` and `Trail.h`. `gpu::Caps` has a new member, `computeShaders`. A scene without
particle systems or trails renders as before.

Weather: `coverAbove` and `cloudShadowHeight` are new; `cover` is unchanged, and the shadow with the default height is
the same as before. `quenchViewRay` returns the same directions, more precisely; a game shader that builds its own
view rays from `FrameUniforms::invViewProj` at the near plane should read them at depth 0 or call `quenchViewRay`. A
game's golden captures with volumetric clouds may move by about a pixel. A game's captures with rain, snow, shafts
or volumetric clouds change with the rain's and shafts' new look and the clouds' accumulation.

App: `quench_imgui` defines `IMGUI_ENABLE_TEST_ENGINE` (public) and implements ImGui's test engine hooks; a game that
links ImGui's own test engine or defines those hooks itself would get duplicate symbols. A game's scripted runs log an
error for every ImGui ID two items share; `expect imgui_id_conflicts == 0` checks for them.
