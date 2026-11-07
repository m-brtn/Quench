# Quench game reference

Code for the modules in [SKILL.md](SKILL.md), as a game uses them. Names come from the engine's headers
(`include/quench/`); read them for everything not shown. `my_game` stands for the game's target.

## Contents

- CMake
- A lit scene with the renderer (and own surface shading)
- Cooked assets in the scene
- Weather and clouds
- Physics
- Audio
- Input and ImGui
- Animation
- Navigation
- Jobs
- Heat
- Profiling
- Scenario checks, captures and golden images
- iOS

## CMake

The template's `CMakeLists.txt` fetches the engine and returns early in `host-tools` (`QUENCH_HEADLESS`). Add the
modules, the cooked assets and the tests after that return:

```cmake
quench_cook_assets(my_game_cooked
    SOURCE_DIR assets_src
    TEXTURES textures/ground.png            # sRGB color
    DATA_TEXTURES textures/ground_normal.png # linear data: normal, metallic-roughness, masks
    MODELS models/crate.gltf                # glTF or GLB; its textures are cooked next to it
    SOUNDS sounds/thud.wav)                 # WAV, FLAC, MP3, Ogg Vorbis: checked and copied

quench_add_game(my_game
    NAME "My Game"
    BUNDLE_ID com.example.my-game
    ICON icon/icon.png
    ASSETS assets                           # copied as they are
    COOKED_ASSETS my_game_cooked            # cooked files win over plain ones of the same name
    ORIENTATIONS landscape
    SOURCES src/Game.cpp)
target_link_libraries(my_game PRIVATE quench_render quench_physics) # quench_animation for characters
```

`quench_add_game()` defines `QUENCH_GAME_NAME` and `QUENCH_GAME_VERSION` for the sources and copies the assets to
`assets/` next to the executable (into the bundle on iOS). `quench_add_shaders(my_game NAME <set> SOURCE <file>.hlsl
ENTRIES <entry>:vertex <entry>:fragment)` embeds a shader set: `#include <<set>_shaders.h>`, then
`quench::shaders::<set>.find("<entry>")`.

## A lit scene with the renderer

```cpp
#include <quench/app/App.h>
#include <quench/render/Daylight.h>
#include <quench/render/MeshBuilder.h>
#include <quench/render/Renderer.h>

#include <glm/ext/matrix_transform.hpp>

using namespace quench;

class MyGame final : public app::Game {
public:
    Result<void> init(app::App& app) override {
        Result<render::Renderer> renderer = render::Renderer::create(app.device());
        if (!renderer) return Error{renderer.error()};
        m_renderer.emplace(std::move(*renderer));
        m_scene.emplace(app.device());
        render::Scene& scene = *m_scene;

        Result<render::MeshId> ground = scene.addMesh(render::planeMesh(glm::vec2(10.0f), 10.0f).desc("ground"));
        Result<render::MeshId> box = scene.addMesh(render::boxMesh(glm::vec3(0.5f)).desc("box"));
        if (!ground) return Error{ground.error()};
        if (!box) return Error{box.error()};
        const render::MaterialId grey = scene.addMaterial({.baseColor = {0.5f, 0.5f, 0.5f, 1.0f}, .roughness = 0.8f});
        const render::MaterialId red = scene.addMaterial({.baseColor = {0.8f, 0.1f, 0.05f, 1.0f}, .roughness = 0.4f});
        scene.addInstance({.mesh = *ground, .material = grey});
        m_box = scene.addInstance({.mesh = *box, .material = red, .transform = boxTransform(1.0f)});

        const glm::vec3 towardSun = render::skyDirection(35.0f, 140.0f);
        const render::Daylight day = render::daylightFor(towardSun);
        scene.addLight({.type = render::LightType::Directional, .color = day.sunColor, .direction = -towardSun});
        scene.setEnvironment(day.environment);
        return {};
    }

    void update(app::App& app, double frameDt) override {
        (void)frameDt;
        m_scene->setTransform(m_box, boxTransform(float(m_previousY + (m_y - m_previousY) * app.alpha())));
    }

    void render(app::App& app, app::Frame& frame) override {
        (void)app;
        const render::View view{.view = glm::lookAt(glm::vec3(0.0f, 3.0f, 7.0f), glm::vec3(0.0f, 0.5f, 0.0f),
                                                    glm::vec3(0.0f, 1.0f, 0.0f)),
                                .fovY = glm::radians(50.0f),
                                .nearPlane = 0.1f};
        m_renderer->render(*m_scene, view, frame.commands, frame.target, frame.output);
    }

    void shutdown(app::App& app) override {
        (void)app;
        m_scene.reset(); // the scene before the renderer, both before the device
        m_renderer.reset();
    }

private:
    static glm::mat4 boxTransform(float y) { return glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, y, 0.0f)); }

    std::optional<render::Renderer> m_renderer;
    std::optional<render::Scene> m_scene;
    render::InstanceId m_box;
    double m_y = 1.0, m_previousY = 1.0; // set in fixedStep
};
```

- Settings: `m_renderer->setSettings(render::RenderSettings{...})` (anti-aliasing, shadows, bloom, tone curve,
  render scale, the traced effects). `effects()` is what runs after the device's fallbacks, `fallbacks()` says why.
- Handles are plain values; removing a mesh or material needs its instances removed first. `setTransform(...,
  teleport = true)` for a respawn (no motion blur from the old place).
- `render::reversedPerspective(fovY, aspect, near)` is the renderer's projection, for picking and projecting points
  (`scenarioPoint`).

### Own surface shading

A surface shader is an HLSL function the engine lights; materials name it by `MaterialDesc::surface`:

```hlsl
#include "quench/Surface.hlsli"

#if defined(QUENCH_STAGE_FRAGMENT)
QuenchSurface stripes(QuenchSurfaceInput i) {
    QuenchSurface s = quenchStandardSurface(i); // the material's values and textures
    s.albedo *= frac(i.world.x) < 0.5 ? 1.0 : 0.4;
    return s;
}
#endif

#define QUENCH_SURFACE stripes
#include "quench/SurfaceEntries.hlsli"
```

```cmake
quench_add_shaders(my_game NAME my_surface SOURCE shaders/stripes.hlsl
    ENTRIES surface_vs:vertex surface_fs:fragment surface_basic_fs:fragment surface_local_shadows_fs:fragment)
# Optional: the same function in ray hits (traced reflections, indirect light, the probe grid, the path tracer)
quench_add_ray_hits(my_game NAME my_hits SURFACES shaders/stripes.hlsl)
```

```cpp
#include <my_hits_shaders.h>
#include <my_surface_shaders.h>

Result<render::SurfaceId> stripes = m_renderer->addSurface({
    .vertexShader = quench::shaders::my_surface.find("surface_vs"),
    .fragmentShader = quench::shaders::my_surface.find("surface_fs"),
    .basicFragmentShader = quench::shaders::my_surface.find("surface_basic_fs"),
    .localShadowFragmentShader = quench::shaders::my_surface.find("surface_local_shadows_fs"),
    .rayHitShaders = &quench::shaders::my_hits, // only with quench_add_ray_hits()
    .rayHit = 1,                                // the file's number in SURFACES, from 1
    .name = "stripes"});
```

- Without `rayHitShaders` the hits of its materials take the standard surface from the material's values (fine for
  most surfaces; the default). One `quench_add_ray_hits()` set per renderer lists every surface file that needs it.
- In the hits `QUENCH_SURFACE_HIT` is 1: textures read their 256 x 256 copies at level 0, `ddx`, `ddy` and `fwidth`
  are 0 (filter by the footprint only in the raster), `pixel` is 0. A surface that declares resources of its own keeps
  them behind `#if !QUENCH_SURFACE_HIT`. The files of a set are compiled into one source: their macros must not
  clash.
- `RenderStats::rayHitSurfaces` counts the instances whose hits run their function (on a device that traces).
  `docs/render.md`, "Surface shaders" and "Ray hits of surfaces".

## Cooked assets in the scene

```cpp
#include <quench/render/SceneAssets.h>

// members, in this order (destroyed in reverse): the scene, then SceneAssets, then the references
std::optional<render::SceneAssets> m_sceneAssets;
assets::AssetRef<render::ModelAsset> m_crate;

// init, after the scene exists
m_sceneAssets.emplace(*m_scene, app.assets());
m_crate = m_sceneAssets->loadModel("models/crate.qmodel"); // loading; a grey box until it is ready
m_sceneAssets->addModel(m_crate, glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f)));

// update, every frame (the app has updated app.assets() before Game::update)
m_sceneAssets->update();

// shutdown, before the scene
m_crate = {};
m_sceneAssets.reset();
```

`m_crate.ready()` turns true when the model is uploaded; a failed load shows a magenta box and logs one line. A
model's own textures are cooked next to it (`models/crate_0.ktx2`) and loaded with it.

Textures for the game's own materials: the handle is valid at once (white until loaded), so the material can be
added in `init`. Keep the references as members, released before `SceneAssets` like the models.

```cpp
m_groundColor = m_sceneAssets->loadTexture("textures/ground.ktx2");         // AssetRef<render::TextureAsset>
m_groundNormal = m_sceneAssets->loadTexture("textures/ground_normal.ktx2"); // cooked from DATA_TEXTURES
render::MaterialDesc ground{.baseColor = {0.8f, 0.8f, 0.8f, 1.0f}, .roughness = 0.8f};
ground.textures[0] = m_groundColor->texture(); // 0 base color, 1 metallic-roughness, 2 normal map, 3 emissive
ground.textures[2] = m_groundNormal->texture();
const render::MaterialId groundMaterial = m_scene->addMaterial(ground);
```

One texture set for every device (Basis Universal, transcoded on the loading job to what the GPU samples):

```cmake
quench_cook_assets(my_game_portable SOURCE_DIR assets_src
    TEXTURES textures/ground.png DATA_TEXTURES textures/ground_normal.png
    TEXTURE_FORMAT basis)  # BASIS_CODEC auto (default: UASTC for data, ETC1S for color), etc1s or uastc
```

Loaded the same way; `m_groundColor->basisCodec()` names the file's codec and `format()` what it became.

## Weather and clouds

The weather is headless (`quench_weather`, which `quench_render` links); the renderer draws its clouds and their
shadows. It is a pure function of its settings, cells and clock, so scripted runs give the same sky every time.

```cpp
#include <quench/render/Renderer.h>
#include <quench/render/Scene.h>
#include <quench/weather/Weather.h>

weather::Weather m_weather; // member

// init
weather::Settings s;
s.coverage = 0.35f;                         // 0 clear .. 1 overcast
s.type = weather::CloudType::Cumulus;       // Stratus, Cumulus, Towering, Cumulonimbus
s.wind = {.direction = {1, 0}, .speed = 10}; // m/s at the clouds, the way the air moves
m_weather.setSettings(s);
m_weather.setTime(1200.0);                  // a start that is not the field's first shape
m_shower = m_weather.addCell({.position = {2000, -8000}, .radius = 3000, .precipitation = 1}); // one raining cloud
render::RenderSettings rs = m_renderer->settings();
rs.clouds = render::Clouds::Volumetric;     // off by default; Layer is the cheap 2D layer and the fallback
rs.cloudResolution = 0.5f;                  // half size, accumulated over frames
m_renderer->setSettings(rs);

// Game::update, every frame
m_weather.update(frameDt);
m_scene->setWeather(m_weather);             // a copy: set it again after each update
const float rain = m_weather.precipitation(playerPosition);                 // 0..1 above a point
const float far = m_weather.distanceToPrecipitation({playerPosition.x, playerPosition.z}); // m, for distant rain
```

`m_renderer->effects().clouds` says what is drawn (Volumetric falls back to Layer by `gpu::Caps`; clouds need
`RenderSettings::sky`), `stats().cloudCover` how overcast it is around the camera. The sun's light on surfaces goes
through the clouds' shadow (`cloudShadows`), the sky's ambient light and the fog grey with the cover, and surfaces
reflect a cube of the sky with its clouds (`skyCubeSize`, `skyCubeFacesPerFrame`). Costs and phone settings:
`docs/weather.md`, "Phones and fallbacks"; `examples/demo/sections/WeatherSection.cpp` (presets, a shower cell, the
sun's day).

Rain falls where the weather's precipitation does (a cell, or `Settings::precipitation`); the renderer draws it,
the weather remembers how wet the ground got, and `audio::WeatherAudio` plays it:

```cpp
#include <quench/audio/WeatherAudio.h>

std::optional<audio::WeatherAudio> m_rainSound; // member; destroyed before the AudioSystem

// init
rs.rain = render::Rain::Medium;             // off by default; Low on phones (no splashes); High for close-ups
rs.rainShafts = true;                       // with clouds: the shafts under raining clouds from afar
m_puddles = m_scene->addMaterial({.baseColor = {0.4f, 0.4f, 0.4f, 1}, .roughness = 0.7f, .puddles = 1.0f});
m_lake = m_scene->addMaterial({.baseColor = {0.05f, 0.08f, 0.1f, 1}, .roughness = 0.05f, .water = true});
m_rainSound.emplace(app.audio());           // the engine's default sounds (below), bus "effects"

// Game::update, every frame, before setWeather
m_weather.setWetnessFocus({cameraPosition.x, cameraPosition.z}); // the remembered wetness follows the camera
m_rainSound->update({.precipitation = m_weather.precipitation(listener),
                     .distanceToRain = m_weather.distanceToPrecipitation({listener.x, listener.z}),
                     .cover = std::max(m_renderer->stats().rainCover, 0.0f)}, frameDt);
```

```cmake
quench_weather_sounds(my_game_weather_sounds)                    # the defaults: quench/weather/rain_*.wav
quench_add_game(my_game ... COOKED_ASSETS my_game_cooked my_game_weather_sounds)
```

- `MaterialDesc::wetness` (1 by default) scales how wet a material gets, `puddles` (0..1) collects standing water
  in its hollows, `water` (a lake) shows only the drops' rings. Surfaces under a roof stay dry through the rain
  occlusion map (the shadow casters, 64 m around the camera). A surface file that defines `QUENCH_SURFACE_NO_RAIN`
  before `quench/SurfaceEntries.hlsli` is never wet.
- `weather.wetness(xz)` gives `{ground, water}` for a game's own use (tyres, footsteps); `Settings::wettingTime`,
  `dryingTime`, `puddleFillTime`, `puddleDryTime` set how fast. `setTime` estimates the wetness of that time, so a
  scripted check that jumps in time gets the same ground.
- The sound's layers (`audio::WeatherLayer`): `RainLight`, `RainHeavy` (crossfaded by the precipitation),
  `RainFar` (the nearest rain within `farDistance`), `RainCover` (on the roof, by the cover). `setSound(layer,
  "sounds/my_rain.wav")` replaces one (a cooked sound; "" silences it), `setEnabled(false)` turns the engine's off
  and `WeatherAudio::targetGains(state, farDistance)` gives its mix rules to a game's own sounds.
- `effects().rain` says what is drawn (none in the path tracer's frame), `stats().rainAtCamera`, `rainStreaks`,
  `rainCover` what happened. Costs and phone settings: `docs/weather.md`, "Cost"; the demo's Weather section, view
  Ground.

## Physics

```cpp
#include <quench/physics/PhysicsWorld.h>

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <glm/gtc/quaternion.hpp> // glm::quat, glm::slerp, glm::mat4_cast

struct Pose {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// members
std::unique_ptr<PhysicsWorld> m_world;
JPH::BodyID m_body;
Pose m_previous{.position = {0.0f, 4.0f, 0.0f}}, m_current = m_previous;

// init
m_world = std::make_unique<PhysicsWorld>(&app.jobs());
JPH::BodyInterface& bodies = m_world->bodies();
bodies.CreateAndAddBody(JPH::BodyCreationSettings(new JPH::BoxShape(JPH::Vec3(10.0f, 0.5f, 10.0f)),
                                                  JPH::RVec3(0.0f, -0.5f, 0.0f), JPH::Quat::sIdentity(),
                                                  JPH::EMotionType::Static, PhysicsLayers::kStatic),
                        JPH::EActivation::DontActivate);
m_body = bodies.CreateAndAddBody(JPH::BodyCreationSettings(new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f)),
                                                           JPH::RVec3(0.0f, 4.0f, 0.0f), JPH::Quat::sIdentity(),
                                                           JPH::EMotionType::Dynamic, PhysicsLayers::kMoving),
                                 JPH::EActivation::Activate);
m_world->optimizeBroadPhase();

// fixedStep
m_previous = m_current;
m_world->step(float(dt));
JPH::RVec3 p;
JPH::Quat q;
m_world->bodies().GetPositionAndRotation(m_body, p, q);
m_current = {glm::vec3(p.GetX(), p.GetY(), p.GetZ()), glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ())};

// update: between the last two ticks
const float alpha = float(app.alpha());
const glm::vec3 position = glm::mix(m_previous.position, m_current.position, alpha);
const glm::quat rotation = glm::slerp(m_previous.rotation, m_current.rotation, alpha);
m_scene->setTransform(m_box, glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation));
```

Changes from one-frame input (a jump on `pressed`) are queued in `update` and applied to the bodies in the next
`fixedStep` (`JPH::BodyInterface::SetLinearVelocity`, `JPH::BodyInterface::AddImpulse`).

`examples/demo/sections/PhysicsSection.cpp` has the whole loop (stacks, impulses, removal). Destroy the world in
`shutdown` or with the game; the job system outlives the game.

## Audio

```cpp
#include <quench/audio/AudioSystem.h>

assets::AssetRef<audio::SoundAsset> m_thud; // member: released before the app's audio system goes

// init
audio::AudioSystem& a = app.audio();
m_thud = a.load("sounds/thud.wav"); // decoded on a job

// update, when something lands
a.play(m_thud, {.bus = a.bus("effects"), .volume = 0.8f, .spatial = true,
                .emitter = {.position = position}});
a.setListener({.position = cameraPosition, .forward = cameraForward});
```

`play(path)` loads and plays in one call; a one-shot whose sound is still loading starts when it is ready (dropped
after `AudioSettings::maxStartDelay`). Music streams: `a.playMusic("music/theme.ogg", {.crossfade = 2.0f})`. The
buses come from `AppDesc::audio.buses` (default `music`, `effects`, `ui` under the master bus).

## Input and ImGui

```cpp
#include <quench/app/Input.h>
#include <imgui.h>

using app::Binding;
using app::GamepadButton;
// init
m_jump = app.input().addAction("jump", {*Binding::key("Space"), Binding::gamepad(GamepadButton::South)});
// update (one-frame states belong here, not in fixedStep)
if (app.input().pressed(m_jump)) m_jumpQueued = true;
// ui
const app::WindowRect safe = app.safeArea();
ImGui::SetNextWindowPos(ImVec2(float(safe.x) + 12.0f, float(safe.y) + 12.0f), ImGuiCond_FirstUseEver);
ImGui::Begin(QUENCH_GAME_NAME, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
if (!app.deterministic()) ImGui::Text("%.1f fps", app.stats().fps);
ImGui::End();
```

While ImGui wants the keyboard or the mouse, key and mouse bindings read as up. The scenario value `action:jump` is
the action's value.

## Animation

Link `quench_animation`. The game owns the system (it registers the skeleton, clip and graph asset types on the
app's asset manager):

```cpp
#include <quench/animation/AnimationSystem.h>

std::optional<animation::AnimationSystem> m_animation; // member, before the AssetRefs it hands out
m_animation.emplace(app.assets(), app.jobs());          // init
m_animation->update(frameDt);                           // Game::update, after setting parameters
```

Characters, graphs, skins, root motion, IK and blend shapes: `docs/animation.md`, "Use";
`examples/demo/sections/AnimationSection.cpp`.

## Navigation

Link `quench_navigation`. The mesh is built on the app's jobs, tile by tile across frames; crowds and queries keep a
reference to it:

```cpp
#include <quench/navigation/Crowd.h>
#include <quench/navigation/NavMesh.h>
#include <quench/navigation/NavQuery.h>

// members, in this order (destroyed in reverse): the mesh, then what uses it
std::optional<navigation::NavMesh> m_navMesh;
std::optional<navigation::Crowd> m_crowd;

// init: the level's walkable triangles (counter-clockwise from above), area types with costs, links
navigation::Geometry level = levelTriangles();               // vertices, indices, an area id per triangle
level.boxes.push_back({.min = {-3, -0.5f, 2}, .max = {4, 0.5f, 7}, .area = 1}); // mud
level.links.push_back({.start = {-6.5f, 2.4f, -3.8f}, .end = {-6.5f, 0, -1.6f}, .id = 1}); // a jump down
navigation::BuildSettings settings;
settings.agent = {.radius = 0.35f, .height = 1.8f, .maxClimb = 0.4f, .maxSlope = 45};
settings.areas = {{"ground", 1.0f}, {"mud", 5.0f}};
Result<navigation::NavMesh> mesh = navigation::NavMesh::build(std::move(level), settings, app.jobs());
if (!mesh) return false; // mesh.error() says why
m_navMesh.emplace(std::move(*mesh));
m_crowd.emplace(*m_navMesh);
m_guard = *m_crowd->addAgent({0, 0, 5}, {.radius = 0.35f, .maxSpeed = 2.0f});
m_crowd->setTarget(m_guard, {8, 0, 7});

// Game::update, every frame: adds finished tiles and starts queued builds; never waits
m_navMesh->update();
// Game::fixedStep
m_crowd->update(float(dt));
for (const navigation::CrowdEvent& e : m_crowd->events())
    if (e.type == navigation::CrowdEvent::Type::LinkStarted) startJump(e.agent, e.start, e.end); // e.link: Link::id
```

An agent placed before its tiles are in is `AgentMove::Lost` and stands until they are. Moving crates and doors are
obstacles (`addObstacle`, `setObstacle`, `removeObstacle`: only their tiles are rebuilt); paths, raycasts and random
points come from a `navigation::NavQuery` (one per thread); `navigation::DebugGeometry` draws the mesh, paths and
agents with any renderer. A level cooked ahead (`NAVMESHES` in `quench_cook_assets()`, a `.json` of model files and
settings, `docs/assets.md`) loads as an asset:

```cpp
#include <quench/navigation/NavMeshAsset.h>

navigation::registerNavMeshAsset(app.assets());                               // init, once
m_levelNav = app.assets().load<navigation::NavMeshAsset>("levels/yard.qnav"); // loads on jobs
if (!m_navMesh && m_levelNav->data())                                         // a later frame
    if (Result<navigation::NavMesh> mesh = navigation::NavMesh::create(m_levelNav->data(), app.jobs()))
        m_navMesh.emplace(std::move(*mesh));
```

`docs/navigation.md`; `examples/demo/sections/NavigationSection.cpp` (two groups, links, obstacles, debug lines).

## Jobs

```cpp
Job m_bake; // member
m_bake = app.jobs().run([this] { bakeNavigation(); }, JobPriority::Low); // starts now
if (m_bake.valid() && m_bake.done()) useNavigation();                   // a later frame
app.jobs().parallelFor(count, 64, [&](std::uint32_t begin, std::uint32_t end) { work(begin, end); });
```

`parallelFor` returns a `Job`; `app.jobs().wait(job)` runs queued jobs on the main thread meanwhile, so it suits
work this frame needs, not loads. With `--worker-threads 0` every job runs on the main thread: check the game
still works so.

## Heat

The thermal reaction is opt-in (the template turns it on with `"adapt_to_heat": true` in its settings). The defaults:
nothing at fair; 60 Hz and 0.8 of the render scale from serious on; 30 Hz, 0.6 and `reduceEffects()` at critical;
lifted after 60 s at a lower state. The App applies the frame rate; the game applies the rest to what the renderer
gets, never to the player's settings:

```cpp
#include <quench/app/ThermalGovernor.h> // also through App.h

// init, or when the player turns the setting on (disable() lifts every limit at once)
app.thermalGovernor().enable({.serious = {.maxFrameRate = 60.0, .renderScaleFactor = 0.75f}});
// update: the player's settings stay as they are; the renderer gets the reduced copy
render::RenderSettings s = m_playerSettings;
const app::ThermalGovernor& heat = app.thermalGovernor();
s.renderScale = std::max(heat.renderScale(s.renderScale), 0.25f);
if (heat.reduceEffects()) s.reflections = render::Reflections::Sky;
m_renderer->setSettings(s);
```

Checks without a hot device: `--thermal-state serious`, or in a scenario `thermal_state serious` (`thermal_state
system` gives it back), then `expect thermal_reaction == 2` and `expect frame_rate_limit == 60`.

## Profiling

Configure the game with the engine's Tracy client (a preset of its own, as the engine's `profile`: Release with
`"QUENCH_TRACY": "ON"`); the build also gets `tracy-capture` and `tracy-csvexport` in its build directory. Zones in
the game's code:

```cpp
#include <quench/core/Profile.h>

void MyGame::fixedStep(quench::app::App& app, double dt) {
    QUENCH_PROFILE_ZONE("game.fixed_step");                // to the end of the scope; a literal name
    {
        QUENCH_PROFILE_ZONE("game.ai");
        updateEnemies(dt);
    }
    QUENCH_PROFILE_PLOT("game.enemies", m_enemies.size()); // a value over time
}
// A name made at run time, text on the zone, a named job:
QUENCH_PROFILE_ZONE_DYNAMIC(level.name);
QUENCH_PROFILE_ZONE_TEXT(path);
m_bake = app.jobs().run("game.bake", [this] { bakeNavigation(); }, JobPriority::Low);
```

Without `QUENCH_TRACY` the macros are nothing and their arguments are not evaluated: no work for the profile alone
inside them. Record an offscreen scenario and get the zones per perf region (the script is in the engine's
checkout: the local override's path, or `build/<preset>/_deps/quench-src`):

```sh
python3 <engine>/tools/tracy_capture.py --build-dir build/profile --output build/profile/tracy/run -- \
    build/profile/my_game --script scenarios/perf.txt --audio-device null
```

It writes `run.tracy` and `run_zones.csv` (count and time per frame, mean, p50, p99 and self time per zone) and
prints the top zones; `--expect game.ai` fails the run without that zone. The user opens `run.tracy` in Tracy's
GUI (0.15.0 or newer); agents never start the GUI.

## Scenario checks, captures and golden images

A scenario (`scenarios/frame.txt`):

```scenario
# The box falls, Space lifts it again; the landing plays a sound; the crate model loads; the frame is captured.
until box_y < 0.55 3
expect box_y > 0.45
until audio_played >= 1 1
until crate_ready == 1 2
mark box_y
tap Space
wait 0.1
expect d_box_y > 0.5
capture frame.png
quit
```

`box_y` and `crate_ready` come from `Game::scenarioValue` (`m_crate.ready() ? 1.0 : 0.0`): wait for what loads
before capturing it. `until NAME OP VALUE TIMEOUT` waits up to TIMEOUT seconds of app time. `audio_measure 0.3`
measures the newest 0.3 s of the mix and sets `audio_rms`, `audio_peak`, `audio_freq` (the strongest frequency) for
`expect`; the mix is real in offscreen runs, only not played.

The tests (`CMakeLists.txt`, inside the template's `if(NOT CMAKE_CROSSCOMPILING)`):

```cmake
set(capture_dir ${CMAKE_CURRENT_BINARY_DIR}/captures)
add_test(NAME frame COMMAND my_game --window 640x360 --frame-size 1280x720 --user-dir ${capture_dir}
         --script ${CMAKE_CURRENT_SOURCE_DIR}/scenarios/frame.txt)
set_tests_properties(frame PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL line" FIXTURES_SETUP frame_capture)

# The engine's image compare tool, built in the game: mean channel difference at most 0.5, at most 0.1% of the
# pixels off by more than 16 (flags change both); exit code 1 when the images differ, 2 when one cannot be read.
add_executable(my_game_image_compare ${quench_SOURCE_DIR}/tools/imagecompare/ImageCompare.cpp)
target_link_libraries(my_game_image_compare PRIVATE SDL3::SDL3-static)
add_test(NAME frame_golden COMMAND my_game_image_compare ${capture_dir}/frame.png
         ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/metal/frame.png --diff ${capture_dir}/frame_diff.png)
set_tests_properties(frame_golden PROPERTIES FIXTURES_REQUIRED frame_capture)
```

The first time, run the `frame` test, look at `build/<preset>/captures/frame.png`, and copy it to
`tests/golden/metal/frame.png` if it is right. Captures are SDR PNGs with ImGui; `--frame-size` fixes their size.
A one-off capture without a script: `my_game --capture /tmp/frame.png --capture-at 2 --quit-after 2`.

## iOS

```sh
cmake --preset host-tools && cmake --build --preset host-tools    # macOS quench_shaderc and quench_cook
cmake --preset ios-sim-release && cmake --build --preset ios-sim-release
xcrun simctl boot "iPhone 18 Pro Max"                               # boots without the Simulator app's window
xcrun simctl install booted build/ios-sim-release/my_game.app
xcrun simctl launch --console --terminate-running-process booted com.example.my-game \
    --capture /tmp/my_game.png --capture-at 2 --quit-after 2.5 --no-audio
cmake --preset ios-xcode -DQUENCH_IOS_TEAM=<team id>               # device: Xcode project, Automatic signing
```

In the Simulator host paths work for `--capture` and `--script`. Device builds, `devicectl` installs and launches,
bundle id suffixes and the user directory on the phone: `docs/app.md`, "iOS runs". Ask the user before using their
phone, and ask them to check what is on its screen.
