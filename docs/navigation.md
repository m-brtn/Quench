# Navigation

`quench_navigation`, a headless module on Recast & Detour (2026-11-07; the user's queue, item 4, from
[Other modules](roadmap.md#other-modules-when-a-game-needs-them)): navmeshes built from level triangles at run time,
tile by tile on the job system across frames, or cooked into `.qnav` assets; area types with costs, off-mesh links
(jumps, ladders, teleporters); dynamic obstacles and geometry changes that rebuild only their tiles; paths, raycasts
and random points; crowds with avoidance and link events; debug geometry for any renderer. The engine demo's
"Navigation" section shows it ([Engine demo](demo.md)). Written for the engine, not moved from a game; neither game
uses it yet. What was run where is under [Platforms](#platforms); nobody has looked at it in motion yet. All
documentation: [index](README.md).

## Use

```cpp
#include <quench/navigation/Crowd.h>
#include <quench/navigation/NavMesh.h>
#include <quench/navigation/NavQuery.h>

namespace nav = quench::navigation;
nav::Geometry level;            // triangles (counter-clockwise from above), an area id per triangle
level.vertices = ...;           // meters, Y up
level.indices = ...;
level.boxes.push_back({.min = {-3, -0.5f, 2}, .max = {4, 0.5f, 7}, .area = 1});        // mud
level.links.push_back({.start = {-6.5f, 2.4f, -3.8f}, .end = {-6.5f, 0, -1.6f}, .id = 1}); // a jump down

nav::BuildSettings settings;
settings.agent = {.radius = 0.35f, .height = 1.8f, .maxClimb = 0.4f, .maxSlope = 45};
settings.tileSize = 48;
settings.areas = {{"ground", 1.0f}, {"mud", 5.0f}};
quench::Result<nav::NavMesh> mesh = nav::NavMesh::build(std::move(level), settings, app.jobs()); // starts jobs

nav::Crowd crowd(*mesh);
nav::NavQuery query(*mesh);
nav::AgentId guard = *crowd.addAgent({0, 0, 5}, {.radius = 0.35f, .maxSpeed = 2.0f});
crowd.setTarget(guard, {8, 0, 7});
nav::ObstacleId crate = mesh->addObstacle({.shape = nav::ObstacleShape::Box, .position = {1, 0.6f, 4},
                                           .halfExtents = {0.6f, 0.6f, 0.6f}});

// Every frame, on the thread that owns the mesh (never waits: it adds finished tiles and starts queued builds)
mesh->update();
// Every fixed step
crowd.update(float(dt));
for (const nav::CrowdEvent& e : crowd.events())
    if (e.type == nav::CrowdEvent::Type::LinkStarted) startJump(e.agent, e.start, e.end); // e.link: the Link::id
const nav::AgentState s = crowd.state(guard); // position, velocity (for the walk speed), move, arrived, link
```

`Result<nav::Path> path = query.findPath(a, b, &filter)` gives the corridor of polygons and the straightened points
(each link start marked with its id); `nearestPoint`, `raycast`, `randomPoint` and `randomPointAround` take an
optional `nav::Filter` (area costs and included or excluded areas; `Filter::of(mesh)` has the mesh's costs).

## Navmeshes

- **Tiles and layers**: the mesh is a grid of square tiles (`tileSize` cells of `cellSize` meters). Recast voxelizes
  each tile into DetourTileCache layers, one per walkable floor above another (a bridge over a passage is two
  layers in its tiles, up to `maxLayersPerTile`, more dropped with a warning); the navmesh tiles are made from the
  layers. Layers are kept uncompressed (no compression library in the build).
- **Run-time build**: `NavMesh::build` takes the geometry and starts one job per tile (all its layers) on the job
  system, at low priority; `update()` adds the finished tiles (only there do tiles change) and starts the next
  builds; `ready()` says when all are in; `finish()` waits (tools, tests, loading screens). Without worker threads
  (the single-threaded web build), `update(jobSeconds)` runs queued jobs for up to that long. `buildNavMeshData`
  builds the same data in one call and waits.
- **Areas**: up to 16 area types (`kMaxAreas`), each a name and a cost (a multiplier of the length walked through
  it, at least 1 so the search stays exact); triangles carry an area id, `AreaBox`es mark boxes after them,
  `kNotWalkable` cuts a box out. Steps up to `maxClimb` and slopes up to `maxSlope` are walkable.
- **Off-mesh links** (`Link`): one way or both, a radius at the start, an area and the game's id; both ends must be
  on the mesh. Paths mark where a link starts; crowds report crossing it.
- **Obstacles**: cylinders and boxes (with a yaw), grown by the agent radius and marked into the layers of the tiles
  they touch: cut out (`kNotWalkable`) or marked as an area (a fire as a costly area). `addObstacle`, `setObstacle`
  (moved: the tiles it left and entered are rebuilt) and `removeObstacle` queue rebuilds on jobs; the mesh's other
  tiles stay usable meanwhile.
- **Geometry changes**: `rebuild(geometry, min, max)` voxelizes the tiles touching a box again (a door, a collapsed
  wall, a placed building).
- **References**: each layer of a tile column has a fixed tile slot, and a rebuilt tile gets the slot's next salt:
  the same build gives the same polygon references whatever order tiles finish in, and a path through a replaced
  tile sees that it is gone (a crowd agent replans).
- **Determinism**: `contentHash()` hashes every tile's data as built; equal inputs and settings give the same hash
  whatever the number of threads (`navigation.tiled_build_is_deterministic`).
- `stats()`: tiles, layers with polygons, polygons, pending tile columns, obstacles, memory, the last build's wall
  time; `revision()` counts tile changes (to redraw debug geometry).

## Queries

`NavQuery` keeps Detour's search memory, so one thread uses it at a time: one per job for queries in parallel, between
two `NavMesh::update()` calls. `maxNodes` (4096) bounds a search (longer ones return partial paths). Start and end
points are looked for within `searchExtents()` (2 m sideways, 4 m up and down by default). An end that cannot be
reached gives a partial path to the nearest reachable point (`Path::partial`); no mesh near the start or the end is
an error.

## Crowds

`Crowd` wraps DetourCrowd: agents with a radius, height, speed and acceleration steer along their corridors, avoid
each other by sampled velocities (`Avoidance`: Off, Low, Medium, Good, High), keep apart (`separation`), follow
replaced tiles and cross links.

- **Targets and velocities**: `setTarget` (the nearest point of the mesh), `clearTarget`, `setVelocity` (steering by
  a player's input instead). An agent has arrived within `arrivalDistance` of its target (default: its radius); the
  crowd reports `Arrived` once. Agents that press together at a shared target (a group) need a larger arrival
  distance or less separation: the demo uses 0.6 m and a separation of 0.5.
- **Filters**: 16 per crowd (`setFilter(index, filter)`, `AgentParams::filter`). Detour's straight-line shortcut of
  a corridor ignores area costs, so the engine uses it only for agents whose filter gives every area the same cost;
  the others keep the costs (they walk around the demo's mud; [Dependencies](dependencies.md#added-for-navigation)).
- **Links**: `CrowdEvent::LinkStarted` and `LinkFinished` with the link's id and its ends as crossed;
  `AgentState::move` is `OnLink` meanwhile, with `linkProgress` from 0 to 1. Detour moves the agent along a straight
  line at half its speed; the game draws the crossing (the demo lifts its characters on an arc and starts a jump
  clip).
- **Lost agents**: an agent with no mesh under it (its tiles not built yet, or cut out under it) is `Lost` and stands
  until a polygon is near it again.
- `route(agent)`: the corners ahead, for drawing.

## Debug geometry

`DebugGeometry` is two plain vertex lists (triangles and lines, linear RGBA) for any renderer: `addNavMesh` (the
polygons by area color, their walls and inner edges, links as arcs with a circle at the start), `addObstacles`,
`addPath` and `addAgents` (a circle per agent and its velocity, for a whole crowd or some agents). The demo draws them
in a game pass of its own after the transparent stage, depth-tested without writing depth.

## Cooking

`quench_cook navmesh IN OUT [--settings FILE] [--depfile FILE]` builds the navmesh of a glTF, GLB or `.qmodel`'s
triangles (static meshes of the default scene; skinned meshes and non-triangle primitives are left out with a note)
into a `.qnav` file: every tile's layers (before obstacles), the settings and the links. Its settings come from
`<input stem>.nav.json` next to the source, or `--settings`, listed in the depfile:

```json
{"agent": {"radius": 0.35, "height": 1.8, "maxClimb": 0.4, "maxSlope": 45},
 "cellSize": 0.15, "cellHeight": 0.1, "tileSize": 48,
 "areas": [{"name": "ground", "cost": 1}, {"name": "mud", "cost": 5}],
 "surfaces": {"Mud": "mud", "Fence": "none", "Decals": "ignore"},
 "boxes": [{"min": [-3, -0.5, 2], "max": [4, 0.5, 7], "area": "mud"}],
 "links": [{"start": [-6.5, 2.4, -3.8], "end": [-6.5, 0, -1.6], "radius": 0.5, "id": 1}]}
```

`surfaces` gives a node, mesh or material name (the first that matches, in that order) an area type, `"none"` (not
walkable: it blocks) or `"ignore"` (left out of the geometry); other surfaces are the first area type. Unknown keys
and area names that are not defined are errors; surface names that match nothing are notes. The same input gives
the same bytes (`cook_cli`). In CMake: `quench_cook_assets(name SOURCE_DIR dir NAVMESHES levels/yard.gltf)` writes
`levels/yard.qnav`.

At run time `.qnav` files are an asset type ("navmesh", `NavMeshAsset.h`), read and checked (magic, version, size,
XXH64 checksum) on the asset manager's jobs:

```cpp
nav::registerNavMeshAsset(app.assets());
auto level = app.assets().load<nav::NavMeshAsset>("levels/yard.qnav");
// once level->data() is set (empty while loading and after a failure):
quench::Result<nav::NavMesh> mesh = nav::NavMesh::create(level->data(), app.jobs()); // tiles on jobs, as build()
```

A reload replaces the data and bumps the asset's version; the game makes a new mesh of it. `NavMesh::data()` returns
a run-time build's layers as cooked data, and `writeNavMeshData` / `readNavMeshData` write and read the format.

## Threads

Tile builds run as jobs on the engine's job system (`App::jobs()`), one per tile, at low priority; the thread
that owns the mesh calls `update()` once per frame and never waits for them. Queries and crowds read the mesh on that
thread, or in jobs between two `update()` calls. `Crowd::update` runs on the calling thread (DetourCrowd is not
split over jobs). With zero workers the builds run inside `update()` within its time slice.

## Cost

Release build (`release` preset), the Mac (Apple M4 Pro, 12 job threads), 2026-11-07:

| What | Time |
|---|---|
| Test level of `navigation.timings` (40 m x 40 m, two floors, a ramp, a bridge, 12 posts; 5 x 5 tiles of 48 cells, 120 polygons): build | 1.3 to 3.4 ms |
| A path between two random points (two random points and the path) | 7 to 15 us |
| Crowd update, 25 agents moving | 0.02 to 0.05 ms |
| Crowd update, 100 agents moving | 0.13 to 0.20 ms |
| The demo's level (28 m x 20 m, 4 x 3 tiles, 14 layers, 56 polygons): first build at the section's start, wall time with its loads | 4 to 9 ms (alone), 58 to 193 ms (in the demo batches, with other sections' work in the same frames) |
| The demo's level built again (section reset) | 1.3 to 1.5 ms |
| One moving obstacle's tiles rebuilt | about 0.56 ms |
| The demo's crowd step, 12 and 24 agents | about 0.017 and 0.025 ms |

Wall times of builds run from the first request to the last tile added in `update()`, so they include the frames in
between. The `headless` preset (Debug) took 2 to 6 times as long (a build 6.4 to 7.5 ms, a path 43 to 44 us, crowds
0.11 to 0.12 and 0.63 to 0.64 ms). As WebAssembly under Node.js (one thread, the build inside `update()`): a build
29.2 ms, a path 22.7 us, crowds 0.091 and 0.267 ms.

## Demo and tests

- **Demo**: section 13, "Navigation" (no key; the shoulders and the menu reach it): a level of two floors (decks at
  2.4 m) joined by a ramp, stairs and a narrow bridge, a mud field at cost 5 (a slider from 1 to 10), jumps down the
  ledges as one-way links, two crates and a barrel that move (obstacles, their tiles rebuilt on jobs), and two groups
  of mannequins (orange and green, 1 to 12 each) that patrol their rounds or go where a click or tap on a floor sends
  them, avoiding each other; their walk and run follow their speed, a link crossing lifts them on an arc with the jump
  clip. The navmesh, the paths and the agents are drawn as overlays (switches). The stats show the navmesh, the
  obstacles, the agents' states, the jumps, the probe path's length and its meters in the mud; with a real clock
  also the last build's time, the crowd step and the last click's path search ([Engine demo](demo.md)).
- **Tests**: `navigation.*` (16 cases: a small level built, bad input as errors, a path over a ramp and a bridge,
  area costs and excluded areas changing the path, a link used and reported (in the path and by a crowd agent),
  an obstacle blocking and unblocking a passage, a geometry rebuild changing tiles, a crowd of 8 crossing without
  overlap (closest 0.858 m with radius 0.4), a crowd following rebuilt tiles, a crowd keeping area costs (none of
  its steps in the mud at cost 5, more than 30 at cost 1), raycasts and seeded random points, debug geometry, the
  tiled build's bytes and tile hashes equal with 0 and 4 worker threads, a cooked round trip through the asset
  manager, a corrupt file as an error, and the timings above); `cook.cook_navmesh`, `cook.cook_navmesh_errors`,
  `cook_cli` (the demo's crate
  cooked twice as a navmesh, the same bytes, the depfile naming the glTF and its buffer);
  `demo_navigation_<backend>` (26 checks and a golden capture: the build, the agents, the probe path around the mud
  at cost 5 and through it at cost 1, no agent in the mud, a click sending the orange group down a jump link with all
  6 arriving, the obstacles' rebuilds with no agent lost, agents on a floor at least 0.5 m apart).

## Platforms

- **Mac**: the tests in the `headless`, `debug`, `release`, `asan` and `tsan` presets; the demo section and its
  capture on SDL GPU's Metal driver, the native Metal backend and Vulkan through MoltenVK (offscreen).
- **iOS Simulator**: `navigation.*` in `ios-sim-tests`. Not run on the iPhone.
- **Web**: the tests as WebAssembly under Node.js, single-threaded (`wasm-node`). No rendering.

## Limitations

- **Not looked at in motion**: the crowd's avoidance, the turns, the walk and run blend and the link arcs were
  checked by numbers and still captures only (the demo's check mode has an item, `navigation.crowd`).
- **Link crossings** are Detour's: a straight line at half the agent's speed, which the game can only decorate (the
  demo's arc); a game-driven crossing (a ladder clip, a jump of its own length) is remaining work.
- **No detail meshes**: tiles made by DetourTileCache's builder have none, so heights on a polygon are those of its
  vertices (stairs are a slope under the steps; agents' feet are placed by the game, foot placement where it has it).
- **The demo builds its level at run time** (it shows the build); the cooked path is covered by the cook and asset
  tests and `cook_cli`, not by a demo level.
- **Obstacles move in steps**: the demo moves its crates every frame but updates their navmesh obstacles after
  0.25 m, so each move rebuilds a few tiles rather than every frame's.
- **Layers are uncompressed** (no compression library); a large world's memory grows with it. No tile streaming by
  distance yet (`StreamingGrid` could drive `NavMesh::rebuild` or per-tile cooked data later).
- **Crowd updates are single-threaded** (DetourCrowd's); about 0.2 ms for 100 agents on the M4 Pro.
