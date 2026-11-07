# Animation

Stage 8 of the [Roadmap](roadmap.md): `quench_animation`, a headless module on ozz-animation (skeletons and clip sets
as assets, sampling and blending, data-driven state machines, events, root motion, masked and additive layers,
two-bone and aim IK, foot placement, bone attachments, animation LOD), the cooking of glTF skins and animations in
`quench_cook`, and GPU skinning in `quench_render`'s retained scene. The engine demo's "Animation" section shows it
([Engine demo](demo.md)). Written for the engine: neither game has skeletal animation to move (Ashfall animates one
mesh procedurally in a vertex shader, TrailForge has none). What was run where is under [Platforms](#platforms);
nothing in this stage was looked at in motion by the user yet. All documentation: [index](README.md).

## Use

```cpp
#include <quench/animation/AnimationSystem.h>

namespace anim = quench::animation;
anim::AnimationSystem animation(app.assets(), app.jobs()); // registers "skeleton", "animation", "animgraph"
anim::CharacterDesc desc;
desc.skeleton = animation.loadSkeleton("characters/hero.qskel");
desc.clips = {animation.loadClips("characters/hero.qanim")};
desc.graph = animation.loadGraph("characters/hero.graph.json");
desc.transform = heroTransform;
desc.feet = anim::FootPlacementDesc{.legs = {{"thigh_l", "shin_l", "foot_l"}, {"thigh_r", "shin_r", "foot_r"}},
                                    .pelvis = "pelvis"};
anim::CharacterId hero = animation.addCharacter(std::move(desc));
animation.setGroundQuery(groundRay); // a ray down into the physics world, called from job threads

// The skinned model through render::SceneAssets: its skins name their joints, the character maps them.
auto heroModel = sceneAssets.loadModel("characters/hero.qmodel");
render::ModelInstanceId model = sceneAssets.addModel(heroModel, heroTransform);
int skin = -1; // once heroModel.ready(): animation.addSkin(hero, skins[0].joints, skins[0].inverseBindMatrices)

animation.setLod({.enabled = true}); // optional: characters small on the screen update less often

// Every frame
animation.setLodView(cameraEye, cameraFovY); // with LOD on
animation.setFloat(hero, "speed", speed);
if (jumpPressed) animation.trigger(hero, "jump");
animation.update(dt);                                  // all characters on the job system
for (const anim::Event& e : animation.events())
    if (e.name == "footstep") playStep(e.value);       // the clip's marker, its value and weight
const anim::RootMotion motion = animation.rootMotion(hero); // move the character by its clips
sceneAssets.setJointPalette(model, 0, animation.skinPalette(hero, skin));
torch.setTransform(animation.jointWorld(hero, animation.findJoint(hero, "hand_l")) * grip);
```

The game owns its `AnimationSystem` (one per `assets::AssetManager`, since it registers the asset types there); the
app shell has none. `AnimationSystem::update()` binds characters whose assets became ready or changed (also by hot
reload), then updates every character on the job system and waits for it: the frame thread joins the work, and with
zero worker threads (the web) it runs on the calling thread. Everything else is main thread only.

## Skeletons and clips

- **`Skeleton`**: joints in depth-first order with names, parents and rest transforms (ozz's SoA layout inside);
  `buildSkeleton(SkeletonDesc)` for tests and generated content, `readSkeleton` / `writeSkeleton` for `.qskel`. A
  skeleton has a signature (a hash of its names and parents) that clip sets carry.
- **`ClipSet`**: the clips of one source (`.qanim`), each with a name, a duration, loop and additive flags, events
  and optional root motion, compressed by ozz's key reduction (default tolerance 1 mm at 10 cm from a joint) and
  quantization. `buildClipSet(skeleton, desc, clips)` maps a description's joint order to the skeleton's by name.
  A clip set made for another skeleton (another signature) does not bind; its character holds the rest pose.
- **Additive clips** are stored as the difference to their first frame and play on additive layers.
- **Root motion**: a clip names a joint (`root` in the demo) whose horizontal motion, and optionally its turn about
  Y, is taken out of the pose and reported per update as `RootMotion` in model space, across loops and cross-fades
  (weighted). The game applies it to its transform (or ignores it and moves the character itself).

## Graphs (state machines)

A graph is a JSON file, loaded as the asset type "animgraph" (`parseGraph` for strings). Example (the demo's
`examples/demo/assets/characters/mannequin.graph.json`, shortened):

```json
{
  "parameters": {"speed": 0, "lean": 0, "jump": "trigger", "wave": "trigger"},
  "layers": [
    {"name": "base",
     "states": [
       {"name": "move", "blend": {"parameter": "speed", "clips": [
         {"clip": "idle", "at": 0}, {"clip": "walk", "at": 1.1}, {"clip": "run", "at": 2.7}]}},
       {"name": "jump", "clip": "jump", "loop": false}],
     "transitions": [
       {"from": "move", "to": "jump", "trigger": "jump", "duration": 0.15},
       {"from": "jump", "to": "move", "exitTime": 0.85, "duration": 0.3}]},
    {"name": "arms", "mask": ["upperarm_r"], "states": [{"name": "none"}, {"name": "wave", "clip": "wave", "loop": false}],
     "transitions": [{"from": "none", "to": "wave", "trigger": "wave", "duration": 0.25},
                     {"from": "wave", "to": "none", "exitTime": 0.8, "duration": 0.4}]},
    {"name": "lean", "blend": "additive", "weightParameter": "lean", "states": [{"name": "lean", "clip": "lean"}]}
  ]
}
```

- **Parameters**: a number (a float with its initial value), `true` / `false` (a bool) or `"trigger"` (set by
  `trigger()`, consumed by the transition that uses it). Set by name; an unknown name or a wrong type is a warning,
  once per name and character, not an assertion (graphs are data and change under a running game).
- **Layers**: the first is the base pose; the others `"blend": "override"` (default) on a joint `mask` (the subtrees
  of the joints named; empty: all) or `"additive"`, each with a `weight` and an optional `weightParameter`.
- **States**: a `clip`, a 1D `blend` tree over a float parameter (points sorted by `at`, two neighbours weighted
  linearly), a 2D `blend2d` space over two float parameters, or neither (the rest pose in the base layer, nothing in
  the others); at most 16 clips per tree or space. The clips of a tree or a space play at one normalized time (phases
  in step, so the feet of walk and run do not cross), its duration the weighted mean of theirs; a point's `phase`
  (default 0) shifts its clip's normalized time, for clips whose feet land at other phases. `speed`,
  `speedParameter` and `loop` (overrides the clip's flag) per state; `start` names the first state.
- **2D blend spaces**: `{"name": "move", "blend2d": {"parameters": ["vx", "vz"], "clips": [{"clip": "idle", "at":
  [0, 0]}, {"clip": "walk", "at": [0, 1.1]}, {"clip": "strafe_l", "at": [0.75, 0], "phase": 0}, ...]}}` (the demo's
  `mannequin_2d.graph.json`: idle, walk, run, walk back and the two strafes at their velocities). Freeform
  Cartesian weights by gradient band interpolation (Johansen, "Automated Semi-Procedural Animation for Character
  Locomotion", 2009, section 6.3; `blendSpaceWeights()` in `Graph.h`): each point's weight is the smallest of
  `1 - (p - p_i)·(p_j - p_i) / |p_j - p_i|²` over the other points, clamped to 0..1, the weights normalized to sum
  1. They are exact at the points (1 there, 0 for the others), continuous, symmetric for mirrored points, the same
  as the 1D tree's on a line of points, and outside the points' hull the nearest edge or point holds. Weights below
  1e-4 are dropped (those clips are not sampled). Points must be at least 1e-4 apart. Events and the root motion's
  loops come from the heaviest clip; the root motion is the weighted sum of the clips' (between points it does not
  match the parameters: the demo's steered character moves at 0.49 m/s when 0.8 m/s ahead is asked for, between
  idle and walk, idle's 3 s cycle slowing the shared phase; the same holds in a 1D tree).
- **Transitions**: `from` (a state or `"*"`, any other state), `to`, `conditions` (all of
  `{"parameter", "op": ">" | ">=" | "<" | "<=" | "==" | "!=", "value"}`), a `trigger`, an `exitTime` (normalized time
  of the source, every cycle for loops) and the cross-fade `duration` in seconds (default 0.2). Checked in order;
  the first that holds starts. A cross-fade runs to its end before the next transition is checked (a trigger set
  meanwhile waits).
- **Errors** name the layer, state or transition (`parseGraph`); clips the graph names that its character's clip
  sets lack are reported when the character binds, and the character holds the rest pose.

## Events

Clip events are named markers (`time`, `name`, `value`), from the clip settings at cook time. An update delivers
every marker the playback passed, in order, also across a loop's end; an update longer than a whole loop delivers
each marker once with its `count`. Only states at a weight of at least `AnimationSettings::eventWeight` (0.5, so one
of a cross-fade's two) deliver events, with the clip's weight; `CharacterDesc::events = false` skips a character
(the demo's crowd). The events of the last update: `AnimationSystem::events()`.

## IK, foot placement and attachments

- **`solveTwoBone`** and **`solveAim`** (`Ik.h`, ozz's solvers on GLM matrices) for a game's own IK; they return the
  corrections to multiply onto the joints' local rotations and whether the target was reached.
- **Foot placement** (`CharacterDesc::feet`, `setFootPlacement`): per leg a ray down from above the animated ankle
  through `setGroundQuery` (the demo casts into a Jolt world; the query runs on job threads, several at once), the
  pelvis down so the lower foot reaches its ground, two-bone IK of each leg to its ground offset, the foot turned to
  the ground's normal (`alignToGround`). Offsets follow the ground exponentially (`smoothing`); ground more than
  `maxStepUp` above or `maxStepDown` below is not reached for. `CharacterState::legs` reports reached, grounded and
  the offset per leg.
- **Look-at** (`CharacterDesc::lookAt`, `setLookAt`): aim IK down a chain (head, neck, chest), the way ozz's look-at
  sample does it (credited in `src/animation/Ik.cpp`), up to `maxAngle` from the facing; turning on and off fades.
- **Attachments**: `jointWorld(character, joint)` is a joint's world matrix after the update (the demo's torch on
  `hand_l`); `findJoint` maps names.

## Animation LOD

Off by default. `AnimationSystem::setLod(AnimationLod)` turns it on (or `AnimationSettings::lod`), and
`setLodView(eye, fovY)` gives the camera each frame (`clearLodView()`: every character every frame again). A
character's size on the screen is `CharacterDesc::height` (1.8 m) over its distance from the eye (to the middle of
its height) times `2 tan(fovY / 2)`: the fraction of the view's height it covers. That picks its rate:

| Size (default threshold) | Clips sampled | IK |
|---|---|---|
| at least `half` (0.15) | every frame | on |
| below `half` | every 2nd frame | on down to `ik` (0.1) |
| below `quarter` (0.06) | every 4th frame | off below `ik` |

With a 50 degree view a 1.8 m character updates every frame within 12.9 m, every 4th frame past 32 m, and leaves out
foot placement and look-at past 19 m. A finer rate (and IK again) only once the size is past its threshold by
`hysteresis` (15 %), so a character on a threshold does not switch back and forth. The characters at one rate are
staggered by their slots, so every frame updates about the same number.

- **Interpolated** (`interpolate`, the default): an update is computed for the frame before the character's next
  one (one period ahead), and the frames in between blend the pose shown last from there toward it (the local
  transforms, as a cross-fade does). The pose shown is on time: on a clip that moves linearly it is where an update
  every frame puts it (within 1e-6 m in `lod_interpolation_and_hold`). It still runs the local-to-model step and the
  palettes every frame, so it saves only the clip sampling.
- **Held** (`interpolate = false`): the pose stays until the next update and then catches up (up to 3 frames
  behind at rate 4). Its palettes do not change between updates, so the renderer reuses the skinned vertices
  ([GPU skinning](#gpu-skinning)).
- **Root motion** of an update is spread evenly over the frames to the next, so a character moved by it does not
  step; at a constant speed each frame gets exactly what an update every frame gives. Held, the motion lags the same
  frames as the pose.
- **Events** come with the updates: interpolated, up to a period early; held, on time or up to 3 frames late.
- A character is updated every frame in its first update after binding; a change of rate updates it at once (with
  interpolation, once the pose shown has reached the last update).
- `CharacterState::lodRate`, `lodIk` and `poseChanged` (false: the palettes and model matrices are the last frame's);
  `AnimationStats::evaluated` (characters whose clips were sampled) and `lodRates[3]` (characters per rate).

With LOD off the update is the same as before it existed (the same bits; the demo's captures did not change).
Decided without the user (2026-11-07): screen size rather than distance (the field of view and the character's
size count), rates 1, 2 and 4 only, interpolation on by default.

## Cooking

`quench_cook skeleton IN OUT` writes the joints of a glTF or GLB file's skins (with their ancestors and the non-mesh
nodes below joints, so attachment points come along) as `.qskel`. `quench_cook animation IN OUT [--skeleton FILE]
[--clips FILE]` writes every animation of the file as a clip of one `.qanim`, for the skeleton of `--skeleton` (a
glTF with the same joints) or of the input. `quench_cook model` writes skins into `.qmodel` (version 2: joint names,
inverse bind matrices, four joints and 16-bit weights per vertex that sum to 65535). Details:
[Jobs, assets and the cook](assets.md#quench_cook).

- **Keys**: step and linear keys are kept (step keys held up to 0.1 ms before the next); cubic splines are sampled
  at 30 Hz; glTF's linear rotations are spherical, ozz's normalized linear, so steps wider than 0.2 rad get
  spherically interpolated keys in between (error below 1e-4 rad); ozz's optimizer then drops what it does not need.
- **Clip settings**: `<input stem>.anim.json` next to the source, or `--clips FILE`, listed in the depfile:

  ```json
  {"tolerance": 0.001, "distance": 0.1,
   "clips": {"walk": {"loop": true, "rootMotion": {"joint": "root", "rotation": false},
                      "events": [{"time": 0.09, "name": "footstep", "value": 0}]},
             "lean": {"loop": false, "additive": true}}}
  ```

  Unknown keys, clips the file does not have and events outside a clip are errors.
- **Deterministic**: the same input gives the same bytes (checked by `cook_cli` on the demo's character).
- **In CMake**: `quench_cook_assets(name SOURCE_DIR dir SKELETONS a.gltf ANIMATIONS a.gltf [SKELETON a.gltf]
  MODELS a.gltf)`.
- `--scale` is refused for skinned models (scale the source).

## Files

`.qskel` and `.qanim` are ozz archives in an engine container (little endian: magic `QSKL` or `QANM`, a version, the
payload size, the payload's XXH64, the payload), so a truncated or corrupt file is an error, not an assertion in
ozz. A `.qanim` also holds per clip its events, flags and root motion, and the skeleton signature.

## GPU skinning

Skinning runs in a compute pass (`src/render/shaders/skin.hlsl`, `skin_cs`, 64 vertices per thread group; four
joints per vertex, linear blend skinning). `MeshDesc::skin` (per vertex `SkinVertex`: four joint indices and
weights) and `jointCount` make a mesh skinned; `Scene::setJointPalette(instance, palette)` (or
`SceneAssets::setJointPalette` for cooked models) gives an instance its matrices, three float4 rows per joint in a
storage buffer. Each frame the renderer skins every skinned instance that is drawn (in view or in a shadow cascade)
into its range of a shared vertex buffer, one dispatch per instance; the shadow cascades, the main pass and the
velocity pass then draw those vertices like any mesh, so surface shaders are unchanged. Two output buffers alternate
per frame: the velocity pass (`velocity_skin_vs`) reads last frame's positions for motion vectors (TAA), and a
`teleport` palette has none. Culling uses bounds from the palette (per joint, the sphere of the vertices it moves).
An instance without a palette shows its bind pose.

Why compute and not the vertex shader (decided without the user, 2026-11-06): one skinning per instance and frame
instead of one per pass and cascade (the standard frame draws a mesh up to five times), no skinning variant of every
surface and shadow shader, and the posed vertices in a buffer are what stage 9's ray tracing needs to refit its
acceleration structures. It needs compute in `quench::gpu` (added in this stage: `ComputePipeline`,
`ComputePass`, storage buffers for compute) and `quench_shaderc --stage compute`. It costs a vertex buffer per
drawn skinned instance and frame (two, for motion vectors).

An instance whose palette did not change since the last frame (`setJointPalette` with the same matrices is ignored,
like a held animation LOD pose) and that was skinned in the frame before is not skinned again: a copy pass copies
its vertices from the frame before's buffer into this frame's (`RenderStats::skinnedReused`), and it has no motion
vectors.

`RenderStats::skinnedInstances`, `skinnedVertices`, `skinnedReused` and `skinningGpuMs` (GPU time of the skinning
pass, with the copies, measured through its own command buffer's fence where `Caps::gpuTiming` holds, a few frames
late; -1 otherwise).

## Cost

Per update, the characters are split into jobs of at least `AnimationSettings::charactersPerJob` (4).
Measured in the demo's benchmark region `animation` (a crowd of 256 plus three characters, 20 joints and
810 skinned vertices each, 836 in the glTF before the cook's vertex merge; Release, vsync, Mac, Apple M4 Pro,
1920 x 1080, 2026-11-06; all numbers in [Performance](performance.md#measurements)):

- `AnimationSystem::update` of 259 characters on 12 threads (11 workers and the frame thread): 0.65 to 0.75 ms on
  average over 240 frames, at most 1.1 to 3.7 ms in a frame; three characters: 0.1 ms.
- The frame's CPU time: 2.6 to 2.8 ms at the median, against 1.1 to 1.2 ms with the three characters alone; the
  difference beyond the update is the renderer's and the scene's work for the skinned instances (each its own
  batch and dispatch).
- The skinning pass on the GPU (its own command buffer): 1.1 to 2.0 ms on average, 0.4 to 0.7 ms for three
  characters, which is mostly a fixed cost.

With animation LOD (2026-11-07, the same crowd of 256, 260 characters; Release, Mac, Apple M4 Pro, 1920 x 1080,
**offscreen**, so the frames are not paced and the update time is lower than in the benchmark on the screen above;
means over 240 frames, two runs; before: the commit before LOD, the same session):

| Camera | LOD | Characters sampled per frame | Update, 12 threads | Update, 1 thread | Skinning GPU |
|---|---|---|---|---|---|
| the hero's view | before | 260 | 0.153 ms | 0.343 ms | 2.13 ms |
| the hero's view | off | 260 | 0.159 to 0.164 ms | 0.334 ms | 2.14 to 2.25 ms |
| the hero's view | interpolated | 124 to 129 | 0.150 to 0.152 ms | 0.329 ms | 2.18 to 2.24 ms |
| the hero's view | held | 143 | 0.119 to 0.132 ms | 0.240 ms | 1.13 to 1.18 ms |
| 45 m away | before | 260 | 0.152 ms | 0.334 ms | 1.72 ms |
| 45 m away | off | 260 | 0.148 to 0.153 ms | 0.309 ms | 1.75 to 1.77 ms |
| 45 m away | interpolated | 65 | 0.129 to 0.136 ms | 0.256 ms | 1.58 to 1.61 ms |
| 45 m away | held | 65 | 0.088 to 0.093 ms | 0.139 ms | 0.46 to 0.47 ms |

On this 20-joint character with a three-clip blend the sampling is a small part of an update: interpolation saves
up to a quarter of the update, holding more than half of it and most of the skinning pass. Skeletons with more joints
and more clips sample more per update. Not measured on the iPhone. The renderer dispatches the skinning per
instance; one dispatch over all of them is the next step if a game needs more characters.

## Budgets and failures

Skeletons, clip sets and graphs are assets of their own types, with budgets in `AnimationSettings`
(`skeletonBudget`, `clipBudget`, `graphBudget`; unlimited by default) and their sizes in the manager's statistics.
A missing or corrupt file, clips for another skeleton, a graph that names missing clips: one log line, and the
character holds its skeleton's rest pose (its mesh the bind pose until the skeleton is ready). A reload rebinds the
characters that use the asset and keeps their parameters.

## Demo and tests

- **Demo**: section 10, "Animation" (key 0): the hero on a circle by root motion (speed blend, jump, wave on a
  masked layer, additive lean, look-at, a torch on its hand, footstep sounds from events), two characters with foot
  placement on a ramp and a step, a crowd of up to 512 with the update and skinning times on screen and animation LOD
  as an option (off by default; interpolated or held); in the view
  "2D blend space", a character steered by its velocity through a blend space while it faces one way
  ([Engine demo](demo.md)). Its character is procedural (`tools/assets/make_demo_character.py`, generated and cooked
  at build time; 20 joints, 836 vertices, clips idle, walk, run, jump, wave, lean, walk_back, strafe_l, strafe_r).
- **Tests**: `animation.*` (29 cases: sampling at and between keys, blend weights and phase sync, 2D blend space
  weights (on a grid: in 0..1, a sum of 1 within 1.2e-7, at most 0.0018 of change per mm, exact at the points,
  mirror symmetric) and their pose, phases and root motion, graph errors, masked and additive
  layers, transitions, cross-fades, triggers and exit times, events in order, across loops and with a large dt, root
  motion, two-bone IK reached and unreachable, aim IK, foot placement on a step and a slope, look-at, missing and
  corrupt assets, clips for another skeleton, reload, a parallel update equal to a single-threaded one (with LOD off
  and on), a crowd on one thread, skin palettes; animation LOD: the rates and the updates per frame by screen size,
  interpolated against held poses, root motion spread over the frames, the thresholds' hysteresis and IK off); `cook.cook_skeleton_from_skins`, `cook_animation_keys_and_settings`,
  `cook_animation_errors`, `cook_skinned_model`, and `cook_cli` (the demo's character cooked twice, the same bytes);
  `render.renderer_skins_on_the_gpu`, `renderer_reuses_unchanged_skins` (the same image from copied vertices,
  also when an instance's range in the buffer moves), `renderer_skinned_motion_vectors`, `renderer_rejects_bad_skins`
  (also `render_vulkan.*`); `gpu_device.compute_dispatch`; `demo_animation_metal` and `demo_animation_vulkan` (21 checks
  and a golden capture); `demo_animation_blend2d_<backend>` (the steered character's root motion against its
  velocity, 17 checks and a golden capture); `demo_animation_lod_<backend>` (the crowd of 256 with LOD: the rates,
  the characters sampled and the reused skins from the hero's view and from 45 m, 18 checks, no capture).

## Platforms

- **Mac**: the tests, the demo section and its captures on Metal and on Vulkan through MoltenVK (with the
  validation layer).
- **iOS Simulator**: the `animation.*`, `cook` and render tests in `ios-sim-tests`. Not run on the iPhone.
- **Web**: `quench_animation`'s tests as WebAssembly under Node.js, single-threaded (`wasm-node`). No rendering.

## Limitations

- **Not looked at in motion by the user**: foot sliding, blend pops, the IK on the ramp and the step were checked by
  numbers and still captures only.
- **Retargeting** (clips of one skeleton on another) is not done: clip sets bind to the skeleton they were cooked
  for. Different proportions with the same joint names need a rest-pose correction (not in ozz).
- **No sync markers** beyond normalized time (and a point's `phase`): clips whose foot contacts are not at the same
  phases do not stay in step. No directional blend spaces (polar gradient bands): a space's weights are Cartesian.
- **Linear blend skinning** only (no dual quaternions), four joints per vertex, no blend shapes (morph targets).
- **Animation LOD** has three rates (1, 2, 4) by screen size only (no visibility: a character outside the view or in
  a shadow only still updates at its size's rate), interpolation still poses every frame, and its look in motion
  was not seen by a person (the demo's check mode has an item).
- **One `SceneAssets` per `AssetManager`** (stage 6): a second scene needs a second manager (the demo's section has
  one).
- **Foot placement** is per leg on a single ray; no prediction of the next footfall, no toe or heel separation.
- Only the stand-alone figure of the demo has been animated; no character from a DCC tool was cooked yet.
