# ECS

`quench_ecs` is an entity component system for a game's own objects: enemies, projectiles, pickups, anything a game
has many of and wants to process by what it is made of. It is optional and headless. Nothing else in the engine uses
it, and engine APIs keep taking plain handles (a scene instance, a physics body, a crowd agent), which a game stores
in components. Asked by the user on 2026-11-14 for a game with many kinds of objects; built the same day. All
documentation: [index](README.md).

It is [Flecs](https://github.com/SanderMertens/flecs) 4.2.0, not a wrapper around it: a game uses Flecs's own C++
API for entities, components, queries, systems and observers (its manual:
<https://www.flecs.dev/flecs/md_docs_2Docs.html>). The engine adds the few things that make it fit:

| What | How |
|---|---|
| Systems that follow the game interface | `ecs::World::fixedStep(dt)` runs the systems of the fixed step, `ecs::World::update(dt)` every other system |
| The engine's log | Flecs's messages go to `quench::log`, category `ecs` |
| No threads of its own | Flecs's thread functions are taken out; asking it for worker threads is an error it reports |
| Counters | `ecs::World::stats()`: entities, tables, systems, calls and wall time of the two steps |

## Use

Link `quench_ecs` and include `<quench/ecs/World.h>` (it includes `<flecs.h>`).

```cpp
struct Position { glm::vec3 value; };
struct Velocity { glm::vec3 value; };
struct Visual { quench::render::InstanceId instance; }; // an engine handle in a component

quench::ecs::World world;
flecs::world& ecs = world.flecs();

// A system of the fixed step: Game::fixedStep runs it.
ecs.system<Position, const Velocity>("Move").kind<quench::ecs::FixedStep>().each(
    [](flecs::iter& it, size_t, Position& p, const Velocity& v) { p.value += v.value * it.delta_time(); });

// A system of the frame: Game::update runs it, once per frame however many fixed steps ran.
ecs.system<const Position, const Visual>("Draw").each(
    [&scene](const Position& p, const Visual& v) { scene.setTransform(v.instance, placed(p.value)); });

// An observer gives the engine handle back when the entity ends.
ecs.observer<const Visual>().event(flecs::OnRemove).each(
    [&scene](const Visual& v) { scene.removeInstance(v.instance); });

ecs.entity().set<Position>({{0, 1, 0}}).set<Velocity>({{1, 0, 0}}).set<Visual>({instance});

void Game::fixedStep(App&, double dt) { world.fixedStep(float(dt)); }
void Game::update(App&, double frameDt) { world.update(float(frameDt)); }
```

## The two steps

- **The fixed step.** A system made with `.kind<ecs::FixedStep>()` runs in `World::fixedStep(dt)`. Its systems run
  in the order they were made. Simulation belongs here: the app calls `Game::fixedStep` with the same `dt` every
  time, and exactly once per frame in scripted runs, so the result is the same in every run.
- **The frame.** Every other system runs in `World::update(dt)`, through Flecs's own phases (`flecs::OnUpdate` when
  a system names none, `flecs::OnStore` for what hands data to the renderer, and the others). Systems of one phase
  run in the order they were made.
- Both take a time above 0 (a `QUENCH_ASSERT`). Flecs reads 0 as "measure the time yourself", which a deterministic
  run must never get, so a value at or below 0 becomes one microsecond in builds without assertions.
- Changes to what an entity is made of (adding or removing a component, deleting the entity) inside a system are
  deferred by Flecs and applied before the next system that needs them, at the latest at the end of the step.

## Rules

- **One thread uses a world.** Systems run on the thread that calls `fixedStep` and `update`, a game's main thread.
  Work for other threads goes to the engine's job system from inside a system (`app.jobs()`), as everywhere else.
  Flecs's own worker threads (`set_threads`, `multi_threaded()`) are not available: the engine has one thread pool.
- **Destroy the world before what its components point at.** An observer that gives a handle back runs when the
  world is destroyed too: declare the scene (or the physics world) before the `ecs::World`, or reset the world first
  in `shutdown`.
- **Queries and other Flecs objects go before the world.** A `flecs::query` kept as a member ends itself in its
  world when it is destroyed: after the world is gone, that is a crash at exit (found by the first game on the
  module, 2026-11-14). Reset such members (`m_query = {};`) before the `ecs::World` is destroyed, or declare them
  after it. Plain `flecs::entity` and `flecs::system` members are ids and may outlive the world unused.
- **A handle may be gone before the observer runs** when both are removed in one step: check with
  `Scene::contains()` before using it.
- **No reflection.** The Flecs build leaves out its reflection, JSON, script, REST and statistics addons
  (`cmake/Dependencies.cmake`), so there is no Flecs Explorer and no serialization of a world: a game saves what it
  needs itself.

## What is built

`include/quench/ecs/World.h`, `src/ecs/World.cpp`, Flecs from its two distribution files as `quench_flecs` with a
custom build: the C++ API, systems, pipelines, timers, modules, cached queries and prefabs.

The demo's section 16, `ecs` ([Engine demo](demo.md#sections)): a swarm of 600 entities (up to 4000) made of a
position, a velocity, a lifetime and a scene instance; six systems of the fixed step spawn, steer, move, charge,
discharge and age them, a system of the frame writes their transforms into the scene, and three observers give
instances back and change colours. Its panel switches systems off and on while they run.

## Verified

2026-11-14, on the Mac (M4 Pro), Release only:

- `quench_ecs_tests`, 5 cases, 5 passed: the two steps run their own systems with the time given; systems of the
  fixed step run in the order they were made; systems move, mark and delete entities and the counters follow; Flecs
  reports no threading and a Flecs warning arrives in the engine's log; two worlds are independent.
- The demo's `ecs` part in the three batches (SDL GPU's Metal driver, the native Metal backend, Vulkan through
  MoltenVK), 28 checks each: the swarm fills to its target; with a lifetime of half a second more than 2400 entities
  end and are replaced while the scene's instance count stays the same (none leaked); with a system switched off its
  part stops; a burst; a clear gives every instance back. Its capture is identical on the three backends.

Not run: the Debug build (the assertions), the sanitizers, the tests as WebAssembly under Node.js, the iOS Simulator,
a phone, a browser. Not measured: the cost of a step (the section shows it; no numbers were recorded). The user has
not looked at the section in motion.

## Open

- Worker threads for systems through the engine's job system (Flecs lets an OS layer supply its "tasks"; not tried).
- Reflection and saving a world, if a game needs them.
- The cost of large worlds on a phone.
