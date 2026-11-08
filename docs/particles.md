# Particles and trails

Stage 14 of the [Roadmap](roadmap.md) (requested by the user on 2026-11-08, for light painting in the first game on
the engine: long thin glowing ribbons with bright heads and fading tails, sparkles they shed, a field of twinkling
points, soft bloom). Particle systems and trails for any genre: sparks, smoke, dust, magic, fireflies, a light trail
behind a character, streams of light along curves. `quench_particles` is headless (what a system is, its spawns, the
CPU simulation, trails and paths); `quench_render` keeps systems and trails in its retained scene by handle, steps
them on the GPU by compute (or on the CPU) and draws them after TAA. Written for the engine, not moved from a game. It
ran on the Mac (SDL GPU's Metal driver, the native Metal backend and Vulkan through MoltenVK), in the iOS Simulator
(the test executables) and, the headless part, as WebAssembly under Node.js; nobody has looked at it in motion or on
a phone yet, and no phone has run it. All documentation: [index](README.md).

## Use

```cpp
#include <quench/particles/Path.h>
#include <quench/render/Renderer.h>
#include <quench/render/Scene.h>

namespace particles = quench::particles;

// A field of twinkling points over a meadow: 4000 slots, spawned evenly in a box, drifting in a slow curl noise.
particles::ParticleDesc field;
field.maxParticles = 4096;
field.rate = 1400.0f;                      // per second (times RenderSettings::particleDensity)
field.shape = particles::Shape::Box;
field.extent = {20.0f, 0.5f, 12.0f};       // half sizes, in the emitter's space
field.lifetime = {2.5f, 4.0f};
field.speed = {0.0f, 0.05f};
field.size = {0.025f, 0.05f};
field.noise = 0.08f;                       // m/s
field.color = {1.6f, 1.8f, 2.4f};          // linear HDR: above 1 glows through bloom
field.twinkle = 0.85f;
field.twinkleRate = 0.6f;
field.sprite = particles::Sprite::Glow;
Result<render::ParticleSystemId> system = scene.addParticles(field, glm::translate(glm::mat4(1), {0, 0.5f, -6}));

// A ribbon of light behind a moving point that sheds sparkles.
particles::TrailDesc ribbon;
ribbon.lifetime = 1.5f;                    // s: the tail fades out over this long
ribbon.width = 0.08f;
ribbon.color = {1.0f, 1.6f, 3.0f};
ribbon.headSize = 0.15f;                   // a glow at the head
const render::TrailId trail = scene.addTrail(ribbon);
scene.setTrailSparkles(trail, *sparkles, 6.0f); // into another system, 6 per metre

// Every frame (Game::update): move the head on the game's clock; the renderer ages the strip by View::time.
scene.moveTrail(trail, headPosition, app.time());
```

`RenderSettings::particles` says where systems are stepped (`Gpu`, the default; `Cpu`; `Off`), `particleDensity`
scales every system's spawn rate (quality presets), `Renderer::effects().particles` says what runs and
`RenderStats` (`particleSystems`, `particles`, `particleSlots`, `particleSpawns`, `trails`, `trailVertices`) what was
drawn. The demo's Particles section (`examples/demo/sections/ParticlesSection.cpp`) has a stream of light along a
closed path with braided heads, a sparkle field, fireflies, a campfire's flames, sparks and smoke.

## Systems

A system is a `particles::ParticleDesc` (`include/quench/particles/Particles.h`) registered with
`Scene::addParticles(desc, transform)`; the transform places, turns and scales its emitter. Its particles live in
`desc.maxParticles` slots, a ring: when every slot is alive, the next spawn replaces the oldest particle.
`setParticles` changes the desc (another `maxParticles` restarts it), `setParticleTransform` moves the emitter
(spawns between frames are placed along the motion; `teleport` for a jump), `setParticlesEmitting(false)` stops its
spawns while the live particles go on, `setParticlesVisible(false)` neither draws nor steps it (its particles wait
where they are), `burstParticles`, `emitParticles` (particles at world positions), `restartParticles`,
`removeParticles`, `particlesAlive`.

### Spawning

`particles::Emitter` (headless) makes a system's spawns on the CPU every frame: `rate` per second while emitting,
`ratePerMeter` per metre the emitter moves, bursts and particles placed by the game (`emitAt`; trails shed their
sparkles this way). Each spawn is made at its own time within the frame and at the emitter's place at that time,
then moved on to the frame's end, so a system looks the same at 30, 60 or 120 frames per second. Spawn k of a system
draws its random numbers from the seed and k only: the same steps spawn the same particles on every run and every
backend.

Shapes, in the emitter's space: `Point`, `Sphere` (`extent.x` the radius; `surface` on its surface only), `Box` (half
sizes), `Disc` (in the xz plane; `surface`: on the rim) and `Path` (along `ParticleDesc::path`, a centripetal
Catmull-Rom curve, open or closed, within `extent.x` of it). The start: `lifetime`, `speed` in a cone of half angle
`spread` around `direction`, turned away from the shape's centre by `outward`, plus `inherit` times the emitter's
velocity; `size`, `rotation`, `spin`.

### Motion

`shaders/quench/ParticleField.h` is one source for C++ and HLSL, so both simulations move a particle the same way
up to rounding. A particle is three float4 (position and age, velocity and lifetime, its random number, size,
rotation and spin). Each step it falls by `gravity` and takes the air's velocity at rate `drag` (1/s), integrated
exactly for a constant air over substeps of at most 1/60 s (at most 8 in one step). The air is a curl noise
(`noise` m/s, features of `noiseScale` m, changing at `noiseRate`: divergence-free, so it swirls without gathering
particles into clumps) plus `wind` times the scene's weather's wind at the particle (`Scene::setWeather`; the gusts
and eddies of `quench/WeatherField.h`, the same field the rain and `Weather::windAt` use). The renderer steps a
system by `View::time`'s change since its last frame, at most 0.25 s (a hitch does not throw particles away).

### Look

Color and opacity times `colorOverLife`, size times `sizeOverLife` (curves of up to 8 keys over age / lifetime,
sampled 16 times for the shaders), `brightnessVariation` between particles and a `twinkle` (depth, `twinkleRate`,
`twinkleSharpness`: 1 a sine, higher shorter and brighter flashes; the mean brightness stays the same). Colors are
linear HDR: above 1 glows through bloom. `Blend::Additive` (light: glows, sparks, magic) or `Blend::Alpha` (smoke,
dust; with `lit`, lit by the sun and the sky's ambient light, without shadows). Sprites are procedural: `Glow` (a
bright core in a soft halo), `Soft`, `Star` (four thin rays) and `Puff`. `Facing::Camera` quads turn by the
particle's rotation; `Facing::Velocity` draws a particle out along `stretch` seconds of its motion on the screen
(sparks, streaks). `softness` fades a particle out where it nears the scene behind it, `nearFade` near the camera,
and `minPixels` keeps a far particle at least that many pixels across, fainter by its area below it (no flicker of
sub-pixel points).

## Trails

`Scene::addTrail(particles::TrailDesc)` (`include/quench/particles/Trail.h`): the game moves the head every frame or
tick with `moveTrail(trail, head, time)` on a clock of its own; the head records a point wherever it has moved
`spacing` m (at most `maxPoints`), the points age, and the strip through them is a centripetal Catmull-Rom curve (up
to `subdivisions` vertices per segment) facing the camera. Width (`width` times `widthOverAge`) and color (`color`
times `colorOverAge`) follow t = max(age / `lifetime`, length / `maxLength`): 0 at the head, 1 at the tail, where the
strip is cut exactly at the end of its life. `headSize` adds a glow at the head while it emits. The renderer ages the
strip by `View::time`'s change since the last move, so it fades out after the head stops; `setTrailEmitting(false)`
stops recording (the strip fades where it is), `teleport` breaks the strip, `clearTrail` forgets it.
`setTrailSparkles(trail, system, perMeter)` sheds `perMeter` particles of a system along each move, at their places,
with the head's velocity times the system's `inherit`. Trails follow the times the game gives, so they look the same
at any frame rate; they are built on the CPU (`particles::Trail::build`) and drawn as one strip each.

## Paths

`particles::Path` (`include/quench/particles/Path.h`): a centripetal Catmull-Rom curve through points (no cusps or
loops between close points), open or closed, measured by its length: `at(distance)` and `tangent(distance)`. Path
emitters spawn along one; games move trail heads and light streams along them (the demo's three streams run along
closed paths, each head braided around the curve).

## In the frame

After TAA and upscaling (MetalFX's too) and the rain's streaks, before the game's `PostHdr` passes and bloom
([Render](render.md#the-frame), step 4): each visible system's spawns uploaded and its slots stepped by a compute pass (one dispatch per system),
the trails' strips built and uploaded, then systems and trails drawn back to front (by their emitter's and their
head's distance; particles within a system are not sorted) into the output-size HDR image, tested against the
scene's depth. So:

- **Bloom:** colors above 1 glow, as the reference's soft bloom does.
- **Reversed depth:** the depth test and the soft fade read the scene's reversed depth (point sampled; at a render
  scale below 1 the depth is the smaller render's).
- **TAA:** particles never enter TAA's history (no ghosting behind a fast spark; test
  `renderer_particles_leave_no_taa_history`), and they are not anti-aliased by it: their sprites are soft and
  `minPixels` fades sub-pixel ones. They are drawn with the unjittered projection against the jittered depth, so a
  hard edge in front of a particle can move by up to half a pixel between frames; `softness` hides it.
- **Fog:** fogged per vertex by the scene's height fog (additive ones dimmed by it, alpha ones blended toward its
  color).
- **Ray tracing:** particles are not in the acceleration structures: no traced shadows or reflections of them, and
  they light nothing (the demo's fire is a point light beside its flames). With traced effects on they are drawn the
  same way, after the denoisers and MetalFX.
- **The path tracer:** off in its frames (`fallbacks()` says "particles: not in the path tracer's frame; none").

`RenderSettings::particles = Gpu` steps systems by compute where `gpu::Caps::computeShaders` (both backends on every
device they run on today) and the step's pipeline was made; else `Cpu`: `particles::Simulation`, the same functions,
uploaded every frame (single-threaded). `Off` draws nothing. Without a depth texture to sample (`sampledDepth`) or
when the draw pipelines fail there are no particles, with a line in `fallbacks()`. Changing the simulation restarts
every system.

## Budgets

At most `particles::kMaxSystemParticles` (65536) slots per system and `kMaxSceneParticles` (262144) per scene:
`addParticles` and a bigger `setParticles` return an error when they would be exceeded (the system keeps its old
desc). Every slot takes 96 bytes on the GPU (the state and what the draw reads; 24 MiB at the limit, 2.2 MiB for the
demo's 23488 slots). Every slot of a drawn system runs the vertex shader, alive or not, and is stepped by the compute
pass: `particleDensity` lowers the spawns and the fill, not that work, so a quality preset should lower
`maxParticles` too (Light below). The renderer's buffers grow by doubling and are kept: once a scene's particles
have been seen, a frame allocates nothing. Profiling: the zones `render.particles` and `render.particles.step` (spawns
and the CPU simulation; [Tracy](performance.md#profiling-with-tracy)), the GPU timed ranges `particles_sim` (the
compute step) and `particles` (the draws) on the native backend ([GPU time per pass](performance.md#gpu-time-per-pass)).

## Cost

2026-11-08, `examples/demo/scenarios/particles_cost.txt` offscreen on the native Metal backend on the M4 Pro: the
demo's dusk scene with 11 systems in 23488 slots (about 7000 particles alive), 20 trails of about 8500 strip
vertices together; GPU busy time p50 per timed range, ms ([Performance](performance.md#particles)).

| Setting | `particles_sim` 1920 x 1080 | `particles` 1920 x 1080 | `particles_sim` 1206 x 2622 | `particles` 1206 x 2622 |
|---|---|---|---|---|
| Everything on the GPU | 0.119 | 0.389 | 0.125 | 0.350 |
| Everything on the CPU | - | 0.343 | - | 0.311 |
| Density 0.5 / 0.25 | 0.136 / 0.120 | 0.340 / 0.308 | 0.118 / 0.114 | 0.315 / 0.308 |
| The streams alone (6 systems, 20 trails) | 0.069 | 0.191 | 0.071 | 0.218 |
| The sparkle field alone (8192 slots) | 0.024 | 0.121 | 0.043 | 0.139 |
| The campfire alone (3 systems) | 0.066 | 0.093 | 0.064 | 0.105 |

The cost follows the slots more than the pixels: half the density saves about a tenth. The frame's CPU time p50 was
0.47 ms with the particles on the GPU and 0.48 ms with them off (p95 1.13 and 0.54), 0.87 ms with the CPU simulation
(p95 2.15); offscreen runs spend most of it on the efficiency cores, so these vary between runs. Not measured on the
screen or on a phone.

## Demo and tests

The demo's section 15, **Particles**: a blue-hour sky over a dark meadow (the Dusk slider blends from golden hour),
three streams of light (gold, blue, rose) whose 20 braided heads run along closed paths and leave ribbons that shed
sparkles and dust, a sparkle field that twinkles denser at dusk, fireflies, and a campfire with flames, sparks that
streak, lit smoke and a flickering point light, in the weather's wind. Controls: the simulation (GPU, CPU, off),
density, each part on or off, the streams' speed, the trails' length, dusk, the wind, a burst of sparks, restart.
Its batch part `particles.txt` (26 checks on every backend: 11 systems and 20 trails, more than 5000 particles, the
CPU simulation, a burst, the density, parts off and on, the simulation off) captures `demo_particles.png` and
`demo_particles_fire.png` (the same images on SDL GPU's Metal driver and the native backend; Vulkan through MoltenVK
within a mean of 0.0008); check mode item `particles.motion`; a benchmark entry ([Engine demo](demo.md)).

Tests: `particles.*` (15 headless cases: curves, rates independent of the frame rate, deterministic spawns, shapes,
spawns along the emitter's motion, the ring, the exact motion without air, the curl noise without sinks, the wind,
the twinkle's mean, the look over the life, paths through their points, trails by distance, fading, stopping and
breaking; also under Node.js) and `render.renderer_particles_*`, `renderer_trails_*` and
`renderer_particle_slots_are_limited` (9 cases on SDL GPU's Metal driver, the native backend and Vulkan through
MoltenVK: the GPU and the CPU simulation give the same pixels (at most 0 levels apart for a glow, a mean of 0.000 for
120 moving particles), hidden behind surfaces and faded near them, stretched along their motion, nothing in TAA's
history, trails bright at the head and faded at the tail, sparkles shed per metre, the fallbacks, the slot limits).

## Platforms

The Mac (SDL GPU's Metal driver, the native Metal backend, Vulkan through MoltenVK), the iOS Simulator (the test
executables) and, the headless tests, WebAssembly under Node.js. The shaders build to SPIR-V and MSL on the Mac and
run from both; DXIL is compiled on Windows hosts only, as for every engine shader ([Platforms](platforms.md)). Not
run on an iPhone; no costs on a phone (the 1206 x 2622 column above is the Mac at a phone's size).

## Limitations

- No sorting within a system: alpha particles of one system can draw in the wrong order (keep smoke faint, or split
  it into systems); systems and trails are sorted by one distance each.
- Every slot of a drawn system is stepped and runs the vertex shader (no compaction or indirect draws).
- Not in the ray tracing structures or the path tracer; lit puffs have no shadows; particles do not collide.
- The CPU simulation is single-threaded; `particlesAlive` is a scan of the system's slots.
- The particles' clock wraps every 4096 s (68 minutes): the noise and the twinkles jump once then.
- Hidden systems are not stepped; a system's spawns wait for a frame that draws it.
- Not looked at in motion or on a phone's screen by a person; no phone has run it.
