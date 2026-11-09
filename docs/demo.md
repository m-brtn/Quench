# Engine demo

`examples/demo` (target `quench_demo`, executable `quench-demo`, app name "Quench Demo") is one app on `quench_main`
and `quench_app` for the Mac and iOS: a menu of sections, each a small interactive scene of primitives and
procedural content that shows one area of the engine, with ImGui controls and the frame, CPU and GPU times. It was
started in stage 5 (user decision, 2026-11-05); every later stage added its section, and stage 10 of the
[Roadmap](roadmap.md) finished it (2026-11-06): one panel layout and one camera for every section, a compact layout
for phones, a tour, a check mode and a benchmark of every section, an About panel, saved settings. `examples/hello`
stays the minimal sample.

Every section has a scripted check and golden captures, so the demo is also a test of the engine, and every stage
ends with a packaged build of it in `dist/` (`tools/package_demo.sh`, below).

## Code

| File | What |
|---|---|
| `Demo.h` | the `Section` interface (id, title, description, `init`, `enter` / `leave`, the steps, `controls()`, `stats()`, scenario values, commands and points), `OrbitCamera`, `Context` and the helpers sections share |
| `sections/<Name>Section.cpp` | one file per section: its scene, controls, numbers and scenario hooks |
| `DemoGame.h`, `Demo.cpp` | the app around the sections (`quench::app::Game`): sections made at the start and initialized when first shown, switching, the settings, the scenario hooks, the flags (`createGame`) |
| `Ui.cpp` | the two layouts, the menu, the Performance and Log panels, the section's panel, About and Settings |
| `GraphicsPanel.h`, `GraphicsPanel.cpp` | the Graphics panel: render options for every section, what runs after the fallbacks, its part of `settings.json`; the display options and the relaunch shared with Settings and About |
| `Camera.cpp` | the camera controls every 3D section shares |
| `Modes.cpp`, `Checks.h`, `Checks.cpp` | the tour, the check mode with its items and `checks.json`, the benchmark of every section |
| `Settings.h`, `Settings.cpp` | `settings.json` |
| `Common.cpp` | the "Frame and fallbacks" panel and the helpers of `Demo.h` (meshes, placement, ground picks, actions, parsing) |
| `BuildInfo.cmake` | the build type and the commit for About (`demo_build_info.h`, written at build time) |

A section is made when the app starts (only its camera and render settings) and initialized when it is first shown:
0.3 to 3.3 ms per section in Release on the M4 Pro (measured 2026-11-06; assets then load on the job system). It
stays loaded until the app ends or it is reset; "Reset section" replaces it with a new object of its kind, as on its
first visit. Leaving a section removes its render passes; its scene stays.

## Sections

| # | Id | Shows | Controls besides the camera |
|---|---|---|---|
| 1 | `materials` | Dielectric and metal spheres from smooth to rough, a textured box, glass (alpha blend) with its translucent, tinted shadow, an alpha-tested fence whose shadow has its holes, an emissive lamp; the sun, three point lights and a spot; a game surface shader (the checker ground) and a game render pass (the light discs) | lights on and off, light intensity, animation, discs, checker |
| 2 | `shadows` | Sun shadow cascades over 90 m of columns, thin bars, a spinning torus, a bouncing ball | cascades (0 to 4), map size, distance, filter (1, 4, 12 taps), sun elevation and azimuth, turning sun |
| 3 | `sky` | The daylight palette for any sun elevation, the atmosphere model and its gradient fallback, the clear color without a sky, height fog; the sun disc above its glow (in SDR at a low sun both are white) | sun, run the day, sky, atmosphere, sun disc, fog density, falloff, base, maximum, sun scatter |
| 4 | `post` | Bloom, tone mapping and anti-aliasing at dusk: glowing rings and lamps, thin poles, spinning blades | none, FXAA, TAA (with the fallback shown), render scale, sharpen, bloom strength and threshold, ACES or AgX, exposure, vignette, dither |
| 5 | `hdr` | HDR display output: a sun at 14 degrees, seven emissive panels from 0.5 to 32 times the radiance at the HDR knee, a white card in the shade; with HDR the sun and the brighter panels go above paper white, the card and the UI do not | output mode (off, auto, on), the display's headroom and support and the output in use, the SDR curve for comparison, a custom peak instead of the display's, panel brightness, sun elevation |
| 6 | `physics` | Jolt through `quench::PhysicsWorld` at 120 Hz, drawn interpolated: a stack of boxes and balls; since 2026-11-08 a wind of the weather's field (`quench_weather`) that `quench::WindDrag` blows the light pink balls with (off by default) | drop a box (B, gamepad X), drop a ball (N, gamepad Y), drop a light ball (L), blast (P, gamepad B, or a click or tap on the floor), reset (R, gamepad Back); the wind at 10 m (0 to 25 m/s) and its gusts |
| 7 | `input` | Named actions over every device, shown live: a character that walks, runs, jumps and fires | WASD, left stick or a finger on the left half: walk; Left Shift or the left stick button: run; J, gamepad A or a tap on the right half: jump; F, gamepad X, a click or a tap: fire |
| 8 | `assets` | Cooked assets loaded through the app's asset manager ([Jobs, assets and the cook](assets.md)): a row of models (a crate from a glTF with an external texture, a pillar from a GLB with embedded JPEG and PNG textures, a vertex-colored ball, a corrupt file and a missing one) shows the load states: gray boxes while loading, magenta on failure. Behind it a 6 x 5 grid of tiles, each with one of eight textures and a model, streams around a moving focus point; the tiles' textures come from the platform's set (BC7, ASTC 4x4 on iOS) or from one of two Basis Universal sets (ETC1S, UASTC; [Basis Universal](assets.md#basis-universal)), transcoded at load time to what the device samples | simulated latency, texture cache budget (unlimited, 64, 16, 4 KB), uploads per frame (8 MB, 16 KB, 1 byte), the tile textures' set and a line with what they became (the format, transcoded from which codec or decoded on load, the time per file), focus angle, moving focus, models on and off, hot reload, reload all; counts of ready, loading, failed, queued and in-flight assets, reloads, evictions, memory per type against its budget, streamed cells |
| 9 | `audio` | The mixer and 3D sound ([Audio](audio.md)): a head with ears at the listener, facing the way the camera looks; an orange emitter circling it at ear height (a motor loop), a blue fly-by passing at speed (a siren, with Doppler), a green box that plays a tone the demo computes (a generator); streamed music A and B with a crossfade; all sounds procedural | emitters on and off, orbit and fly-by speed, tone frequency and position, attenuation model, minimum and maximum distance, Doppler scale; music off, A or B; per bus volume, mute and a peak meter; pause effects; the reverb on or off (the 3D sounds send to a reverb bus under the effects) and its room size; the HRTF for headphones on or off (the panel then shows the HRTF voices); a ping, a burst of 40 pings (voice stealing), a click or tap on the ground plays a ping there; voices, stolen and rejected sounds, underruns, clipped samples, mix time and the output device |
| 10 | `animation` | Skeletal animation ([Animation](animation.md)) of a procedural mannequin: the hero walks a circle by root motion with a speed blend of idle, walk and run, jumps, waves (a layer masked to the right arm), leans (an additive layer), turns its head toward the camera (look-at), holds a torch on its left hand (an attachment) and plays a step sound at each footstep event (a louder one at the jump's landing); two characters stand with foot placement on a ramp and across a 0.2 m step (rays into a Jolt world); a crowd walks in rings; in the view "2D blend space", away from them, a character steered by its velocity through a 2D blend space of idle, walk, run, walk back and the two strafes walks ahead, sideways and backward inside a ring while it faces one way; in the view "Skinning", on the other side, a close-up of two checkered bars on posts whose upper halves the section turns by 170 degrees about their length, and beside them two standing characters whose inner forearms it turns the same way: the left pair, labelled "Linear (reference)", is always skinned linearly (the twist pinches it to a thin neck), the right pair follows the dual quaternion checkbox and its label ("Linear" or "Dual quaternion"; dual quaternions keep it round, its squares running around it in a spiral), and a slider sets the twist (0 to 180 degrees); in the view "Retargeting", behind the hero's camera, the mannequin and a second character with other joint names, proportions and rest pose (the strider, its walk retargeted from the mannequin's at cook time) walk the same clip abreast, the camera following them; in the view "Blend shapes", behind the skinning pair (and drawn in that view only), a swaying jelly blob on three joints reshaped by four blend shapes (squash, stretch, puff, spikes) before its skinning, its clip squashing it where its sway turns and stretching it in between through their weights; animation LOD as an option (off by default): the characters small on the screen update every 2nd or 4th frame; dual quaternion skinning for the showcase's right pair and the other characters (not the linear reference or the blob) as an option (off by default) | speed, lean, jump (J, gamepad A), wave (K, gamepad X), restart; foot IK, look-at and footstep sound on and off; ramp angle (0 to 30 degrees), crowd size (0 to 512); animation LOD on and off, interpolated or held; dual quaternion skinning on and off and the showcase's twist, with a line on what to look at; the view (the hero's stage, the 2D blend space, the skinning showcase, the retargeting pair or the blob); the blob's weights from its clip on and off, its four blend shape weights (added to the clip's) and a reset; the steered character by itself (around a circle) or by two velocity sliders; the velocity it is asked for and the one its clips move it by; the hero's state and transition, characters, joints, clip samples, ground rays, events, skinned instances, vertices and reused skins, the characters updated and at each rate, the update time and threads, the skinning GPU time |
| 11 | `raytracing` | Ray tracing where `gpu::Caps` allow it ([Render module](render.md#ray-traced-shadows-and-ambient-occlusion)), on the native Metal backend (the default on Apple platforms since 2026-11-07; `--gpu-backend sdl` gives the fallbacks): soft ray-traced sun shadows through a fence's diamond holes (cut-out), tinted amber by a spinning glass torus (translucent) and cast by the animation section's mannequin walking a circle (skinned, refitted every frame), ambient occlusion under a crate and a column; since stage 11 ([Ray-traced lighting](render.md#ray-traced-lighting)) a framed mirror and five gold spheres from roughness 0.02 to 0.48 (traced reflections), a red and green corner with a white block (light bounced by the probe grid), a lamp in the corner and a spot behind the fence (lamp shadows, the spot throws the fence's holes forward), a warm emissive panel that lights the ground through the probes; MetalFX temporal upscaling with the engine's temporal denoiser at render scale 0.67 where the device has MetalFX (MetalFX denoising costs about three times as much on the M4 Pro and is a choice in the panel); on phones and tablets that trace (`App::mobile()`) render scale 0.5 with MetalFX spatial upscaling, no probe grid and 60 frames per second ([Performance](performance.md#stage-11-on-the-iphone)); MetalFX spatial upscaling, the engine's denoisers, and the path tracer as a reference (it stops the sun, the torus and the walk). On SDL GPU (Metal or Vulkan through MoltenVK) the same scene with the fallbacks and a line saying why; there the lamps can have shadow maps, the occlusion GTAO and the reflections SSR (options, off by default) | sun shadows (shadow maps or ray traced), shadow rays, softness, ambient occlusion with its rays and radius (and GTAO where it is not traced), lamp shadows (and their shadow maps where they are not traced), reflections
(and SSR where they are not traced), indirect light (sky, probes, rays), the denoiser, upscaling (bilinear, MetalFX spatial, temporal, denoising) and render scale, the path tracer, each with the fallback it got; turn the sun, walk, sun elevation, 60 Hz (on by default on phones and tablets; while the section is current); the top level's instances, builds, refits and compactions (or the shadow draws), the hit textures, probes and path samples, and outside deterministic runs the GPU time of the rays, denoising, probes, reflections, indirect light, the path tracer, the shadow maps, GTAO, MetalFX and the frame |
| 12 | `tunnel` | Area lights and anisotropic GGX ([Area lights](render.md#area-lights), stage 12): a 27 m tunnel at night lit only by its lamps, eight area lights (two cyan strips in the floor, two warm squares in the ceiling, a magenta panel on a wall, an orange and a violet neon tube, a round blue portal at the end) and two spots; metal wall panels with color, normal and metallic-roughness maps, wet asphalt with a near-mirror puddle, a rubber mat and wheel stops, embossed plastic crates, brushed aluminium (anisotropic: kick plates, ribs, the sphere). The textures are procedural, written at build time. The lamps move on a clock of their own: the violet tube rides two ceiling rails along the tunnel, the orange tube slides in its bay, the amber spot turns its cone, the white one circles a sphere and the portal breathes, their hangers, brackets and spot housings with them (instance transforms, refitted in the top level). The camera drives a loop through the tunnel by default, eased, looking down the tunnel; a drag, Reset camera or a script's camera hands it to the free camera, and everything stands still while the path tracer runs. Where `gpu::Caps` allow it (the native Metal backend) four lamps cast soft traced shadows, reflections are traced and the probe grid lights the rest, at render scale 0.67 with MetalFX temporal; on phones and tablets that trace half size with MetalFX spatial, two lamp shadows, no traced reflections and the sky's light; phones that do not trace at three quarters of the size; every phone at 60 frames per second ([Performance](performance.md#the-tunnel-on-the-iphone)); the path tracer as a reference. On SDL GPU the lamps light and shine (LTC) without shadows (the two spots with shadow maps if the Graphics panel's option is on) and reflect only the night sky | ray traced or path traced, lamp shadows, traced reflections, each lamp group on or off, the aluminium's anisotropy (0 to 1), the puddle, 60 Hz; Pause all, Lamps move, the camera's Drive or Free, Stop and Drive speed; the lamps and area lights in the frame and outside deterministic runs the GPU time of the shading, the rays, the denoiser, the reflections, the path tracer and the frame |
| 13 | `navigation` | Navigation ([Navigation](navigation.md), 2026-11-07): a navmesh built at run time on the job system over a level of two floors (decks at 2.4 m) joined by a ramp, stairs and a narrow bridge, with a mud field that costs more to cross and one-way jumps down the decks' ledges (off-mesh links); two crates and a barrel move, cut holes in the navmesh and have their tiles rebuilt on jobs. Two groups of mannequins (orange and green) patrol their rounds with crowd avoidance, single file over the bridge, around the mud, down the stairs and the ramp, jumping down a ledge on an arc with the jump clip, their walk and run following their speed; a click or a tap on a floor sends the chosen group there in a formation. Overlays: the navmesh by area (its walls and edges, the links' arcs), the obstacles, each agent's path ahead, its circle and a marker over its head in the group's color | the group (orange, green, both), agents per group (1 to 12), patrol, moving obstacles, the mud's cost (1 to 10), the navmesh and paths overlays, scatter, restart; the navmesh's tiles, layers, polygons and memory, the obstacles and pending tiles, the agents walking, on links and lost, the jumps, a probe path across the field (its length and meters in the mud), and outside deterministic runs the last build's time on the job threads, the crowd step and the last click's path search |
| 14 | `weather` | Weather ([Weather](weather.md), stage 13 parts 1 and 2, 2026-11-07, parts 3 and 4, 2026-11-08): a plain 20 km wide with 48 hills, six towers and a lake, seen from a few hundred metres up, under the clouds of a `quench_weather` field drifting with the wind and changing shape: fair-weather cumulus with flat grey bases and bright tops, towering cumulus, cumulonimbus, an overcast stratus deck, high cirrus, and one raining cumulonimbus placed as a cell in a blue sky (its rain at the camera and the distance to it in the panel); the clouds' shadows crossing the plain, the sun dimming and the light greying as the cover closes, the lake reflecting the clouds through the sky cube, sunsets from the sky's own light. Volumetric clouds at the render size or at half of it with accumulation, or the 2D cloud layer (the fallback). On the ground (part 2): rain only where a cloud rains, falling with the wind past a dark block, splashes, wet paving with puddles and rings, a pond, a shelter whose floor and crate stay dry, a crate in the open that gets wet, shafts of rain under distant clouds, a rainbow in a sun shower, the ground drying after a passing shower, and the rain's sound (light, heavy, distant, on the roof). Below freezing (part 3): flakes fluttering and drifting with the wind, a snow cover building on the paving, the grass, the roof and the crate tops but not under the shelter or on walls, melting into wet ground and puddles in a thaw, white shafts under distant snow showers, and quiet. In the wind (part 4): a meadow beside the place, its grass (in patches), bushes and trees swaying with the weather's gusts and eddies (`shaders/plants.hlsl`), the rain and snow carried by the same gusts, and in the windy storm (a gale under a thunderstorm) lightning that lights the clouds and the meadow, the thunder after it by its distance, and the wind's sound (light and strong). On phones and tablets (`App::mobile()`) half size, 48 steps, one sky cube face per frame, 60 frames per second, the rain and the snow at Low without shafts (estimated from the Mac's costs, [Weather](weather.md#phones-and-fallbacks)) | view (from the air, from the ground, toward the cloud, the shelter, over the shelter, in the meadow); presets (fair weather, sun shower, towering, storm, overcast, cirrus, rain here, downpour, passing shower, snowfall, thaw, snow showers, windy storm), coverage, cloud type, type variation, precipitation, cirrus, a raining cumulonimbus, wind speed and direction, gusts, turbulence, change of shape, the weather's speed (0 to 60 times), the temperature, sun elevation and azimuth, Run day, Sunset; quality (full size, half size, 2D layer, off), march steps, cloud shadows, sky cube, its faces per frame, 60 Hz; rain (off, low, medium, high), snow (the same), rain shafts, rainbow, lightning and thunder and the lightning's rate, weather sound and its volume; the clouds drawn and the march's size, how overcast it is, the cell's rain here and the distance to it, the wetness, standing water and snow cover, the rain and the snow drawn and the cover, the wind at 10 m (with gusts and the mean), the lightning's light, the flashes and the thunder heard, the sound's layers, the weather's time |
| 15 | `particles` | Particles and trails ([Particles](particles.md), stage 14, 2026-11-08): light painting over a meadow at the blue hour (a sky blended from golden hour by the Dusk slider, standing stones, low hills). Three streams of light (gold, blue, rose) run along closed paths (a figure eight, a ring with loops, a low weave), 20 heads braided around their curves, each leaving a ribbon that thins and fades toward its tail and sheds sparkles and dust into the streams' particle systems; a field of 8192 twinkling points over the meadow, denser at dusk; fireflies; a campfire of stones and logs with flames, sparks streaking up (stretched along their motion), lit smoke and a flickering point light; all of it in a light wind of the weather's field. The view "Fire" goes close to the campfire. On the GPU by compute where the device has compute shaders, else on the CPU | the simulation (GPU, CPU, off) and the one that runs, density, light streams, trail sparkles, sparkle field, fireflies, campfire, stream speed, trail length (s), dusk, the wind at 10 m, a burst of sparks, restart; the systems and trails drawn, the particles alive in their slots, this frame's spawns, the trails' vertices, each part's particles |

Every section's panel has the same parts in this order: the controls, the section's numbers ("Stats"), the
"Frame and fallbacks" header (the scene's counts, the 3D resolution, draws, culled instances, lights, shadow draws,
what runs after the device fallbacks: sun shadows, cascades, depth clamp, ambient occlusion, anti-aliasing,
upscaling, translucent shadows, bloom and sky; the HDR and depth formats; every fallback with its reason, from
`Renderer::fallbacks()`; the GPU backend and its capabilities, and why there is no ray tracing where there is none),
and "Reset section" and "Reset camera". The description of what to look at is a caption under the scene (in the
compact layout, the top of the controls sheet).

## Coverage

What of the engine the demo shows, and where (audit of 2026-11-06; "added" marks what stage 10 added):

| Engine area | Where | How it shows |
|---|---|---|
| Retained scene, PBR materials, point, spot and sun lights | materials | spheres from smooth to rough, metals, glass, an emissive lamp, lights on and off; the scene's counts in "Frame and fallbacks" (added) |
| Game surface shaders and render passes | materials | the checker ground, the light discs, each on and off |
| Cut-out and translucent shadows | materials | the fence's holes, the glass's tinted shadow; the transmittance map or its dithered fallback |
| Cascaded sun shadows | shadows | cascades 0 to 4, map size, distance, filter, the turning sun; depth clamp or its fallback |
| Sky, atmosphere, fog | sky | the day cycle, the atmosphere and its gradient fallback, no sky, height fog and its sun scatter |
| Bloom, tone curves, render scale | post | bloom, ACES and AgX, exposure, vignette, dither, render scale, sharpen |
| Anti-aliasing | post | none, FXAA, TAA, with the fallback the device gave |
| HDR display output | hdr | off, auto, on; the display's headroom; a custom peak; the SDR curve beside it |
| Physics on Jolt | physics | bodies dropped and blasted at 120 Hz, drawn interpolated |
| Input actions | input; every 3D section | actions on keys, mouse, gamepad and touch with their times; the shared camera controls (added: pan, the touch gestures, the keyboard) |
| Asset loading, placeholders, failures | assets | gray boxes while loading, magenta for the corrupt and the missing model; ready and failed counts |
| Cache budgets, upload budget | assets | budgets per type, evictions, uploads per frame |
| Streaming by distance | assets | the tile grid around a moving focus; the load and keep rings on the ground and every model's load state (added) |
| Hot reload (Mac; a device through the asset overlay of a development build) | assets | the hot reload switch (on with `--hot-reload`), "Reload all", and a note "Assets reloaded: N" over the scene (added) |
| Audio | audio | buses, 3D sound, Doppler, a generator, streamed music with a crossfade, voice stealing, meters, reverb, HRTF |
| Animation | animation | blending, state machine, layers, IK, foot placement, attachments, events, root motion, a crowd on the job system, GPU skinning |
| Ray tracing, MetalFX | raytracing | ray-traced sun and lamp shadows, occlusion, reflections, the probe grid and per-pixel indirect light, the spatial and temporal denoisers, MetalFX spatial, temporal and denoising, the path tracer, each with its fallback and the reason |
| Navigation: navmesh building on jobs, area costs, off-mesh links, obstacles, paths, crowds, debug geometry | navigation | the level's navmesh built at run time and drawn by area, the mud's cost changing the paths, jumps down the ledges, moving obstacles rebuilding their tiles, two groups with avoidance sent by a click or tap (since 2026-11-07) |
| Weather: the weather map, volumetric clouds and the 2D layer, cloud shadows, overcast light, the sky cube | weather | presets from fair weather to overcast, a raining cell in a blue sky, wind and change of shape, sunset, each quality setting and its fallback (since 2026-11-07) |
| Weather: rain (streaks, splashes, shafts, rainbow), the rain occlusion map, wetness, puddles and ripples, the rain's sound | weather | a sun shower from one cloud seen from the ground, under a shelter and toward the cloud, a downpour, a passing shower drying, the rain's levels and sound (since 2026-11-07) |
| Weather: snow (flakes, the snow cover building and melting, its shafts, its quiet) | weather | a snowfall from the ground and under the shelter, the cover building over the shelter, a thaw, snow showers from the air (since 2026-11-08) |
| Weather: wind (gusts and eddies, plants swaying, particles carried), lightning and thunder, the wind's sound; the wind's drag on physics bodies | weather, physics | the meadow in a calm and in the windy storm, a flash and its thunder; light balls blown across the physics floor (since 2026-11-08) |
| Particles and trails: spawning in shapes and along paths, curl noise and the weather's wind, twinkle, additive, alpha and lit blending, velocity-stretched sparks, trails with heads and shed sparkles, the GPU and the CPU simulation | particles | light streams along closed paths, a sparkle field denser at dusk, fireflies, a campfire's flames, sparks and smoke, each part on and off, the density, the simulation and its fallback (since 2026-11-08) |
| Area lights, anisotropic GGX, normal and metallic-roughness maps | tunnel | rect, disk and tube lights with their drawn shapes (LTC on every backend), their soft traced shadows, the path tracer's area light sampling, brushed aluminium, cooked texture maps made at build time |
| The job system | the Performance panel | threads and pending jobs, a job test (256 items on the pool, its time and the items per thread) (added); physics, animation and loads run on it |
| Frame, CPU and GPU times | the Performance panel | fps, frame, CPU and GPU time, a frame-time graph (added), the GPU time per pass on the native Metal backend; hidden in deterministic runs (captures) |
| Memory and GPU resources | the Performance panel | the process's memory and the live GPU resources, the section's load and switch times (added) |
| Thermal state, the reaction to heat | the Performance panel; Settings | the thermal state, and with Adapt to heat what is reduced and for how long (since 2026-11-07) |
| GPU backend, device, fallbacks | every section's "Frame and fallbacks"; About | the backend, driver, device and capabilities, every fallback with its reason (added); a relaunch with the other backend on the Mac (added) |
| Display scale, safe area, orientation | the layouts | sizes from the display's scale, windows inside the safe area, the compact layout in portrait and landscape (added) |
| Logging | the Log panel | the demo's log lines with the warning and error counts (added) |
| User files through `quench::fs` | Settings, the check mode, the benchmark | `settings.json`, `checks.json`, `benchmark.json`, screenshots (added) |
| Scenarios, captures, perf regions | the checks | every section's check; the benchmark of every section on the same perf regions (added) |

Not shown, because the engine does not have it yet: Basis Universal textures ([Roadmap](roadmap.md)). Shader hot reload has no panel: with `--hot-reload` on the Mac, an edit of the
engine's or the demo's shaders shows in the running demo ([Shader hot reload](app.md#shader-hot-reload)).

## Layouts

Sizes follow the display's scale (the engine's ImGui style), times the UI scale of the settings.

- **Wide** (a desktop window, a tablet): the menu at the top left (the sections numbered 1 to 15, the mode buttons
  Tour, Checks and Benchmark, About, Settings, Graphics, Hide, the Performance and Log panels, and a hint of the keys
  under them: "Sections: keys 1-9, 0 = 10", "Previous, next: PgUp PgDn LB RB", "Menu: Tab, D-pad; back: Esc, B",
  "Hide: H, Start; Graphics: G"); the section's window at the top right, or under the menu where the window is too
  narrow for both; the description as a caption at the bottom; the windows of the modes at the bottom in its place.
  Both windows collapse with their title's arrow; the menu grows down to the caption and scrolls below it (at the
  tests' 960 x 540, by two lines since the navigation section and one more each since the weather and the particles
  sections). The Graphics
  window opens between the menu and the section's window (over the middle where there is no room), closed by
  default, with a close button in its title.
- **Compact** (a safe area under 40 x 26 em: phones in either orientation, small windows): a bar at the top (Menu,
  previous, the section's title, next, Controls) and one sheet at a time: the menu on the left (the mode buttons
  above the list of sections since the particles section, so that a phone in landscape reaches them without
  scrolling); the section's panel
  docked at the bottom in portrait and on the right in landscape, so the scene stays visible; About and Settings over
  the screen; Graphics (the menu sheet's button) docked as the section's panel is, at most half the screen's height
  in portrait and 24 em wide in landscape, each option's name over a control as wide as the sheet. A drag on a
  sheet's background scrolls it.
- The Graphics button and the "Graphics: G" hint are in scripted runs too (the menu's golden images show them); the
  Graphics window is hidden in the sections' captures with the demo's other windows (`windows off`). Scripted runs
  start without the panel's values (they read no `settings.json`); G and `sheet graphics` open it there.
- Both keep every window inside the safe area (the notch, the home indicator, rounded corners).
- Touch-sized controls (taller items, wider scroll bars and grabs, a touch margin) where the device has a touch
  screen (iOS), or always or never by the setting. Touches on a Mac trackpad are not a touch screen.

## Controls

| | Mouse and keyboard | Gamepad | Touch |
|---|---|---|---|
| Orbit | left drag; the arrows | right stick | one finger |
| Pan | right or middle drag | left stick | two fingers |
| Zoom | the wheel; + and - | the triggers | pinch |
| Act on the scene (blast, ping, fire, send a group) | a click | the section's button | a tap |
| Next, previous section | `]` `[`, Page Down, Page Up | the shoulder buttons | the arrows of the bar (compact) |
| A section | keys 1 to 9 for sections 1 to 9, 0 for section 10 (11 to 15 by next or previous); a click on the menu | | a tap on the menu |
| Menu by keys and buttons | Tab, then the arrows and Enter; Escape goes back | the D-pad, then A; B goes back | |
| Hide the windows | H | Start | Hide in the menu |
| The Graphics panel | G (again: close); Graphics in the menu | | Graphics in the menu sheet (compact) |

The camera's keys and sticks work while the scene has the keyboard and the gamepad: Tab or the D-pad hand them to
the windows (ImGui navigation, focus on the current section), Escape, B or a click on the scene hand them back. The
input section's character takes W A S D, the left stick and the fingers, so there only the mouse orbits and pans
(every device still zooms); a section's own keys are in its description. In the tunnel the camera drives a loop
by default: any move of the camera (orbit, pan or zoom on any device, Reset camera) hands it to the free camera, and
the panel's Camera: Drive takes it back. The same list is in About, under Controls.

## Graphics panel

The render options of `RenderSettings` ([Render module](render.md#settings)) and the display's options, for every
section at once: a value set here replaces that option in every section until it is given back. At the top a line
with fps, frame, CPU and GPU time (as the Performance panel; hidden in deterministic runs), the section and its
count of fallbacks, "Use section defaults" (gives every option back) and how many are set. The groups:

| Group | Options (keys in `settings.json`) |
|---|---|
| Anti-aliasing and resolution | anti-aliasing `aa` (none, FXAA, TAA), sharpen `sharpen` (0 to 1), render scale `scale` (0.25 to 1), upscaling `upscaling` (bilinear, MetalFX spatial, MetalFX temporal, MetalFX denoising) |
| Sun shadows | `sun_shadows` (shadow maps, ray traced), `cascades` (0 to 4), map size `shadow_size` (512 to 4096), `shadow_distance` (m), filter `shadow_filter` (1, 4, 12 taps), translucent shadows `translucent_shadows` (transmittance map, dithered), shadow rays `shadow_rays` (1 to 8), softness `shadow_softness` (times the sun's angular radius) |
| Ambient occlusion | `ao` (none, ray traced, screen space: GTAO), rays `ao_rays` (1 to 8; GTAO's slices), radius `ao_radius` (m), strength `ao_strength` (0 to 1), screen-space fallbacks `screen_fallbacks` (on, off: GTAO where the occlusion cannot be traced, SSR where the reflections cannot; `RenderSettings::screenSpaceFallbacks`) |
| Ray-traced lighting | lamp shadows `local_shadows` (0 to 4 lights), their shadow maps where they are not traced `local_shadow_maps` (off, 256, 512, 1024 texels; `RenderSettings::localShadowMapSize`), reflections `reflections` (sky, ray traced, screen: SSR), their roughness limit `reflection_roughness`, indirect light `indirect` (sky, probes, ray traced), probe spacing `probe_spacing` (m), probe updates `probe_updates` (1, 2, 4, 8: every probe per frame, 1/2, 1/4, 1/8 of them; `RenderSettings::probeUpdatePeriod`), probe cascades `probe_cascades` (1 to 4), probes moved out of geometry `probe_relocation` and probes in the open left out `probe_classification` (on, off), denoiser `denoiser` (spatial, temporal), path tracer `path_tracer` (on, off) and its bounces `path_bounces` (1 to 16) |
| Bloom, tone and sky | bloom `bloom` (on, off), `bloom_strength`, `bloom_threshold`, tone curve `tone` (ACES, AgX), `exposure` (a factor on each section's own exposure, 0.25 to 4), `vignette` (0 to 1), `dither` (on, off), `sky` (on, off) |
| Display | HDR output (off, auto, on) and the present mode (vsync, immediate, mailbox): the same controls and values as in Settings, with the output in use (HDR, headroom, paper white) and a line when a present mode is not there for the window |
| GPU backend | the backend, driver and device with ray tracing, MetalFX and GPU timing, why there is no ray tracing where there is none; on the Mac the relaunch with the other backend (as in About) |

Every control shows the value in use: the panel's where it sets the option (its name in blue, with a Reset button
whose tooltip gives the section's value), else the section's. Under an option a line says what runs instead after
the device's fallbacks and why, from `Renderer::effects()` and the reasons of `Renderer::fallbacks()` (in yellow:
"-> FXAA: ...", "-> shadow maps: ...", "-> bilinear: ...; the native Metal backend has it on Apple GPUs"), or how
the option works now (in gray: sharpen only after TAA without MetalFX, MetalFX temporal in TAA's place, the 3D
resolution of the render scale, spatial upscaling only below render scale 1, 0 cascades turn the ray-traced shadows
off too, map size and filter only for shadow maps, rays and softness only for ray-traced shadows, bloom off at
strength 0, no dither in HDR output, MetalFX denoising in place of TAA and the engine's denoiser, the path tracer in
place of the real-time frame).

Options the panel does not set keep each section's values, and a section still changes them (the shadows section's
turning sun, the sky section's day). Where a section changes an option the panel sets, the panel's value is the one
the renderer gets; the section's panel says so under Controls ("Set in Graphics for every section: anti-aliasing,
cascades. The controls here show those values and change them there.", with "Open Graphics"), and its controls and
"Frame and fallbacks" show and change the panel's value. Reset section keeps the panel's values. The check mode
and the benchmark run without them (each item and each measurement uses the section's own settings; the panel says
so and its controls are off) and they come back when the mode ends. The values are kept in `settings.json` (below);
Defaults in Settings gives every one back. Adapt to heat ([Settings](#about-and-settings)) applies on top of what
the panel and the section give, also in the check mode and the benchmark; while it limits something, a line under
the panel's title says what, and the controls still show the values set.

## Modes

The menu's buttons or a flag start them; one runs at a time, and its window at the bottom shows where it is.

- **Tour** (`--tour [SECONDS]`, default 8; the Tour button takes the Tour time of the settings): every section in turn for
  SECONDS each, round and round, from the current one. Any key, click, touch, button or stick stops it.
- **Check mode** (`--checks`): the checks of the devlog that wait for a person ("Checks waiting for the user"), 52
  items, one at a time: it switches to the item's section, sets it up (a camera, a setting) and says what to look at
  or listen to. OK, Problem or Skip, with a note, marks the item and goes to the next; the arrows go back and
  forward, End stops. Every mark is written to `checks.json` in the user directory at once, with the backend and the
  time; the next run starts at the first item not marked yet, with the earlier marks (a file that cannot be read
  starts empty, with a warning). The items: the fence's holes and the glass's tinted shadow (materials), the
  cascades (shadows), the day and fog (sky), anti-aliasing (post), HDR on an HDR display (hdr), the bodies
  (physics), the devices (input), loading and streaming and the Basis Universal tiles (assets), panning, Doppler, the tone without clicks, the
  music's crossfade, the buses and the pings, the reverb, the HRTF in headphones (audio, by ear), feet that do not slide, footsteps in sync, layers and
  look-at, foot placement, the crowd, the 2D blend space, animation LOD, dual quaternion skinning, the retargeted walk and the blend shapes (animation), noise, MetalFX ghosting and the fallback, and since stage 11
  the reflections, the bounced light, the lamp shadows, MetalFX denoising in motion and the path tracer against the
  real-time frame (raytracing), since stage 12 the area lights and their shadows, the brushed aluminium, the
  puddle and the lamps and the camera in motion (tunnel), since 2026-11-07 the probe cascades, relocation and
  classification in motion (`raytracing.probes`), the lamps' shadow maps on SDL GPU (`tunnel.maps`) and Adapt to heat (`tunnel.heat`: on a
  phone, the tunnel's defaults in motion for 10 minutes with Adapt to heat on; the Performance panel shows the
  thermal state and what is reduced; frames should stay smooth, with a lower render scale from serious on), the
  crowd and the navmesh in motion (`navigation.crowd`: the groups' rounds over the bridge, around the mud, down the
  stairs and the jumps, avoidance without touching or jitter, feet that do not slide, a tap sending a group), the
  clouds in motion (`weather.clouds`: the cumulus drifting and changing shape without popping, grain or smearing,
  their shadows moving over the plain, the lake reflecting them, the raining cumulonimbus in a blue sky, Sunset and
  Overcast), the rain in motion (`weather.rain`: the streaks slanting with the wind, splashes, rings on the puddles
  and the pond, the dry shelter, the rainbow and shafts toward the cloud, the ground drying after a passing shower)
  and by ear (`weather.rain_sound`: light and heavy rain without clicks or an audible loop, the roof's drumming, the
  distant rumble as a shower passes, silence when off), since 2026-11-08 the snow in motion (`weather.snow`: flakes
  fluttering and drifting without streaking or popping, none under the shelter, the cover building in patches on
  the paving, the roof and the crate tops and not on their sides, quiet), the plants in the wind
  (`weather.wind`: the grass, bushes and trees bending downwind and swaying as gusts cross the meadow, without
  jitter, smearing or seams between the grass's patches; the rain slanting and swirling; lightning lighting the
  clouds and the meadow) and by ear (`weather.wind_sound`: the wind rising and falling with the gusts without an
  audible loop, thunder a second or more after a near flash and duller from a far one, no clipping over the rain),
  the particles and trails in motion (`particles.motion`: smooth ribbons bright at the head and fading toward the
  tail, the sparkles they shed, the twinkling field, the fireflies, the campfire's streaking sparks and smoke
  downwind, no flicker, popping, smearing or hard edges at the ground; the CPU simulation the same), the frame rate
  and the layout on the device. The user looked at the Navigation and Weather sections in motion and
  listened to the weather on the Mac and on the iPhone 17 on 2026-11-08 ("yes, everything is fine"), a report on
  the sections as a whole; their items stay in the list for a look item by item.
- **Benchmark** (`--benchmark-all [SECONDS]`, default 5; the Benchmark button: 5 s): every section measured in turn
  as `scenarios/benchmark.txt` measures it: a warm-up of a second (or SECONDS, if shorter) and until the section's
  assets are in (at most 6 s), then a perf region of SECONDS in real time (`App::beginPerf` .. `endPerf`). One report
  for all of them, `benchmark.json` in the user directory; `--benchmark-all` quits when it is written (exit code 1
  if it could not be). Use Release, vsync and a fixed window size for numbers that compare
  ([Performance](performance.md)).

### Flags

| Flag | |
|---|---|
| `--section ID` | start in a section: `materials`, `shadows`, `sky`, `post`, `hdr`, `physics`, `input`, `assets`, `audio`, `animation`, `raytracing`, `tunnel`, `navigation`, `weather` (default: the last one, from the settings); an unknown id fails |
| `--tour [SECONDS]` | the tour, SECONDS per section (0.1 to 600) |
| `--checks` | the check mode |
| `--benchmark-all [SECONDS]` | the benchmark of every section (0.1 to 120 s each), then quit |
| `--no-settings` | neither read nor write `settings.json` |
| `--settings-file NAME` | the settings file in the user directory (default `settings.json`); in a scripted run the only way to use one |
| `--checks-file NAME` | the check results (default `checks.json`) |
| `--benchmark-file NAME` | the benchmark report (default `benchmark.json`) |

`--tour`, `--checks` and `--benchmark-all` exclude each other. The engine's flags (`--help`,
[App shell](app.md#command-line)) work as everywhere: for the assets section also `--asset-latency SECONDS`,
`--hot-reload` and `--worker-threads N`, for the audio section `--audio-device NAME`, `--no-audio` and
`--audio-capture FILE.wav`; `--gpu-backend sdl` for SDL GPU (the native Metal backend is the default on Apple
platforms since 2026-11-07; `--gpu-backend metal` names it), `--user-dir DIR` for another user
directory, `--orientation portrait|landscape` on a phone.

### Files

All three are JSON with `"version": 1`, in the user directory (`~/Library/Application Support/Quench/Quench Demo/`
on the Mac, `Library/Application Support/Quench/Quench Demo/` in the app's data container on iOS) through
`quench::fs`.

- `settings.json`: `section` (the last one), `show_ui`, `ui_scale` (0.5 to 3), `touch_controls` (`auto`, `on`,
  `off`), `hdr` (`off`, `auto`, `on`), `present_mode` (`vsync`, `immediate`, `mailbox`), `tour_seconds` (1 to 120),
  `adapt_to_heat` (a boolean, off; since 2026-11-07, still version 1: a file without it loads with it off),
  `graphics` (the Graphics panel's options: an object with only the options it sets, under the keys of the table
  above; the choices as names, `aa` `none`, `fxaa`, `taa`; `upscaling` `bilinear`, `spatial`, `temporal`;
  `sun_shadows` `maps`, `rt`; `translucent_shadows` `map`, `dithered`; `ao` `none`, `rt`; `tone` `aces`, `agx`;
  `bloom`, `dither` and `sky` as booleans, the rest as numbers; for example `"graphics": {"aa": "fxaa", "cascades":
  2, "exposure": 1.5}`). Still version 1: a file without `graphics` (all earlier ones) loads with nothing set, an
  unknown key or a bad value is skipped, a number is kept in the option's range, and an older demo ignores the key.
  Written half a second after a change and at the end. A file that cannot be read or parsed, or of another version,
  gives the defaults and a warning, and the next save writes a good one. `--hdr` and `--present-mode` on the command
  line win over the file. Scripted runs (`--script`) ignore the file unless `--settings-file` names one, so checks
  start from the defaults (no Graphics options) and leave the user's file alone.
- `checks.json`: `app`, `app_version`, `build` (the build type and the commit), `backend`, `driver`, `device`,
  `platform`, `written` (UTC); `items`, one per check: `id`, `section`, `title`, `status` (`pending`, `ok`,
  `problem`, `skipped`), `note`, `backend` and `time` of the mark; `summary`: the count per status.
- `benchmark.json`: the same run information, `seconds`, `window` (pixels); `sections`, one per section: `section`,
  `frames`, `frame_avg_ms`, `frame_p50_ms`, `frame_p99_ms`, `frame_max_ms`, `cpu_avg_ms`, `cpu_p50_ms`,
  `cpu_p99_ms`, `process_cpu_ms` and `cpu_performance_share` (the process's CPU time per frame and its percentage
  on performance cores; -1 where unknown), `gpu_avg_ms`, `gpu_p50_ms`, `gpu_samples`, `pass_p50_ms` (the median per timed GPU range, native
  Metal backend), `thermal_start`, `thermal_end`, `thermal_max`, `thermal_reaction_start`, `thermal_reaction_end`,
  `thermal_reaction_max` (`off` while Adapt to heat is off), `details` (the renderer's state, as on a `perf:` line).

## About and settings

About (the menu's button) shows the version, the build type and commit, the platform, the GPU backend, driver and
device with ray tracing, MetalFX and GPU timing, the window and its pixel density, the display's headroom, the audio
device and the job threads; the controls; the engine's licence and the third-party licences (shipped as assets,
`quench_cook_assets(... FILES ...)`, read when About first opens). On the Mac it has a button that starts the demo
again with the other GPU backend (`App::relaunch`: the same command line with another `--gpu-backend`; the settings
are saved first; the same buttons are in the Graphics panel; the default backend's button says so). About names the
platform's default and says when a run is not on it (chosen at launch, or the native backend did not start and SDL
GPU took its place; the log says why). iOS cannot start an app again from inside: the backend is chosen at launch,
the native Metal backend by default, SDL GPU with `--gpu-backend sdl` or the environment variable
`QUENCH_GPU_BACKEND=sdl` (below). The settings store no backend (also before 2026-11-07), so nothing is migrated:
the backend follows the default unless the command line or the environment names one.

Settings: the UI scale (applied when the slider is let go), touch-sized controls (auto, on, off), HDR output, the
present mode (one piece of code with the Graphics panel's Display group), the tour's time, Adapt to heat, Defaults
(also gives back every option of the Graphics panel and turns Adapt to heat off), and Screenshot
(`screenshot-N.png` in the user directory).

Adapt to heat (off) turns on the engine's thermal reaction with its defaults ([App shell](app.md#thermal-state)):
from the device's thermal state serious on, at most 60 Hz and 0.8 of the render scale; at critical 30 Hz, 0.6, and
the traced reflections and indirect light fall back to the sky's while the lamp shadows and the shadow and
occlusion rays go down to one; lifted after 60 s at a lower state. The limits apply on top of each section's
settings and the Graphics panel's options, which stay as they are (the panel says so in a line while limits apply),
and never raise the frame rate above the one set. The Performance panel shows the thermal state and, below it, what
the reaction reduces and how long the device has been cooler. On a Mac the state is nominal unless the machine is
hot; `--thermal-state serious` shows the reaction anywhere.

## Check mode on the iPhone

Build and install the demo as in [App shell](app.md#ios-runs) (bundle id `org.quench.demo` with the paid team's
empty suffix; the phone unlocked), then either open the menu and tap Checks, or start it with the flag:

```sh
cmake --preset host-tools && cmake --build --preset host-tools
cmake --preset ios-xcode -DQUENCH_IOS_TEAM=<team id> -DQUENCH_IOS_BUNDLE_ID_SUFFIX=
xcodebuild -project build/ios-xcode/Quench.xcodeproj -scheme quench_demo -configuration Release \
    -destination id=<device udid> -allowProvisioningUpdates build
DEVICECTL=/Library/Developer/PrivateFrameworks/CoreDevice.framework/Versions/A/Resources/bin/devicectl
$DEVICECTL device install app --device <udid> build/ios-xcode/examples/demo/Release-iphoneos/quench-demo.app
# the native Metal backend, the default (ray tracing, MetalFX, GPU times per pass):
$DEVICECTL device process launch --device <udid> --console --terminate-existing org.quench.demo --checks
# SDL GPU for the fallback items:
$DEVICECTL device process launch --device <udid> --console --terminate-existing \
    --environment-variables '{"QUENCH_GPU_BACKEND":"sdl"}' org.quench.demo --checks
# the results back to the Mac:
$DEVICECTL device copy from --device <udid> --domain-type appDataContainer --domain-identifier org.quench.demo \
    --source "Library/Application Support/Quench/Quench Demo/checks.json" --destination /tmp/checks.json
```

The marks survive between runs, so the native Metal items and the SDL GPU items can be done in two launches. On the
Mac: `build/release/examples/demo/quench-demo --checks` (and `--gpu-backend sdl`); the file is in
`~/Library/Application Support/Quench/Quench Demo/`.

## Content

The assets section's content is procedural (`tools/assets/make_demo_assets.py`, CC0, `assets_src/CREDITS.md`):
`examples/demo/assets_src` (glTF, GLB, PNG; the GLB embeds a JPEG) is cooked at build time by
`quench_cook_assets(quench_demo_cooked ...)` and merged into the build's `assets/` next to the executable
(`quench_add_game(... COOKED_ASSETS quench_demo_cooked)`, target `quench_demo_assets`), and
`examples/demo/assets/models/broken.qmodel` is a deliberately corrupt file. With hot reload on, editing a file in
`assets_src` and building `quench_demo_assets` while the demo runs re-cooks that file, and the demo loads it again;
on an iPhone, a development build of the demo loads it once `tools/push_assets.sh` has copied it
([Hot reload on a device](assets.md#hot-reload-on-a-device); `scenarios/hot_reload_device.txt` waits for that).

The audio section's sounds are not in git: `tools/assets/make_demo_sounds.py` (Python standard library, procedural,
CC0, `assets/CREDITS.md`) writes `sounds/motor.wav`, `siren.wav`, `ping.wav` (short loops and a one-shot, 24 and
48 kHz mono), `music_a.wav` and `music_b.wav` (6 s stereo loops at 22050 Hz: a drone with A minor eighths, and one
with D major triplets) into the build tree at build time, and `quench_cook_assets(quench_demo_sounds ... SOUNDS ...)`
checks and copies them into `assets/`; `sounds/step.wav` (a short footstep, stage 8) is one more. Building the demo
therefore needs Python 3.

The animation section's character is not in git either: `tools/assets/make_demo_character.py` (Python standard
library, procedural, CC0) writes `characters/mannequin.gltf` and `.bin` (one skin of 20 joints, 884 vertices, clips
idle, walk, run, jump, wave, lean, walk_back, strafe_l and strafe_r, the legs solved by IK from foot paths so
planted feet stay put) and
`mannequin.anim.json` (loops, the additive lean, footstep and landing events, root motion on `root`) at build time;
`quench_cook_assets(quench_demo_character ... MODELS ... SKELETONS ... ANIMATIONS ...)` cooks the model, the
skeleton and the clips. The graphs, `examples/demo/assets/characters/mannequin.graph.json` and
`mannequin_2d.graph.json` (the steered character's blend space), are in git. The section has
its own asset manager on the app's job system (one `SceneAssets` per manager, [Jobs, assets and the
cook](assets.md#textures-and-models-in-a-scene)) and its own `AnimationSystem`; it restarts on `set restart 1`,
which makes its capture independent of when the loads finished.

The tunnel's textures are not in git either: `tools/assets/make_demo_tunnel.py` (Python standard library,
procedural, own content) writes ten PNG files into the build tree at build time (about 7 s): the wall panels'
color, normal and metallic-roughness maps (512 x 512, 2 x 2 panels of 1.5 m with seams, rivets, a vent and a raised
plate), the asphalt's (stones in a dark binder, wet patches darker and smoother), the puddle (an outline in alpha),
the embossed plastic's and the rubber mat's normal maps (diamonds, coins) and the brushed aluminium's
metallic-roughness streaks. `quench_cook_assets(quench_demo_tunnel ... TEXTURES ... DATA_TEXTURES ...)` cooks them
into KTX2 (colors in sRGB, the maps linear). The section has its own asset manager and `SceneAssets`, as the
animation section does.

The navigation section's level is made in code (`sections/NavigationSection.cpp`: boxes, a ramp and steps, the same
triangles for the navmesh and for drawing) and built into a navmesh at run time; its characters are the animation
section's mannequin, loaded by its own asset manager and `AnimationSystem`. It restarts on `set restart 1` (the
obstacles at their clock's start, the groups at their first waypoints), with a camera cut when the agents appear, so
its capture does not depend on how long the build took.

The weather section's scene is made in code (`sections/WeatherSection.cpp`: a checkered plain, spheres for hills,
boxes for towers, a smooth plane for the lake) and its clouds come from a `quench_weather` field with a fixed seed
and a cell; its scripts set the weather's clock (`set weather_time`) and stop it (`set time_scale 0`), so its
captures do not depend on when the part ran. The noise of the clouds is procedural, drawn by the GPU. The rain's
place on the ground (paving with puddles, a pond, a shelter of posts and a roof, two crates, a dark block), where
the snow lies too, is boxes and planes made in code; the rain's sounds are the engine's defaults (`quench_weather_sounds(quench_demo_weather_sounds)`,
procedural, [Audio](audio.md#weather-sounds)).

The licences About shows are the engine's `LICENSE` and `THIRD_PARTY_LICENSES`, copied into `assets/` by
`quench_cook_assets(quench_demo_licences ... FILES ...)`; the commit it shows comes from `BuildInfo.cmake`, which
writes `demo_build_info.h` at build time only when the commit changes ("unknown" without git; uncommitted changes
are not marked).

## Checks

The demo's checks run in three batches ([App shell](app.md#captures-and-golden-images)): one offscreen app process
per GPU backend, `--window 960x540 --gpu-debug --hdr-headroom 4`, runs every check as a part of one scenario.

| Batch | Scenario | Backend |
|---|---|---|
| `demo_batch_metal` | `batch.txt` | SDL GPU's Metal driver (`--gpu-backend sdl`), with the Metal API validation layer |
| `demo_batch_metal_native` | `batch_native.txt`: `batch.txt` with `raytracing.txt`, `tunnel.txt`, `tunnel_path.txt` and `tunnel_motion.txt` in place of `raytracing_fallback.txt`, `tunnel_fallback.txt` and `tunnel_motion_fallback.txt`, with `cycle.txt` before `tunnel_motion.txt` | the native Metal backend (`--gpu-backend metal`; the default on Apple platforms), with the validation layer; compared with the Metal golden images except ray tracing's own |
| `demo_batch_vulkan` | `batch.txt` | Vulkan through MoltenVK (`--gpu-driver vulkan`, which selects SDL GPU) with the Khronos validation layer (an error or warning fails the batch) |

Each batch fails when the app ran on another backend or driver than its own (a fallback from the native backend
included), from the log's `gpu: backend ...` line.

`batch.txt` includes, in this order: materials, shadows, sky, post, hdr, hdr_on, physics, input, assets, audio,
animation, raytracing_fallback, tunnel_fallback, ui, compact_portrait, compact_landscape, menu, audio_tone,
audio_flyby, audio_music, audio_reverb, audio_hrtf, tunnel_motion_fallback, animation_blend2d, animation_lod, animation_skinning,
animation_retarget, animation_blendshapes, raytracing_maps, raytracing_screen, tunnel_maps, heat, assets_basis,
navigation, weather, rain, snow, wind, physics_wind, particles, cycle.
The parts that move (the tunnel's motion) and the parts added later come last, before the cycle, which has no
capture: TAA's jitter follows the frame count, so a part inserted earlier would move every capture after it.
`batch_native.txt` ends with animation_blend2d, animation_lod, animation_skinning, animation_retarget,
animation_blendshapes, heat, raytracing_mirror, raytracing_probes, assets_basis, navigation, weather, rain, snow, wind, physics_wind and particles after tunnel_motion (no
raytracing_maps, raytracing_screen or tunnel_maps: the native backend traces those shadows and the occlusion on the
Mac; the render tests run GTAO there).
Every part selects its section first (and resets it where it needs the defaults), so it does not depend on the parts
before it, and its `time` starts at 0. A test per part and backend, `demo_<part>_<backend>`, checks the part's line
in the batch's log (`part <file>: N passed, 0 failed`) and compares its capture (or captures:
`animation_skinning` has two) with `tests/golden/<backend>/<device>/<golden>.png`. The batches run in Release builds (`QUENCH_TEST_CAPTURES`); Debug
and the sanitizer builds run smoke runs instead (`smoke.txt`: every section once, offscreen, with assertions or
under the sanitizers): on Apple platforms `demo_smoke_metal` on SDL GPU and `demo_smoke_metal_native` on the native
backend (since 2026-11-07; it fails when the native backend falls back), elsewhere `demo_smoke` on the default.
The scenarios point at the window in percent or at named points (`@menu:<section>`,
`@floor`), so their checks also pass in a portrait phone window (the captures are compared on the Mac only).

The section captures show the scene only: before each, the part sends `windows off`, which hides the menu, the
section's panel, the caption, the hints and the top bar, and `windows on` after it. A section's own on-screen
elements stay where they are what is checked. The menu and the layouts have captures of their own.

| Part | What it checks | Capture |
|---|---|---|
| `materials`, `shadows`, `sky`, `post`, `hdr`, `physics` | the section's options switched one by one, the renderer's and the section's values, the start state put back | `demo_<part>.png` at 2 s: the scene |
| `hdr_on` | `hdr on`: the output is HDR extended linear with headroom 4 and the capture's peak is above 2 and at most 4 times paper white; `hdr off` returns to SDR | `demo_hdr_on.png` at 2 s (clipped at paper white): the scene |
| `input` | keyboard, gamepad and touch actions, the virtual stick | `demo_input.png` at 3 s: the scene (the touch overlay, fingers and stick, is drawn only while a finger is down) |
| `assets` | below | `demo_assets.png` at 1.5 s: the scene with the section's overlay (each model's load state, the streaming rings, the reload note), which is what the section shows |
| `audio` | every emitter, the tone, the crossfade to music B and a burst of 40 pings, no underruns or clipped samples (the mix runs offline on the frame clock, as in every offscreen run) | `demo_audio.png` at 1.65 s: the scene |
| `animation` | below | `demo_animation.png` at 2.4 s: the scene |
| `animation_blend2d` | the steered character (crowd 0, 9 characters): by itself its root motion goes ahead and then to its left within 15 degrees of the asked velocity; set directly, backward at -0.9 m/s and to its right at -0.75 m/s within 10 degrees, inside its ring (17 checks) | `demo_animation_blend2d.png` at 3.5 s after the restart, strafing to its left: the scene |
| `animation_lod` | the crowd of 256 (265 characters) with animation LOD: from the hero's view more than 150 at every 2nd frame and fewer than 180 updated per frame, interpolated poses change every frame (no reused skins), held ones let the renderer reuse more than 80 skins; from 45 m more than 200 at every 4th frame and fewer than 90 updated; off again, all 265 every frame (18 checks) | none |
| `animation_skinning` | the skinning showcase (crowd 0, 9 characters, all bound, twist 170 degrees); the checkbox switched to dual quaternions and back (11 checks); its waits end half a frame off the fixed clock's steps | two captures about 1 s after the restart: `demo_animation_skinning.png` linear (both bars and both forearms pinched, labels "Linear (reference)" and "Linear") and `demo_animation_skinning_dq.png` dual quaternion (the right bar and forearm round, the bar's squares in a spiral, its label "Dual quaternion") |
| `animation_retarget` | the retargeting pair (crowd 0, 9 characters with the blob, all bound) after 248 frames: within 1 degree of each other on their circles, the strider's hips 1.14 to 1.16 times as high as the mannequin's pelvis (10 checks); its waits end half a frame off the fixed clock's steps, so that they take the same number of frames wherever the batch's app time is | `demo_animation_retarget.png`: the two walking toward the camera in step, the hero's stage out of view |
| `animation_blendshapes` | the blob (crowd 0, 9 characters, all bound): its model has 4 blend shapes, the weights set by name; after the capture its clip's weights: squash above 0.3 while stretch is 0, then stretch above 0.25 while squash is 0, puff and spikes 0, a slider added to the clip's weight (19 checks); its waits end half a frame off the fixed clock's steps | `demo_animation_blendshapes.png` at 1 s after the restart: the blob swaying, puff 0.7, spikes 1, squash 0.3 (the sliders; the clip's weights off) |
| `raytracing` (native batch) | the section's defaults run: ray-traced sun shadows and occlusion, 2 lamp shadows, reflections, the probe grid (2048 probes), the temporal denoiser, MetalFX temporal at scale 0.67 in TAA's place (no cascades, no shadow draws, at least 19 instances in the top level, the character skinned, a material texture in the hit arrays, no fallbacks); MetalFX denoising runs when asked; shadow maps come back when asked; MetalFX temporal at scale 0.5 resolves in TAA's place, spatial runs; per-pixel indirect light runs; the path tracer adds samples in TAA's place; bilinear at scale 1 again | `demo_raytracing_native.png` at 2.5 s (every traced effect at full size with the engine's temporal denoiser, the sun and the character moving): the scene |
| `raytracing_probes` (native batch, before assets_basis; since 2026-11-07) | the probe grid's cascades, relocation and classification ([Render](render.md#ray-traced-lighting)): 1 m probes in 3 cascades (`probe_spacing 1`, `probe_cascades 3`, 6144 probes traced), moved out of geometry and classified, bilinear at scale 1, no fallbacks; the section's own grid again after the capture (2048 probes) | `demo_raytracing_probes_native.png` at 2.5 s from the overview (`reset_camera`): the ground lit by the grid out to the horizon (in `demo_raytracing_native.png`, with one 2 m grid, the ground beyond 16 m has the sky's ambient light: the grey band behind the scene) |
| `raytracing_mirror` (native batch; since 2026-11-07) | the checker ground's own function in the ray hits (`quench_add_ray_hits`, [Render](render.md#ray-hits-of-surfaces)): traced reflections, one instance whose hits run their surface's function, bilinear at scale 1, no fallbacks; the camera close to the mirror (`camera 25 22 3.4 -2.2 1.0 -3.6`) | `demo_raytracing_mirror_native.png`: the mirror showing the ground's cells and the sun's shadows on them as the ground itself (before, the material's flat mean color) |
| `raytracing_fallback` (Metal, Vulkan) | every request falls back (cascades, no occlusion, no top level, unshadowed lamps, the sky's reflections and light, no probes, bilinear for MetalFX denoising and spatial at scale 0.5, the real-time frame for the path tracer), and the renderer lists the fallbacks (at least 5, then 6) | `demo_raytracing.png` at 2.5 s: the scene |
| `raytracing_maps` (Metal, Vulkan) | the lamps' shadow maps at 512 (Graphics panel, "Shadow maps"): no traced lamp shadows, 2 lamps with 7 maps (the point lamp 6, the spot lamp 1) and draws in them, the sun's cascades, no top level; off again, no maps | `demo_raytracing_maps.png` at 2.5 s: the scene, the block's shadow from the point lamp on the green wall and the fence's from the spot lamp |
| `raytracing_screen` (Metal, Vulkan) | the screen-space fallbacks (Graphics panel, "Screen-space fallbacks") with the section's traced occlusion and reflections asked for: GTAO and SSR run after a prepass of their own (depth and G-buffer), no top level, the sun's cascades; off again, no occlusion, the sky's reflections | `demo_raytracing_screen.png` at 2.5 s: the scene with GTAO's contact shadows under the block, the crate, the spheres and along the walls, the gold spheres' lower halves reflecting the floor (since 2026-11-07, SSR) |
| `tunnel` (native batch) | the section's defaults run: 10 lamps, 8 of them area lights, 4 lamp shadows, reflections, the probe grid, MetalFX temporal at 0.67, no shadow draws (no sun), at least 46 instances in the top level, no fallbacks; lamp groups off and on change the counts; the anisotropy changes; the path tracer adds samples; bilinear at scale 1 again | `demo_tunnel_native.png` at 2.5 s (every traced effect at full size, the lamps at their time 0, standing still, the camera free): the scene |
| `tunnel_path` (native batch) | the path tracer runs to 256 samples with the lamps still, then stops | `demo_tunnel_path_native.png`, without a golden image (1.3 MB of noise); look at it beside `demo_tunnel_native.png` |
| `tunnel_fallback` (Metal, Vulkan) | the 8 area lights light without shadows, the sky's reflections and light, no top level, bilinear at scale 1, the path tracer falls back (at least 3 fallbacks); the tubes and the portal off and on | `demo_tunnel.png` at 2.5 s: the scene |
| `tunnel_maps` (Metal, Vulkan) | the lamps' shadow maps at 512 (the panel's "Lamp shadow maps", since 2026-11-07): no traced lamp shadows, 4 lamps with 21 maps (the two floor strips and the ceiling square 5 each, the orange tube 6) and draws in them, the screen pass's prepass, no top level; off again, no maps, no prepass | `demo_tunnel_maps.png` at 2.5 s: the scene as `demo_tunnel.png` with soft lamp shadows (the crates and the plinth on the floor, the bay's pillars cutting the orange tube's light on the near right wall, the strips' light on the walls) |
| `tunnel_motion` (native batch, the last part) | the drive starts eased (its speed rises over a second), the lamps' clock runs, the path tracer stops both and they go on after it, Pause all stops both, a script's camera hands the drive to the free camera | `demo_tunnel_motion_native.png`: 3 s of the drive and the lamps from their time 0 |
| `tunnel_motion_fallback` (Metal, Vulkan, the last part) | the same without ray tracing: the path tracer falls back, so nothing stops for it | none |
| `ui` | the wide layout with its windows | `demo_ui.png`: the materials section with the menu, its panel and the caption |
| `compact_portrait`, `compact_landscape` | `compact.txt` in a phone-sized frame (400 x 720 and 720 x 320 points): the compact layout, the bar's arrows, the menu sheet, About and its licences, the controls sheet | `demo_compact_portrait.png`, `demo_compact_landscape.png`: the controls sheet over the materials section |
| `menu` | switching sections by keys, gamepad, a click and a tap on the list; the menu by Tab, the arrows and Enter and by the D-pad, A and B; hiding the windows | none |
| `audio_tone`, `audio_flyby`, `audio_music`, `audio_reverb`, `audio_hrtf` | the checks on the mix itself (below) | none |
| `heat` (every batch, before assets_basis and the cycle; on the native backend before raytracing_mirror) | Adapt to heat in the tunnel through `thermal_state` (bilinear at scale 1, so the scale is the reaction's): off, serious changes nothing; on, serious gives 60 Hz and 0.8 of the render scale, critical 30 Hz, 0.6 and the effects reduced (on the native backend 1 lamp shadow of 4, the sky's reflections and light); a frame rate set at 24 stays 24; with a hold of 1 s (`set heat_recover 1`) fair and then nominal lift the limits in two steps, 4 changes in all; unknown counts as nominal; off lifts them at once; the system's state again at the end (48 checks) | none |
| `assets_basis` (every batch, after heat; on the native backend after raytracing_probes) | the tiles switched to the Basis Universal ETC1S set, then the UASTC set: at least 4 tile textures ready from Basis files each time, none decoded to RGBA8 (BC7 or ASTC on SDL GPU, Metal and Vulkan through MoltenVK; ETC2 for ETC1S on the native backend), still only the corrupt and the missing model failed; the platform's set again (18 checks) | none |
| `navigation` (every batch, before the cycle on SDL GPU, the last part on the native backend; since 2026-11-07) | the navmesh built (at least 12 tiles, 40 polygons, 3 obstacles), 12 agents, none lost; the probe path around the mud at cost 5 (no meter in it, longer than 10.5 m) and through it at cost 1 (more than 6 m in it); no agent step in the mud at cost 5; the debug overlays drawn; a click under the bridge sends the orange group down a jump link (a link used, a landing) and all 6 arrive; the moving obstacles rebuild their tiles (3 revisions or more) with no agent lost; agents on a floor at least 0.5 m apart (26 checks) | `demo_navigation.png` 4 s after the restart, the obstacles still: the orange group in single file on the bridge, the green one south of the mud with their paths around it, the navmesh, the holes around the crates and the barrel and the jump arcs (the same image on SDL GPU's Metal driver and the native backend; Vulkan through MoltenVK within 2 pixels) |
| `weather` (every batch, before the rain; since 2026-11-07) | the section's defaults with the weather's clock at 1200 s and stopped: volumetric clouds at half size (480 x 270), one cell raining (above 0.5 under it), none at the camera, the rain more than 3 km away, the cover below 0.6; the 2D layer when asked; the overcast preset closes the cover (above 0.8) with volumetric clouds (11 checks) | three captures: `demo_weather.png` (fair weather: cumulus with grey bases and bright tops, the raining cumulonimbus on the right, their shadows on the plain, the lake reflecting the clouds), `demo_weather_sunset.png` (towering cumulus at a sun 3 degrees up: orange edges, dark bases) and `demo_weather_layer.png` (the 2D layer: flat patches and their shadows); since part 2 with the rain's shafts (under the cumulonimbus, a band of distant showers at sunset, under the layer's cloud); the same images on SDL GPU's Metal driver and the native backend, Vulkan through MoltenVK within a mean of 0.002 |
| `rain` (every batch, before the snow; since 2026-11-07) | the Rain here preset from the ground with the weather's clock at 4000 s and stopped, the rain at Medium with shafts, rainbow and sound, the section restarted and its time waited for (so the streaks and rings do not depend on when the part ran): the rain at the camera above 0.3, 24576 streaks, 1536 splashes, more than 8 objects in the rain map, the cover below 0.1, the ground soaked (above 0.9), the light and heavy layers above 0.2, the mix's RMS above 0.03; under the shelter the cover above 0.8, the roof's layer above 0.4 and the open-air ones below 0.1; the shower off and 4 minutes of weather: wetness 0.5 to 0.85, puddles 0.5 to 1; a passing shower 0.8 to 2.5 km away heard from afar (above 0.2, RMS above 0.003); the rain off with the cell still raining: no streaks or shafts, the sound still on; no underruns or clipping (44 checks) | three captures: `demo_rain.png` (the streaks against the dark block and toward the sun, puddles and rings on the paving, the shelter's dry floor and crate), `demo_rain_rainbow.png` (toward the cloud: the rainbow, the streaks, the dry crate under the roof and the wet one in the open) and `demo_rain_shelter.png` (from under the roof: the dry floor, the rain past its edge, the puddles beyond); the same images on SDL GPU's Metal driver and the native backend, Vulkan through MoltenVK within a mean of 0.04 |
| `snow` (every batch, before the wind; since 2026-11-08) | the Snowfall preset from the ground at -4 degrees, the weather's clock at 4000 s and stopped, the rain and the snow at Medium with shafts and sound: all of the precipitation snow, the snowfall at the camera above 0.4 and no rain there, 16384 flakes, the cover full (above 0.99) and the ground dry, the cover over the camera below 0.1, no rain layer playing; under the shelter the cover above 0.8 and no roof's layer; over the shelter from bare dry ground (the thaw's past at 8000 s), five minutes of the snowfall in 5 s at 60 times the weather's speed: the cover 0.2 to 0.32, the ground still dry; a minute of the thaw at 6 degrees: the cover 0.12 to 0.24, wetness above 0.1, standing water above 0.02, no snowfall; the snow showers from the air with their shafts; the snow off: no flakes, no snowfall drawn, still no rain; no underruns or clipping (35 checks) | four captures: `demo_snow.png` (the flakes against the dark block, the paving, grass and the crate in the open white, the floor and the crate under the shelter bare), `demo_snow_building.png` (from over the shelter: the first patches on the paving, the roof and the crate tops, none on their sides), `demo_snow_thaw.png` (patches left on wet, darker paving) and `demo_snow_sky.png` (white curtains under the snow showers over the plain); the same images on SDL GPU's Metal driver and the native backend, Vulkan through MoltenVK within a mean of 0.006 |
| `wind` (every batch, before physics_wind; since 2026-11-08) | the meadow in the Weather section with the weather's clock stopped: 79 plants; in a calm (fair weather, wind 0) no wind at 10 m, no lightning, the wind's layers silent; the Windy storm preset at 4000 s: gusts 0.6 and turbulence 0.4, the mean wind at 10 m 10 to 11 m/s, a lull of its gusts here (8 to 9.5), no flash, the rain at the camera above 0.9, the heavy rain's layer above 0.9, both wind layers playing; two seconds later a gust (above 10.2 m/s, the strong layer above 0.4); the next flash within 3 km (`set flash 1`): the lightning's light above 0.5, one flash lit, its thunder waiting and played within 12 s; eight seconds of the storm at six times its lightning: at least 3 thunders played over the rain and the gale; the lightning off: no light, no flash; no underruns or clipping (31 checks) | four captures: `demo_wind_calm.png` (the meadow upright under a low sun), `demo_wind.png` (the grass and the crowns bent downwind, the rain slanting under the storm), `demo_wind_gust.png` (two seconds later, bent by other amounts) and `demo_wind_flash.png` (5 ms after a flash: the meadow and the rain lit); the same images on SDL GPU's Metal driver and the native backend, Vulkan through MoltenVK within a mean of 0.03 |
| `particles` (every batch, after physics_wind: before the cycle on SDL GPU, the last part on the native backend; since 2026-11-08) | the section's dusk scene restarted, in a wind of 3 m/s, after 4 s: the GPU simulation, 11 systems and 20 trails in the scene and drawn, more than 1500 strip vertices, more than 5000 particles (more than 200 stream sparkles, 3500 in the field, 40 fireflies, 50 sparks, 20 puffs of smoke); the CPU simulation a second after the switch (more than 1500 particles); a burst of sparks (more than 120 more); density 0.25 (the field below 1600); the streams off (no trails, 5 systems) and on again; the simulation off (nothing drawn) (26 checks) | two captures: `demo_particles.png` (the blue-hour sky with the gold, blue and rose ribbons, their heads, sparkles and dust, the twinkling field, the campfire on the left) and `demo_particles_fire.png` (half a second after the view "Fire": the flames, streaking sparks, smoke and the field close up, the streams above); the same images on SDL GPU's Metal driver and the native backend, Vulkan through MoltenVK within a mean of 0.0008 |
| `physics_wind` (every batch, before the cycle on SDL GPU, the last part on the native backend; since 2026-11-08) | the Physics section's wind: three light balls put down upwind stay without wind; at a wind of 20 m/s at 10 m the wind at the floor 3 to 7 m/s (4.8); three seconds later the balls more than 0.4 m downwind of where they lay (they roll about 2.7 m); no ImGui ID shared by two items in any frame of the batch so far (`imgui_id_conflicts`, since 2026-11-08) (9 checks) | none |
| `cycle` (every batch, the last part on SDL GPU) | every section three times (created or reset, switched, reset), with the live GPU resources, the scene's instances and the error count marked after the first round and the memory after the second: no growth (the GPU resources within 3, fences and transfer buffers come and go; memory below 64 MB in the third round; no instance more; no new error); at its end no ImGui ID shared by two items in any frame of the run (`imgui_id_conflicts`, [App](app.md#scenarios); since 2026-11-08, when the Weather section's Cirrus preset and slider shared one). The memory is marked a round later since stage 11: after the first round alone it swung between -24 and +47 MB on the native backend, where the ray tracing section's filter histories come and go. The bound is 64 MB since the tunnel moves (2026-11-06): over 20 rounds the footprint moved within about 60 MB from round to round, with the tunnel's motion and without it, with no trend and the same GPU resources | none |

The cycle runs on Vulkan through MoltenVK since 2026-11-06, when the growth there was found in SDL GPU's Vulkan
driver (a released fence could keep its command buffer and the textures it used in flight; the GPU layer works
around it, [Platforms](platforms.md)). Over 12 rounds of resets with the validation layer the footprint grew from
918 to 1088 MB, by less each round (from +50 to +4 MB), with the same GPU resources; SDL GPU's Metal driver stayed
within 671 to 695 MB, the native backend within 957 to 1015 MB. Its first round resets every section, so the
settings the parts before it changed do not count as growth.

Every section has a clock of its own (`Section::sectionTime`), which runs only while the section is shown: its
animations and the surface shaders' time follow it, so a capture does not depend on how long the batch ran before
the part. The renderer's own state still does in part: the native ray tracing capture from a run of its own and
from the batch differ by a mean of 0.32 (0.09% of the pixels by more than 16), within the thresholds. Two batch runs
of the same binary give the same bytes in every capture (48 captures, 2026-11-06).

Outside the batches, with runs of their own because they need flags or files of their own:

| CTest test | What it runs |
|---|---|
| `demo_tour` | `--tour 0.1` with `scenarios/tour.txt`: a full round back to the first section, every section initialized, a key stops it, `mode tour` starts it again from the current section |
| `demo_checks`, `demo_settings`, `demo_benchmark` | `tests/DemoUserFiles.cmake` runs the demo on a fresh `--user-dir` and reads the file it wrote: the check mode's marks by command and by the buttons, read back by a second run, a corrupt `checks.json` starts empty (`checks*.txt`); settings saved, read back, a corrupt file gives the defaults and is written again, `--no-settings` leaves it (`settings_*.txt`); `mode benchmark 0.1` writes a report with 15 sections (`benchmark_all.txt`) |
| `demo_version`, `demo_bad_section`, `demo_bad_flag` | `--version` prints "Quench Demo <version>"; an unknown `--section` fails; `--tour` with `--checks` fails |

The batches write no WAV file: `demo_audio_wav_match`, which compared the WAV files of the Metal and the Vulkan audio
check, is gone. The audio part's checks run on all three backends.

The assets check (`scenarios/assets.txt`, about 3.6 s per backend) loads with a simulated latency of 0.2 s and checks
the placeholders first (loads in flight, no model ready), then waits for the manager to be idle: three models ready,
the corrupt and the missing one failed (two failures in all), more than four streamed cells. It reloads every asset
(at least six reloads, the failures stay failures), lowers the texture cache to 4 KB, turns the focus to the other
side of the grid and waits for cells to unload and the cache to evict. It runs with FXAA, not TAA: loads finish in
real time, and with TAA the history of the frames before a load arrived made two runs differ.

The animation check (`scenarios/animation.txt`, 21 checks) waits for the character to load, restarts the section at
0.5 s of app time with a crowd of 24 at 1.6 m/s (33 characters with the steered one, the skinning and retargeting pairs and the blob, all bound), triggers a jump (the state changes, the
landing event comes, the state returns to the blend), checks the footstep events, the foot placement (both legs of
the ramp stand reach, the step stand's raised leg is grounded about 0.2 m up and the other on the floor, the ramp
stand's pelvis moves down), that the torch stays in the hand (`held_distance`) and the look-at, then waves and is
captured at 2.4 s. It runs with TAA like the others: the section restarts at a fixed time, so the capture does not
depend on load timing (two runs gave the same image on each backend).

The audio mix checks are five parts of the batches: `audio_tone.txt` (23 checks, about 1.7 s of app
time), `audio_flyby.txt` (11 checks, 2.2 s), `audio_music.txt` (13 checks, 3.9 s), `audio_reverb.txt` (16
checks, 4.7 s) and `audio_hrtf.txt` (26 checks, 1.5 s). Each turns the orbit, the
fly-by and the music off, waits for the sounds and re-syncs to a fixed app time (loads finish in real time), measures
the mix with `audio_measure` ([App shell](app.md#scenarios)) and ends with no underruns or clipped samples. The tone:
1 kHz at 2 m in front (RMS 0.058 to 0.067, 998 to 1002 Hz), moved to the right (left channel below 0.002), at 8 m
with the inverse, linear and no attenuation, gliding to 1500 Hz, then a burst of 40 pings steals voices. The fly-by's
fundamental approaching (477 to 486 Hz from 440 Hz at 30 m/s) and receding (401 to 410 Hz). Music A (110 Hz), the
crossfade to B (two streams, then one, 147 Hz), the music bus at 0 (silent). The reverb: the tone stops and the mix
is silent after its fade without the reverb; with it at room size 0.8 the tone measures 0.068 to 0.080 RMS (0.0733),
its tail after the fade above 0.008 (0.0111) and a second later 0.0004 to 0.0015 (0.0006); at room size 0.2 the tail
is under 0.0001 a second after the fade; off again, silence. The HRTF: a 1 kHz tone 2 m to the left, panned (left
0.084 to 0.093 RMS, right under 0.0001), through the HRTF (left 0.105 to 0.125, right 0.047 to 0.059: 0.1146 and
0.0529, one HRTF voice), moved to the right (left 0.049 to 0.061, right 0.100 to 0.120), then off again (panned
hard right, no HRTF voice). Every test runs offscreen, where the mix is offline, so
no test plays through the speakers.

Named points: `menu:<section id>` (a menu entry while the menu is shown), `ui:<name>` (the windows' buttons:
`ui:sections`, `ui:prev`, `ui:title`, `ui:next`, `ui:controls`, `ui:close`, `ui:tour`, `ui:checks`,
`ui:benchmark`, `ui:about`, `ui:settings`, `ui:hide`, `ui:job_test`, `ui:reset_section`, `ui:reset_camera`,
`ui:relaunch`, `ui:stop`, `ui:check_ok`, `ui:check_problem`, `ui:check_skip`, `ui:check_prev`, `ui:check_next`,
`ui:check_end`, `ui:check_note`, `ui:graphics`, `ui:graphics_defaults`), and the
section's (`floor`, ...).

Offscreen frames are not paced ([App shell](app.md#command-line-flags)), except while assets load: the three
batches together take about 9 s (2026-11-06, in parallel; each 7 to 9 s for 290 to 310 checks). On the screen
(`--onscreen`) a scripted run holds every frame to 120 per second, and with vsync also to the display.

The scenarios turn dithering off (`set dither 0`), which keeps the golden PNGs at 120 to 380 KB. Deterministic runs
(`--script`, `--capture`; not inside a perf region) hide the timings, the memory and the Log panel's counts, which change from run to run.
Game values a scenario can read: `section`, `sections`, `ui`, `aa` (0 none, 1 FXAA, 2 TAA, after fallbacks), `cascades`, `bloom`,
`draws`, `instances`, `culled`, `shadow_draws`, `shadow_instances`, `transmittance_draws`, `translucent_shadows` (0
transmittance map, 1 dithered, after fallbacks), `moved`, `lights`, `render_w`, `render_h`,
`camera_yaw`, `camera_pitch`, `camera_distance`, `clicks`, `skinned_instances`, `skinned_vertices`, `sun_shadows`
(0 maps, 1 ray traced), `ambient_occlusion` (0 none, 1 ray traced, 2 screen space), `screen_occlusion` (GTAO ran),
`screen_prepass_draws`, `reflections` (0 sky, 1 ray traced, 2 screen space), `screen_reflections` (SSR ran), `upscaling` (0 bilinear, 1 spatial, 2 temporal),
`render_scale`, `ray_instances`, `ray_builds`, `ray_refits`, `ray_hit_surfaces` (instances whose hits run
their surface's function) (all after fallbacks), `fallbacks` (their count),
`camera_x`, `camera_z` (the target); the demo's own: `section_ready`, `sections_ready`, `init_ms`, `switch_ms`,
`scene_instances`, `compact`, `sheet` (0 none, 1 the menu, 2 the controls, 3 About, 4 Settings, 5 Graphics), `menu_focus`,
`about`, `licence_bytes`, `ui_scale`, `touch_ui`, `settings_loaded` (1 read, 0 not, -1 unusable), `settings_writes`,
`mode` (0 none, 1 tour, 2 checks, 3 benchmark), `tours`, `check_index`, `check_items`, `check_writes`,
`checks_pending`, `checks_ok`, `checks_problem`, `checks_skipped`, `benchmark_sections`, `benchmark_written`,
`log_errors`, `log_warnings`, `job_test_ms` (-1 until a job test finished), `adapt_heat` (the setting),
`heat_scale` (the render scale the renderer gets, after the reaction), `heat_changed_at` (the app time of the
reaction's last change; -1 before one); then the
section's own (for example `bodies`, `blasts`,
`player_x`, `jumps`, `marker_draws`, `exposure`; the assets section's `assets_loading`, `assets_ready`,
`assets_failed`, `assets_idle`, `assets_loads`, `assets_reloads`, `assets_evictions`, `showcase_ready`,
`showcase_failed`, `streaming_active`, `streaming_loads`, `streaming_unloads`, `tile_set` (0 the platform's, 1 Basis ETC1S, 2 Basis UASTC; `set tile_set platform|etc1s|uastc`), `tiles_basis` and `tiles_decoded` (ready tile textures from a Basis file, decoded to RGBA8 on load); the audio section's `sounds_ready`,
`music_track`, `flyby_x`, `flyby_pitch`, `pings`, `reverb`, `room_size`, `hrtf`; the engine's `audio_*` values; the animation section's
`anim_ready`, `anim_failed`, `anim_characters`, `anim_bound`, `anim_ground_rays`, `hero_state` (0 the blend, 1 the
jump), `hero_transition`, `hero_look_at`, `hero_angle`, `footsteps`, `landings`, `crowd`, `held_distance`,
`slope_pelvis`, and per stand and leg `slope_leg0_offset` (also `_reached`, `_grounded`; `slope` or `step`, leg 0
or 1), `view`, the steered character's `steer_vx`, `steer_vz` (asked), `strafer_vx`, `strafer_vz` (moved, over
0.2 s), `strafer_heading_error` (degrees) and `strafer_radius`, animation LOD's `anim_evaluated` (characters
updated in the last update), `anim_lod_rate1`, `anim_lod_rate2`, `anim_lod_rate4` (characters at each rate) and
`skinned_reused` (the renderer's reused skins, of the frame before), `dual_quaternion` (the checkbox's skinning
of the showcase's right pair and the other characters: 1 dual quaternion, 0 linear), `twist` (the showcase's, in degrees), the retargeting pair's `pair_gap` (degrees on their circles) and
`pair_height_ratio`, the blob's `blob_shapes` (its model's blend shapes), `blob_weight_<shape>` (the weight it is drawn with), `blob_clip_<shape>` (its clip's value) and `blob_clip`; the ray tracing section's `rt_ready`, `sun_azimuth`; the weather section's `clouds` (0 off, 1 the 2D layer, 2 volumetric, after fallbacks), `cloud_width`, `cloud_height`, `cloud_cover`, `sky_cube_faces`, `coverage`, `cloud_type`, `cells`, `weather_time`, `sun_elevation`, `limit_60hz`, `rain_here`, `rain_distance`, `shower_rain`, `view` (0 the air, 1 the ground, 2 toward the cloud, 3 the shelter, 4 over the shelter, 5 the meadow), `rain` (the level drawn, after fallbacks), `rain_at_camera`, `rain_cover`, `rain_streaks`, `rain_splashes`, `rain_occluders`, `rain_shafts`, `rainbow`, `wetness`, `standing_water`, `snow` (the level drawn), `snow_at_camera`, `snow_share`, `snow_flakes`, `snow_cover`, `temperature`, `sound_light`, `sound_heavy`, `sound_far`, `sound_cover`, `sound_wind_light`, `sound_wind_strong` (the layers' gains), `sound_playing`, `gusts`, `turbulence`, `wind_here` and `mean_wind_here` (m/s at 10 m above the camera's ground, with the gusts and without), `lightning` (its light at the camera), `lightning_flashes`, `strikes_heard` (flashes passed to the sound), `thunder_played`, `thunder_waiting`, `plants`; the physics section's `wind`, `wind_at_floor`, `light_balls` and `light_ball_x` (their mean x)). Game commands: `section ID`, `reset_section`, `reset_camera`,
`sheet none|sections|controls|about|settings|graphics`, `mode off|tour|checks|benchmark [SECONDS]`,
`check ok|problem|skip [NOTE]`, `check next|prev`, `job_test`, `settings_save` (now, not half a second later),
`settings_corrupt` (overwrites the settings file with text that is not JSON),
`camera YAW PITCH DISTANCE [X Y Z]` (degrees, meters; the point it orbits, else the section's), `set NAME VALUE` (`ui_scale`, `touch_controls auto|on|off`,
`adapt_heat on|off`, `heat_recover SECONDS` (the reaction's hold for this run, not saved); render settings: `aa none|fxaa|taa`,
`tone aces|agx`, `cascades`, `shadow_size`, `shadow_filter`, `shadow_distance`, `translucent_shadows map|dithered`,
`sun_shadows maps|rt`, `shadow_rays`, `shadow_softness`, `ao none|rt`, `ao_rays`, `ao_radius`,
`upscaling bilinear|spatial|temporal`, `scale`, `bloom`, `bloom_strength`,
`sky`, `dither`, `sharpen`, `vignette`; otherwise the section's options, for the physics section `wind` (m/s at 10 m),
`gusts`, `light_balls N` (drops N light balls upwind); for the assets section `latency SECONDS`,
`texture_budget BYTES|unlimited`, `upload_budget BYTES|unlimited`, `focus_angle DEGREES`, `move_focus`, `reload`,
`models`, `hot_reload`; for the audio section `orbit`, `flyby` (also restarts the pass), `tone`, `tone_hz`,
`tone_x`, `tone_z`, `orbit_speed`, `flyby_speed`, `attenuation none|inverse|linear|exponential`, `min_distance`,
`max_distance`, `doppler`, `music 0|1|2`, `bus_<name>` (volume), `mute_<name>`, `pause_effects`, `ping`,
`burst N`, `reverb`, `room_size`, `hrtf`; for the animation section `restart`, `speed`, `lean`, `jump`, `wave`, `foot_ik`, `look_at`,
`footstep_sound`, `slope DEGREES`, `crowd N`, `lod`, `lod_interpolate`, `dual_quaternion`, `twist DEGREES`, `view hero|blend2d|skinning|retarget|blendshapes`, `steer_auto`, `steer_vx`, `steer_vz`, `blob_squash|stretch|puff|spikes WEIGHT`, `blob_clip`; for the ray tracing section `turn_sun`, `walk`, `spin` (the torus turns; since 2026-11-07),
`sun_azimuth`, `sun_elevation`; for the weather section `preset fair|shower|towering|storm|overcast|cirrus|rain_here|downpour|passing|snowfall|thaw|snow_showers|windy_storm`,
`view sky|ground|cloud|shelter|above|meadow`, `rain off|low|medium|high`, `snow off|low|medium|high`, `temperature`, `rain_shafts`, `rainbow`, `sound`, `gusts`, `turbulence`, `lightning`, `lightning_rate`, `flash` (the next flash within 3 km: the weather's time set to 5 ms after it),
`quality full|reduced|layer|off`, `coverage`, `cloud_type`, `type_variation`, `precipitation`, `cirrus`, `wind_speed`,
`wind_from` (degrees), `evolution`, `seed`, `shower`, `time_scale`, `weather_time`, `sun_elevation`, `sun_azimuth`,
`run_day`, `steps`, `cloud_shadows`, `sky_cube`, `cube_faces`, `limit_60hz`).

## Benchmark

`scenarios/benchmark.txt` measures the materials, shadows, sky, post-processing, HDR (in SDR, then with HDR output on),
physics, assets, audio, animation, ray tracing, tunnel, navigation, weather and particles sections for 5 s each in real time (budgets
and numbers: [Performance](performance.md)); the assets region streams with the moving focus and the default
budgets, after the first loads have finished; the audio region plays the three emitters, the tone and the music; the
animation region has a crowd of 256 (264 characters), and its perf line ends with the section's own times over its
last 240 frames: `anim_update_ms` (mean) and `anim_update_max_ms` of `AnimationSystem::update`, `anim_threads`,
`anim_lod` (off, interpolate or hold), `anim_evaluated` (the mean of the characters updated per frame), and
`skinning_gpu_ms`, the mean of the skinning pass's GPU time (-1 without GPU timing); the navigation region (since
2026-11-07, not run on the screen yet) has two groups of 12 with the obstacles moving, and its perf line ends with
`nav_agents`, `nav_crowd_ms` (the crowd step's moving average) and `nav_polygons`; the weather region (since
2026-11-07, not run on the screen yet) has fair weather at 10 times its speed with the section's quality, and its
perf line ends with `clouds`, `steps`, `cube_faces` and `limit_60hz`; the particles region (since 2026-11-08, not
run on the screen yet) has the section's dusk scene restarted, and its perf line ends with `particles`, `trails`,
`trail_vertices` and `simulation`. It runs in a window on the
screen (`--onscreen`: scripted runs are offscreen otherwise, where frames are not presented) with `--audio-device
null` (the mix runs on miniaudio's null device thread, without sound):

```sh
build/release/examples/demo/quench-demo --onscreen --frame-size 1920x1080 \
    --script examples/demo/scenarios/benchmark.txt --audio-device null
```

The benchmark mode (`--benchmark-all`, above) measures every section the same way, with the windows as they are
(shown by default, as in the scenario), and writes the numbers as JSON; the scenario stays the reference for comparisons in the devlog, with its own regions (HDR on,
the crowd of 256).

`scenarios/raytracing_benchmark.txt` measures the ray tracing section's options one by one, 4 s each after a second
to settle (since stage 11; 5 s before), at render scale 1 with TAA unless named: rays for shadows and occlusion
(`rt_shadows_ao`), shadows alone, hard shadows (1 ray), shadow maps; then one stage 11 effect after another on top
of shadows and occlusion: lamp shadows, reflections, the probe grid, per-pixel indirect light; everything traced
with the temporal denoiser (`all_traced`), with the spatial one, with MetalFX temporal at 0.67, with MetalFX
denoising at 0.67 and 0.5; MetalFX temporal at 1, 0.67 and 0.5, spatial and bilinear at 0.5 over shadows and
occlusion; and the path tracer. Since stage 12 it goes on to the tunnel at full size: the lamps unshadowed
(`tunnel_lit`), with four traced shadows, with reflections, with the probe grid (`tunnel_all_traced`), the section's
default at 0.67 with MetalFX temporal, all with the lamps still at their time 0 and the camera free (comparable with
the runs from before they moved), then the default with the lamps moving (`tunnel_lamps_moving`) and with the camera
driving too (`tunnel_moving`), and the path tracer. Run it with `--onscreen` on the native backend (the default on
Apple platforms, or `--gpu-backend metal`; on SDL GPU every region measures the fallbacks); the perf lines carry the
GPU time per timed range
([Performance](performance.md#measurements)). For the
cost of each effect add `--present-mode immediate`: with vsync the GPU's clock follows the load, and light regions
read slower per range than they are (the stage 11 costs in [Performance](performance.md) come from such a run).

`scenarios/weather_cost.txt` (since 2026-11-07) measures the weather section's quality settings offscreen (clouds
off, the 2D layer, half and full size in fair weather and in a storm, the phone defaults; 3 s each after a second),
at any size: the GPU time per range on the native backend ([Weather](weather.md#cost)); `scenarios/rain_cost.txt` the
rain's levels the same way (from the ground and from the air, and the phone defaults); `scenarios/snow_cost.txt` the
snow's (a snowfall from the ground, snow showers from the air, and the phone defaults); `scenarios/wind_cost.txt` the
wind's (the meadow still and swaying, the rain and the snow with and without gusts, the phone defaults);
`scenarios/particles_cost.txt` (since 2026-11-08) the particles section's (off, on the GPU and the CPU, density 0.5
and 0.25, the streams, the field and the campfire alone; [Particles](particles.md#cost)).

On a phone the same costs come from `scenarios/raytracing_phone.txt` (each effect at the device's size, then the
render scales and upscalers with everything and with less, 4 s each; every group of regions waits for the nominal
thermal state first, at 20 frames per second on the cheapest frame), and `scenarios/raytracing_phone_sustained.txt`
holds the section's phone defaults for a minute at the display's rate and for a minute at 60 Hz;
`scenarios/raytracing_phone_probes.txt` adds the probe grid to the phone defaults, traced every frame and in 2, 4
and 8 turns, classified every frame and in 4 turns, 1 m probes in two cascades moved and classified in 4 turns
(since 2026-11-07), then holds it in 4 turns and classified for 20 s at 60 Hz (not run on a phone yet). The noise of
the per-pixel indirect light (since 2026-11-07, offscreen on the Mac, no thresholds;
[Performance](performance.md#the-denoiser-in-a-still-scene)): `scenarios/raytracing_noise.txt` holds the phone
defaults with it still and captures two frames in a row after 60, 120, 300 and 600 frames, then the same at full
size, bilinear at half size, with MetalFX temporal and denoising, with the probe grid alone and with the spatial
denoiser; `scenarios/raytracing_noise_motion.txt` captures it in motion (the camera and the sun stepped as at 60 Hz)
and the same instant settled, then after the sun jumps by 60 degrees. For the tunnel,
`scenarios/tunnel_phone.txt` measures each option at half size (and the lamps at 0.67 and full size), then holds the
section's phone defaults at 60 Hz for 40 s with the lamps still, then 20 s in motion (`tunnel_moving_60hz`), and
captures them and the path tracer; `tunnel_phone_fallback.txt` runs the fallback frame on SDL GPU, still and in
motion. The scenarios on SDL GPU (this one and the three below) need `--gpu-backend sdl` (or
`QUENCH_GPU_BACKEND=sdl`) since the native backend became the default. `scenarios/lamp_maps_phone.txt` measures the
lamps' shadow maps on SDL GPU (the ray tracing section's two
lamps and the tunnel's four nearest lamps, area lights among them, unshadowed and with maps of 256 and 512, the
tunnel's two nearest at 256, then those two at 60 Hz for 20 s; not run on a phone yet). `scenarios/gtao_phone.txt` measures GTAO on SDL GPU (the shadows section, the ray
tracing section and the tunnel without occlusion and with it, and a capture; not run on a phone yet).
`scenarios/ssr_phone.txt` measures SSR on SDL GPU (the materials section, the ray tracing section and the tunnel
with the sky's reflection and with SSR, and a capture `ssr_phone.png`; not run on a phone yet).
`scenarios/phone_sustained.txt` holds the ray tracing section's phone defaults for five minutes and then the
tunnel's, in motion, for five more, at 60 Hz in 20 s regions from the nominal thermal state. Copy the file into the
app's user directory and launch it with `devicectl`
([App shell](app.md#ios-runs)); the perf lines carry the thermal state of each region.

## Packaged builds: `tools/package_demo.sh`

```sh
tools/package_demo.sh          # macOS: dist/QuenchDemo-<version>-<hash>-macos.zip
tools/package_demo.sh --ios    # also iOS: dist/QuenchDemo-<version>-<hash>-ios.zip
```

`<hash>` is the short commit hash, with `-dirty` when the working tree has changes (the build is then not exactly
that commit). The script builds `quench_demo` with the `release` preset (`--preset NAME` for another), runs the
demo's tests (`ctest -L demo`: the batches and the runs with files of their own; `--no-checks` skips them when
`tools/check.sh release` has just run them on the same build), and then:

- **macOS**: assembles `QuenchDemo.app` (the executable, an Info.plist with the version and the binary's minimum
  macOS as `LSMinimumSystemVersion`, an icns made from `examples/demo/icon/icon.png`, `LICENSE` and
  `THIRD_PARTY_LICENSES` in `Contents/Resources`, the demo's cooked assets in `Contents/Resources/assets`, where
  SDL's base path of a bundled app points), checks that it links only system libraries, signs it ad hoc and
  zips it with `LICENSE` and `THIRD_PARTY_LICENSES` next to it. The archives carry no extended attributes: `ditto`
  would store them as AppleDouble `._` files, which a plain `unzip` turns into files inside the bundle that break its
  signature; the script fails on any `._` entry. It then unzips the archive with a plain `unzip` into a temporary
  directory, verifies the signature and starts the app from `/` with `scenarios/smoke.txt` (every section once,
  offscreen: no window on the screen, no sound): exit code 0, the scenario
  passed, and a capture (`dist/<name>-macos-smoke.png`) to look at. The app is signed ad hoc and not notarized: a
  copy downloaded on another Mac is quarantined, and Gatekeeper opens it only after it is allowed in System Settings,
  Privacy & Security (not tried on another Mac yet).
- **iOS** (`--ios [--team ID]`): builds `quench_demo` with the `ios-xcode` preset (Automatic signing with the team:
  `--team`, else `QUENCH_IOS_TEAM` from the environment, else the one `build/ios-xcode` is configured with; an Apple
  ID signed in to Xcode, which creates the development profile on its own), checks that the app's `assets/` matches
  the staged folder (`diff -r`; see [Assets](assets.md)), and packs the
  signed app as `QuenchDemo.ipa`, with the licences, into the zip. The team must be a paid one: the script refuses a free Personal
  Team (Xcode's account data marks it), whose profiles expire after 7 days, and checks that the app is signed by the
  team (`codesign`, `TeamIdentifier`), also after a plain `unzip` of the `.ipa`. It runs only on the devices in the
  profile, and the profile expires (the paid
  team's wildcard profile made on 2026-11-05 lasts until 2027-11-05, with 5 devices). The script prints the team, the
  signature, the profile's name, expiry and device count. Nothing is installed on a device by the script.

`dist/` is git-ignored.

App Store archives and submission checks are described in [Distributing the demo through the App Store](demo-app-store.md).
