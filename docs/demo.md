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
| 6 | `physics` | Jolt through `quench::PhysicsWorld` at 120 Hz, drawn interpolated: a stack of boxes and balls | drop a box (B, gamepad X), drop a ball (N, gamepad Y), blast (P, gamepad B, or a click or tap on the floor), reset (R, gamepad Back) |
| 7 | `input` | Named actions over every device, shown live: a character that walks, runs, jumps and fires | WASD, left stick or a finger on the left half: walk; Left Shift or the left stick button: run; J, gamepad A or a tap on the right half: jump; F, gamepad X, a click or a tap: fire |
| 8 | `assets` | Cooked assets loaded through the app's asset manager ([Jobs, assets and the cook](assets.md)): a row of models (a crate from a glTF with an external texture, a pillar from a GLB with embedded JPEG and PNG textures, a vertex-colored ball, a corrupt file and a missing one) shows the load states: gray boxes while loading, magenta on failure. Behind it a 6 x 5 grid of tiles, each with one of eight BC7 textures and a model, streams around a moving focus point | simulated latency, texture cache budget (unlimited, 64, 16, 4 KB), uploads per frame (8 MB, 16 KB, 1 byte), focus angle, moving focus, models on and off, hot reload, reload all; counts of ready, loading, failed, queued and in-flight assets, reloads, evictions, memory per type against its budget, streamed cells |
| 9 | `audio` | The mixer and 3D sound ([Audio](audio.md)): a head with ears at the listener, facing the way the camera looks; an orange emitter circling it at ear height (a motor loop), a blue fly-by passing at speed (a siren, with Doppler), a green box that plays a tone the demo computes (a generator); streamed music A and B with a crossfade; all sounds procedural | emitters on and off, orbit and fly-by speed, tone frequency and position, attenuation model, minimum and maximum distance, Doppler scale; music off, A or B; per bus volume, mute and a peak meter; pause effects; the reverb on or off (the 3D sounds send to a reverb bus under the effects) and its room size; the HRTF for headphones on or off (the panel then shows the HRTF voices); a ping, a burst of 40 pings (voice stealing), a click or tap on the ground plays a ping there; voices, stolen and rejected sounds, underruns, clipped samples, mix time and the output device |
| 10 | `animation` | Skeletal animation ([Animation](animation.md)) of a procedural mannequin: the hero walks a circle by root motion with a speed blend of idle, walk and run, jumps, waves (a layer masked to the right arm), leans (an additive layer), turns its head toward the camera (look-at), holds a torch on its left hand (an attachment) and plays a step sound at each footstep event (a louder one at the jump's landing); two characters stand with foot placement on a ramp and across a 0.2 m step (rays into a Jolt world); a crowd walks in rings; in the view "2D blend space", away from them, a character steered by its velocity through a 2D blend space of idle, walk, run, walk back and the two strafes walks ahead, sideways and backward inside a ring while it faces one way; in the view "Skinning", on the other side, two standing characters whose left forearms the section turns by 170 degrees about their length, skinned linearly (the elbow pinches to a thin neck) and by dual quaternions (it keeps its thickness); in the view "Retargeting", behind the hero's camera, the mannequin and a second character with other joint names, proportions and rest pose (the strider, its walk retargeted from the mannequin's at cook time) walk the same clip abreast, the camera following them; in the view "Blend shapes", behind the skinning pair (and drawn in that view only), a swaying jelly blob on three joints reshaped by four blend shapes (squash, stretch, puff, spikes) before its skinning, its clip squashing it where its sway turns and stretching it in between through their weights; animation LOD as an option (off by default): the characters small on the screen update every 2nd or 4th frame; dual quaternion skinning for the other characters as an option (off by default) | speed, lean, jump (J, gamepad A), wave (K, gamepad X), restart; foot IK, look-at and footstep sound on and off; ramp angle (0 to 30 degrees), crowd size (0 to 512); animation LOD on and off, interpolated or held; dual quaternion skinning on and off; the view (the hero's stage, the 2D blend space, the skinning pair, the retargeting pair or the blob); the blob's weights from its clip on and off, its four blend shape weights (added to the clip's) and a reset; the steered character by itself (around a circle) or by two velocity sliders; the velocity it is asked for and the one its clips move it by; the hero's state and transition, characters, joints, clip samples, ground rays, events, skinned instances, vertices and reused skins, the characters updated and at each rate, the update time and threads, the skinning GPU time |
| 11 | `raytracing` | Ray tracing where `gpu::Caps` allow it ([Render module](render.md#ray-traced-shadows-and-ambient-occlusion)), on the native Metal backend (`--gpu-backend metal`): soft ray-traced sun shadows through a fence's diamond holes (cut-out), tinted amber by a spinning glass torus (translucent) and cast by the animation section's mannequin walking a circle (skinned, refitted every frame), ambient occlusion under a crate and a column; since stage 11 ([Ray-traced lighting](render.md#ray-traced-lighting)) a framed mirror and five gold spheres from roughness 0.02 to 0.48 (traced reflections), a red and green corner with a white block (light bounced by the probe grid), a lamp in the corner and a spot behind the fence (lamp shadows, the spot throws the fence's holes forward), a warm emissive panel that lights the ground through the probes; MetalFX temporal upscaling with the engine's temporal denoiser at render scale 0.67 where the device has MetalFX (MetalFX denoising costs about three times as much on the M4 Pro and is a choice in the panel); on phones and tablets that trace (`App::mobile()`) render scale 0.5 with MetalFX spatial upscaling, no probe grid and 60 frames per second ([Performance](performance.md#stage-11-on-the-iphone)); MetalFX spatial upscaling, the engine's denoisers, and the path tracer as a reference (it stops the sun, the torus and the walk). On SDL GPU (Metal or Vulkan through MoltenVK) the same scene with the fallbacks and a line saying why; there the lamps can have shadow maps, the occlusion GTAO and the reflections SSR (options, off by default) | sun shadows (shadow maps or ray traced), shadow rays, softness, ambient occlusion with its rays and radius (and GTAO where it is not traced), lamp shadows (and their shadow maps where they are not traced), reflections
(and SSR where they are not traced), indirect light (sky, probes, rays), the denoiser, upscaling (bilinear, MetalFX spatial, temporal, denoising) and render scale, the path tracer, each with the fallback it got; turn the sun, walk, sun elevation, 60 Hz (on by default on phones and tablets; while the section is current); the top level's instances, builds, refits and compactions (or the shadow draws), the hit textures, probes and path samples, and outside deterministic runs the GPU time of the rays, denoising, probes, reflections, indirect light, the path tracer, the shadow maps, GTAO, MetalFX and the frame |
| 12 | `tunnel` | Area lights and anisotropic GGX ([Area lights](render.md#area-lights), stage 12): a 27 m tunnel at night lit only by its lamps, eight area lights (two cyan strips in the floor, two warm squares in the ceiling, a magenta panel on a wall, an orange and a violet neon tube, a round blue portal at the end) and two spots; metal wall panels with color, normal and metallic-roughness maps, wet asphalt with a near-mirror puddle, a rubber mat and wheel stops, embossed plastic crates, brushed aluminium (anisotropic: kick plates, ribs, the sphere). The textures are procedural, written at build time. The lamps move on a clock of their own: the violet tube rides two ceiling rails along the tunnel, the orange tube slides in its bay, the amber spot turns its cone, the white one circles a sphere and the portal breathes, their hangers, brackets and spot housings with them (instance transforms, refitted in the top level). The camera drives a loop through the tunnel by default, eased, looking down the tunnel; a drag, Reset camera or a script's camera hands it to the free camera, and everything stands still while the path tracer runs. Where `gpu::Caps` allow it (the native Metal backend) four lamps cast soft traced shadows, reflections are traced and the probe grid lights the rest, at render scale 0.67 with MetalFX temporal; on phones and tablets that trace half size with MetalFX spatial, two lamp shadows, no traced reflections and the sky's light; phones that do not trace at three quarters of the size; every phone at 60 frames per second ([Performance](performance.md#the-tunnel-on-the-iphone)); the path tracer as a reference. On SDL GPU the lamps light and shine (LTC) without shadows (the two spots with shadow maps if the Graphics panel's option is on) and reflect only the night sky | ray traced or path traced, lamp shadows, traced reflections, each lamp group on or off, the aluminium's anisotropy (0 to 1), the puddle, 60 Hz; Pause all, Lamps move, the camera's Drive or Free, Stop and Drive speed; the lamps and area lights in the frame and outside deterministic runs the GPU time of the shading, the rays, the denoiser, the reflections, the path tracer and the frame |

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
| Area lights, anisotropic GGX, normal and metallic-roughness maps | tunnel | rect, disk and tube lights with their drawn shapes (LTC on every backend), their soft traced shadows, the path tracer's area light sampling, brushed aluminium, cooked texture maps made at build time |
| The job system | the Performance panel | threads and pending jobs, a job test (256 items on the pool, its time and the items per thread) (added); physics, animation and loads run on it |
| Frame, CPU and GPU times | the Performance panel | fps, frame, CPU and GPU time, a frame-time graph (added), the GPU time per pass on the native Metal backend; hidden in deterministic runs (captures) |
| Memory and GPU resources | the Performance panel | the process's memory and the live GPU resources, the section's load and switch times (added) |
| GPU backend, device, fallbacks | every section's "Frame and fallbacks"; About | the backend, driver, device and capabilities, every fallback with its reason (added); a relaunch with the other backend on the Mac (added) |
| Display scale, safe area, orientation | the layouts | sizes from the display's scale, windows inside the safe area, the compact layout in portrait and landscape (added) |
| Logging | the Log panel | the demo's log lines with the warning and error counts (added) |
| User files through `quench::fs` | Settings, the check mode, the benchmark | `settings.json`, `checks.json`, `benchmark.json`, screenshots (added) |
| Scenarios, captures, perf regions | the checks | every section's check; the benchmark of every section on the same perf regions (added) |

Not shown, because the engine does not have it yet: Basis Universal textures ([Roadmap](roadmap.md)). Shader hot reload has no panel: with `--hot-reload` on the Mac, an edit of the
engine's or the demo's shaders shows in the running demo ([Shader hot reload](app.md#shader-hot-reload)).

## Layouts

Sizes follow the display's scale (the engine's ImGui style), times the UI scale of the settings.

- **Wide** (a desktop window, a tablet): the menu at the top left (the sections numbered 1 to 12, the mode buttons
  Tour, Checks and Benchmark, About, Settings, Graphics, Hide, the Performance and Log panels, and a hint of the keys
  under them: "Sections: keys 1-9, 0 = 10", "Previous, next: PgUp PgDn LB RB", "Menu: Tab, D-pad; back: Esc, B",
  "Hide: H, Start; Graphics: G"); the section's window at the top right, or under the menu where the window is too
  narrow for both; the description as a caption at the bottom; the windows of the modes at the bottom in its place.
  Both windows collapse with their title's arrow; the menu grows down to the caption and scrolls below it (at the
  tests' 960 x 540, by one line). The Graphics window opens between the menu and the section's
  window (over the middle where there is no room), closed by default, with a close button in its title.
- **Compact** (a safe area under 40 x 26 em: phones in either orientation, small windows): a bar at the top (Menu,
  previous, the section's title, next, Controls) and one sheet at a time: the menu on the left; the section's panel
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
| Act on the scene (blast, ping, fire) | a click | the section's button | a tap |
| Next, previous section | `]` `[`, Page Down, Page Up | the shoulder buttons | the arrows of the bar (compact) |
| A section | keys 1 to 9 for sections 1 to 9, 0 for section 10 (11 and 12 by next or previous); a click on the menu | | a tap on the menu |
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
| Ray-traced lighting | lamp shadows `local_shadows` (0 to 4 lights), their shadow maps where they are not traced `local_shadow_maps` (off, 256, 512, 1024 texels; `RenderSettings::localShadowMapSize`), reflections `reflections` (sky, ray traced, screen: SSR), their roughness limit `reflection_roughness`, indirect light `indirect` (sky, probes, ray traced), probe spacing `probe_spacing` (m), probe updates `probe_updates` (1, 2, 4, 8: every probe per frame, 1/2, 1/4, 1/8 of them; `RenderSettings::probeUpdatePeriod`), denoiser `denoiser` (spatial, temporal), path tracer `path_tracer` (on, off) and its bounces `path_bounces` (1 to 16) |
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
Defaults in Settings gives every one back.

## Modes

The menu's buttons or a flag start them; one runs at a time, and its window at the bottom shows where it is.

- **Tour** (`--tour [SECONDS]`, default 8; the Tour button takes the Tour time of the settings): every section in turn for
  SECONDS each, round and round, from the current one. Any key, click, touch, button or stick stops it.
- **Check mode** (`--checks`): the checks of the devlog that wait for a person ("Checks waiting for the user"), 40
  items, one at a time: it switches to the item's section, sets it up (a camera, a setting) and says what to look at
  or listen to. OK, Problem or Skip, with a note, marks the item and goes to the next; the arrows go back and
  forward, End stops. Every mark is written to `checks.json` in the user directory at once, with the backend and the
  time; the next run starts at the first item not marked yet, with the earlier marks (a file that cannot be read
  starts empty, with a warning). The items: the fence's holes and the glass's tinted shadow (materials), the
  cascades (shadows), the day and fog (sky), anti-aliasing (post), HDR on an HDR display (hdr), the bodies
  (physics), the devices (input), loading and streaming (assets), panning, Doppler, the tone without clicks, the
  music's crossfade, the buses and the pings, the reverb, the HRTF in headphones (audio, by ear), feet that do not slide, footsteps in sync, layers and
  look-at, foot placement, the crowd, the 2D blend space, animation LOD, dual quaternion skinning, the retargeted walk and the blend shapes (animation), noise, MetalFX ghosting and the fallback, and since stage 11
  the reflections, the bounced light, the lamp shadows, MetalFX denoising in motion and the path tracer against the
  real-time frame (raytracing), since stage 12 the area lights and their shadows, the brushed aluminium, the
  puddle and the lamps and the camera in motion (tunnel), the frame rate and the layout on the device.
- **Benchmark** (`--benchmark-all [SECONDS]`, default 5; the Benchmark button: 5 s): every section measured in turn
  as `scenarios/benchmark.txt` measures it: a warm-up of a second (or SECONDS, if shorter) and until the section's
  assets are in (at most 6 s), then a perf region of SECONDS in real time (`App::beginPerf` .. `endPerf`). One report
  for all of them, `benchmark.json` in the user directory; `--benchmark-all` quits when it is written (exit code 1
  if it could not be). Use Release, vsync and a fixed window size for numbers that compare
  ([Performance](performance.md)).

### Flags

| Flag | |
|---|---|
| `--section ID` | start in a section: `materials`, `shadows`, `sky`, `post`, `hdr`, `physics`, `input`, `assets`, `audio`, `animation`, `raytracing`, `tunnel` (default: the last one, from the settings); an unknown id fails |
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
`--audio-capture FILE.wav`; `--gpu-backend metal` for the native Metal backend, `--user-dir DIR` for another user
directory, `--orientation portrait|landscape` on a phone.

### Files

All three are JSON with `"version": 1`, in the user directory (`~/Library/Application Support/Quench/Quench Demo/`
on the Mac, `Library/Application Support/Quench/Quench Demo/` in the app's data container on iOS) through
`quench::fs`.

- `settings.json`: `section` (the last one), `show_ui`, `ui_scale` (0.5 to 3), `touch_controls` (`auto`, `on`,
  `off`), `hdr` (`off`, `auto`, `on`), `present_mode` (`vsync`, `immediate`, `mailbox`), `tour_seconds` (1 to 120),
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
  Metal backend), `details` (the renderer's state, as on a `perf:` line).

## About and settings

About (the menu's button) shows the version, the build type and commit, the platform, the GPU backend, driver and
device with ray tracing, MetalFX and GPU timing, the window and its pixel density, the display's headroom, the audio
device and the job threads; the controls; the engine's licence and the third-party licences (shipped as assets,
`quench_cook_assets(... FILES ...)`, read when About first opens). On the Mac it has a button that starts the demo
again with the other GPU backend (`App::relaunch`: the same command line with another `--gpu-backend`; the settings
are saved first; the same buttons are in the Graphics panel). iOS cannot start an app again from inside: the backend
is chosen at launch, with `--gpu-backend metal` or the environment variable `QUENCH_GPU_BACKEND=metal` (below).

Settings: the UI scale (applied when the slider is let go), touch-sized controls (auto, on, off), HDR output, the
present mode (one piece of code with the Graphics panel's Display group), the tour's time, Defaults (also gives
back every option of the Graphics panel), and Screenshot (`screenshot-N.png` in the user directory).

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
$DEVICECTL device process launch --device <udid> --console --terminate-existing org.quench.demo --checks
# the native Metal backend (ray tracing, MetalFX, GPU times per pass) for the ray tracing items:
$DEVICECTL device process launch --device <udid> --console --terminate-existing \
    --environment-variables '{"QUENCH_GPU_BACKEND":"metal"}' org.quench.demo --checks
# the results back to the Mac:
$DEVICECTL device copy from --device <udid> --domain-type appDataContainer --domain-identifier org.quench.demo \
    --source "Library/Application Support/Quench/Quench Demo/checks.json" --destination /tmp/checks.json
```

The marks survive between runs, so the SDL GPU items and the native Metal items can be done in two launches. On the
Mac: `build/release/examples/demo/quench-demo --checks` (and `--gpu-backend metal`); the file is in
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
library, procedural, CC0) writes `characters/mannequin.gltf` and `.bin` (one skin of 20 joints, 836 vertices, clips
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

The licences About shows are the engine's `LICENSE` and `THIRD_PARTY_LICENSES`, copied into `assets/` by
`quench_cook_assets(quench_demo_licences ... FILES ...)`; the commit it shows comes from `BuildInfo.cmake`, which
writes `demo_build_info.h` at build time only when the commit changes ("unknown" without git; uncommitted changes
are not marked).

## Checks

The demo's checks run in three batches ([App shell](app.md#captures-and-golden-images)): one offscreen app process
per GPU backend, `--window 960x540 --gpu-debug --hdr-headroom 4`, runs every check as a part of one scenario.

| Batch | Scenario | Backend |
|---|---|---|
| `demo_batch_metal` | `batch.txt` | SDL GPU's Metal driver, with the Metal API validation layer |
| `demo_batch_metal_native` | `batch_native.txt`: `batch.txt` with `raytracing.txt`, `tunnel.txt`, `tunnel_path.txt` and `tunnel_motion.txt` in place of `raytracing_fallback.txt`, `tunnel_fallback.txt` and `tunnel_motion_fallback.txt`, with `cycle.txt` before `tunnel_motion.txt` | the native Metal backend, with the validation layer; compared with the Metal golden images except ray tracing's own |
| `demo_batch_vulkan` | `batch.txt` | Vulkan through MoltenVK with the Khronos validation layer (an error or warning fails the batch) |

`batch.txt` includes, in this order: materials, shadows, sky, post, hdr, hdr_on, physics, input, assets, audio,
animation, raytracing_fallback, tunnel_fallback, ui, compact_portrait, compact_landscape, menu, audio_tone,
audio_flyby, audio_music, audio_reverb, audio_hrtf, tunnel_motion_fallback, animation_blend2d, animation_lod, animation_skinning,
animation_retarget, animation_blendshapes, raytracing_maps, raytracing_screen, cycle. The parts that
move (the tunnel's motion) and the parts added later come last, before the cycle, which has no capture: TAA's jitter
follows the frame count, so a part inserted earlier would move every capture after it. `batch_native.txt` ends with
animation_blend2d, animation_lod, animation_skinning, animation_retarget and animation_blendshapes after
tunnel_motion (no raytracing_maps or raytracing_screen: the native backend traces those shadows and the occlusion on
the Mac; the render tests run GTAO there).
Every part selects its section first (and resets it where it needs the defaults), so it does not depend on the parts
before it, and its `time` starts at 0. A test per part and backend, `demo_<part>_<backend>`, checks the part's line
in the batch's log (`part <file>: N passed, 0 failed`) and compares its capture with
`tests/golden/<backend>/<device>/<golden>.png`. The batches run in Release builds (`QUENCH_TEST_CAPTURES`); Debug
and the sanitizer builds run `demo_smoke` instead (`smoke.txt`: every section once, offscreen, with assertions or
under the sanitizers). The scenarios point at the window in percent or at named points (`@menu:<section>`,
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
| `animation_skinning` | the skinning pair (crowd 0, 9 characters, all bound); the other characters switched to dual quaternion skinning and back (9 checks) | `demo_animation_skinning.png` at 1 s after the restart: the two characters' upper bodies, the linear one's elbow pinched, the dual quaternion one's whole |
| `animation_retarget` | the retargeting pair (crowd 0, 9 characters with the blob, all bound) after 248 frames: within 1 degree of each other on their circles, the strider's hips 1.14 to 1.16 times as high as the mannequin's pelvis (10 checks); its waits end half a frame off the fixed clock's steps, so that they take the same number of frames wherever the batch's app time is | `demo_animation_retarget.png`: the two walking toward the camera in step, the hero's stage out of view |
| `animation_blendshapes` | the blob (crowd 0, 9 characters, all bound): its model has 4 blend shapes, the weights set by name; after the capture its clip's weights: squash above 0.3 while stretch is 0, then stretch above 0.25 while squash is 0, puff and spikes 0, a slider added to the clip's weight (19 checks); its waits end half a frame off the fixed clock's steps | `demo_animation_blendshapes.png` at 1 s after the restart: the blob swaying, puff 0.7, spikes 1, squash 0.3 (the sliders; the clip's weights off) |
| `raytracing` (native batch) | the section's defaults run: ray-traced sun shadows and occlusion, 2 lamp shadows, reflections, the probe grid (2048 probes), the temporal denoiser, MetalFX temporal at scale 0.67 in TAA's place (no cascades, no shadow draws, at least 19 instances in the top level, the character skinned, a material texture in the hit arrays, no fallbacks); MetalFX denoising runs when asked; shadow maps come back when asked; MetalFX temporal at scale 0.5 resolves in TAA's place, spatial runs; per-pixel indirect light runs; the path tracer adds samples in TAA's place; bilinear at scale 1 again | `demo_raytracing_native.png` at 2.5 s (every traced effect at full size with the engine's temporal denoiser, the sun and the character moving): the scene |
| `raytracing_fallback` (Metal, Vulkan) | every request falls back (cascades, no occlusion, no top level, unshadowed lamps, the sky's reflections and light, no probes, bilinear for MetalFX denoising and spatial at scale 0.5, the real-time frame for the path tracer), and the renderer lists the fallbacks (at least 5, then 6) | `demo_raytracing.png` at 2.5 s: the scene |
| `raytracing_maps` (Metal, Vulkan) | the lamps' shadow maps at 512 (Graphics panel, "Shadow maps"): no traced lamp shadows, 2 lamps with 7 maps (the point lamp 6, the spot lamp 1) and draws in them, the sun's cascades, no top level; off again, no maps | `demo_raytracing_maps.png` at 2.5 s: the scene, the block's shadow from the point lamp on the green wall and the fence's from the spot lamp |
| `raytracing_screen` (Metal, Vulkan) | the screen-space fallbacks (Graphics panel, "Screen-space fallbacks") with the section's traced occlusion and reflections asked for: GTAO and SSR run after a prepass of their own (depth and G-buffer), no top level, the sun's cascades; off again, no occlusion, the sky's reflections | `demo_raytracing_screen.png` at 2.5 s: the scene with GTAO's contact shadows under the block, the crate, the spheres and along the walls, the gold spheres' lower halves reflecting the floor (since 2026-11-07, SSR) |
| `tunnel` (native batch) | the section's defaults run: 10 lamps, 8 of them area lights, 4 lamp shadows, reflections, the probe grid, MetalFX temporal at 0.67, no shadow draws (no sun), at least 46 instances in the top level, no fallbacks; lamp groups off and on change the counts; the anisotropy changes; the path tracer adds samples; bilinear at scale 1 again | `demo_tunnel_native.png` at 2.5 s (every traced effect at full size, the lamps at their time 0, standing still, the camera free): the scene |
| `tunnel_path` (native batch) | the path tracer runs to 256 samples with the lamps still, then stops | `demo_tunnel_path_native.png`, without a golden image (1.3 MB of noise); look at it beside `demo_tunnel_native.png` |
| `tunnel_fallback` (Metal, Vulkan) | the 8 area lights light without shadows, the sky's reflections and light, no top level, bilinear at scale 1, the path tracer falls back (at least 3 fallbacks); the tubes and the portal off and on | `demo_tunnel.png` at 2.5 s: the scene |
| `tunnel_motion` (native batch, the last part) | the drive starts eased (its speed rises over a second), the lamps' clock runs, the path tracer stops both and they go on after it, Pause all stops both, a script's camera hands the drive to the free camera | `demo_tunnel_motion_native.png`: 3 s of the drive and the lamps from their time 0 |
| `tunnel_motion_fallback` (Metal, Vulkan, the last part) | the same without ray tracing: the path tracer falls back, so nothing stops for it | none |
| `ui` | the wide layout with its windows | `demo_ui.png`: the materials section with the menu, its panel and the caption |
| `compact_portrait`, `compact_landscape` | `compact.txt` in a phone-sized frame (400 x 720 and 720 x 320 points): the compact layout, the bar's arrows, the menu sheet, About and its licences, the controls sheet | `demo_compact_portrait.png`, `demo_compact_landscape.png`: the controls sheet over the materials section |
| `menu` | switching sections by keys, gamepad, a click and a tap on the list; the menu by Tab, the arrows and Enter and by the D-pad, A and B; hiding the windows | none |
| `audio_tone`, `audio_flyby`, `audio_music`, `audio_reverb`, `audio_hrtf` | the checks on the mix itself (below) | none |
| `cycle` (every batch, the last part on SDL GPU) | every section three times (created or reset, switched, reset), with the live GPU resources, the scene's instances and the error count marked after the first round and the memory after the second: no growth (the GPU resources within 3, fences and transfer buffers come and go; memory below 64 MB in the third round; no instance more; no new error). The memory is marked a round later since stage 11: after the first round alone it swung between -24 and +47 MB on the native backend, where the ray tracing section's filter histories come and go. The bound is 64 MB since the tunnel moves (2026-11-06): over 20 rounds the footprint moved within about 60 MB from round to round, with the tunnel's motion and without it, with no trend and the same GPU resources | none |

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
| `demo_checks`, `demo_settings`, `demo_benchmark` | `tests/DemoUserFiles.cmake` runs the demo on a fresh `--user-dir` and reads the file it wrote: the check mode's marks by command and by the buttons, read back by a second run, a corrupt `checks.json` starts empty (`checks*.txt`); settings saved, read back, a corrupt file gives the defaults and is written again, `--no-settings` leaves it (`settings_*.txt`); `mode benchmark 0.1` writes a report with 12 sections (`benchmark_all.txt`) |
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
`render_scale`, `ray_instances`, `ray_builds`, `ray_refits` (all after fallbacks), `fallbacks` (their count),
`camera_x`, `camera_z` (the target); the demo's own: `section_ready`, `sections_ready`, `init_ms`, `switch_ms`,
`scene_instances`, `compact`, `sheet` (0 none, 1 the menu, 2 the controls, 3 About, 4 Settings, 5 Graphics), `menu_focus`,
`about`, `licence_bytes`, `ui_scale`, `touch_ui`, `settings_loaded` (1 read, 0 not, -1 unusable), `settings_writes`,
`mode` (0 none, 1 tour, 2 checks, 3 benchmark), `tours`, `check_index`, `check_items`, `check_writes`,
`checks_pending`, `checks_ok`, `checks_problem`, `checks_skipped`, `benchmark_sections`, `benchmark_written`,
`log_errors`, `log_warnings`, `job_test_ms` (-1 until a job test finished); then the
section's own (for example `bodies`, `blasts`,
`player_x`, `jumps`, `marker_draws`, `exposure`; the assets section's `assets_loading`, `assets_ready`,
`assets_failed`, `assets_idle`, `assets_loads`, `assets_reloads`, `assets_evictions`, `showcase_ready`,
`showcase_failed`, `streaming_active`, `streaming_loads`, `streaming_unloads`; the audio section's `sounds_ready`,
`music_track`, `flyby_x`, `flyby_pitch`, `pings`, `reverb`, `room_size`, `hrtf`; the engine's `audio_*` values; the animation section's
`anim_ready`, `anim_failed`, `anim_characters`, `anim_bound`, `anim_ground_rays`, `hero_state` (0 the blend, 1 the
jump), `hero_transition`, `hero_look_at`, `hero_angle`, `footsteps`, `landings`, `crowd`, `held_distance`,
`slope_pelvis`, and per stand and leg `slope_leg0_offset` (also `_reached`, `_grounded`; `slope` or `step`, leg 0
or 1), `view`, the steered character's `steer_vx`, `steer_vz` (asked), `strafer_vx`, `strafer_vz` (moved, over
0.2 s), `strafer_heading_error` (degrees) and `strafer_radius`, animation LOD's `anim_evaluated` (characters
updated in the last update), `anim_lod_rate1`, `anim_lod_rate2`, `anim_lod_rate4` (characters at each rate) and
`skinned_reused` (the renderer's reused skins, of the frame before), `dual_quaternion` (the other characters'
skinning: 1 dual quaternion, 0 linear), the retargeting pair's `pair_gap` (degrees on their circles) and
`pair_height_ratio`, the blob's `blob_shapes` (its model's blend shapes), `blob_weight_<shape>` (the weight it is drawn with), `blob_clip_<shape>` (its clip's value) and `blob_clip`; the ray tracing section's `rt_ready`, `sun_azimuth`). Game commands: `section ID`, `reset_section`, `reset_camera`,
`sheet none|sections|controls|about|settings|graphics`, `mode off|tour|checks|benchmark [SECONDS]`,
`check ok|problem|skip [NOTE]`, `check next|prev`, `job_test`, `settings_save` (now, not half a second later),
`settings_corrupt` (overwrites the settings file with text that is not JSON),
`camera YAW PITCH DISTANCE` (degrees, meters), `set NAME VALUE` (`ui_scale`, `touch_controls auto|on|off`; render settings: `aa none|fxaa|taa`,
`tone aces|agx`, `cascades`, `shadow_size`, `shadow_filter`, `shadow_distance`, `translucent_shadows map|dithered`,
`sun_shadows maps|rt`, `shadow_rays`, `shadow_softness`, `ao none|rt`, `ao_rays`, `ao_radius`,
`upscaling bilinear|spatial|temporal`, `scale`, `bloom`, `bloom_strength`,
`sky`, `dither`, `sharpen`, `vignette`; otherwise the section's options, for the assets section `latency SECONDS`,
`texture_budget BYTES|unlimited`, `upload_budget BYTES|unlimited`, `focus_angle DEGREES`, `move_focus`, `reload`,
`models`, `hot_reload`; for the audio section `orbit`, `flyby` (also restarts the pass), `tone`, `tone_hz`,
`tone_x`, `tone_z`, `orbit_speed`, `flyby_speed`, `attenuation none|inverse|linear|exponential`, `min_distance`,
`max_distance`, `doppler`, `music 0|1|2`, `bus_<name>` (volume), `mute_<name>`, `pause_effects`, `ping`,
`burst N`, `reverb`, `room_size`, `hrtf`; for the animation section `restart`, `speed`, `lean`, `jump`, `wave`, `foot_ik`, `look_at`,
`footstep_sound`, `slope DEGREES`, `crowd N`, `lod`, `lod_interpolate`, `dual_quaternion`, `view hero|blend2d|skinning|retarget|blendshapes`, `steer_auto`, `steer_vx`, `steer_vz`, `blob_squash|stretch|puff|spikes WEIGHT`, `blob_clip`; for the ray tracing section `turn_sun`, `walk`, `sun_azimuth`,
`sun_elevation`).

## Benchmark

`scenarios/benchmark.txt` measures the materials, shadows, sky, post-processing, HDR (in SDR, then with HDR output on),
physics, assets, audio, animation and ray tracing sections for 5 s each in real time (budgets and numbers:
[Performance](performance.md)); the assets region streams with the moving focus and the default budgets, after the
first loads have finished; the audio region plays the three emitters, the tone and the music; the animation region
has a crowd of 256 (264 characters), and its perf line ends with the section's own times over its last 240 frames:
`anim_update_ms` (mean) and `anim_update_max_ms` of `AnimationSystem::update`, `anim_threads`, `anim_lod` (off,
interpolate or hold), `anim_evaluated` (the mean of the characters updated per frame), and `skinning_gpu_ms`, the mean of the skinning pass's GPU time (-1 without GPU timing). It runs in a window on the
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
driving too (`tunnel_moving`), and the path tracer. Run it with `--onscreen --gpu-backend metal` (on SDL GPU
every region measures the fallbacks); the perf lines carry the GPU time per timed range
([Performance](performance.md#measurements)). For the
cost of each effect add `--present-mode immediate`: with vsync the GPU's clock follows the load, and light regions
read slower per range than they are (the stage 11 costs in [Performance](performance.md) come from such a run).

On a phone the same costs come from `scenarios/raytracing_phone.txt` (each effect at the device's size, then the
render scales and upscalers with everything and with less, 4 s each; every group of regions waits for the nominal
thermal state first, at 20 frames per second on the cheapest frame), and `scenarios/raytracing_phone_sustained.txt`
holds the section's phone defaults for a minute at the display's rate and for a minute at 60 Hz;
`scenarios/raytracing_phone_probes.txt` adds the probe grid to the phone defaults, traced every frame and in 2, 4
and 8 turns, then holds it in 4 turns for 20 s at 60 Hz (not run on a phone yet). For the tunnel,
`scenarios/tunnel_phone.txt` measures each option at half size (and the lamps at 0.67 and full size), then holds the
section's phone defaults at 60 Hz for 40 s with the lamps still, then 20 s in motion (`tunnel_moving_60hz`), and
captures them and the path tracer; `tunnel_phone_fallback.txt` runs the fallback frame on SDL GPU, still and in
motion. `scenarios/lamp_maps_phone.txt` measures the lamps' shadow maps on SDL GPU (the ray tracing section's two
lamps and the tunnel's two spots, unshadowed and with maps of 256 and 512, then the tunnel with maps of 256 at 60 Hz
for 20 s; not run on a phone yet). `scenarios/gtao_phone.txt` measures GTAO on SDL GPU (the shadows section, the ray
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
