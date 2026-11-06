# Jobs, assets and the cook

Stage 6 of the [Roadmap](roadmap.md): the engine's job system (`quench_core`), the asset system (`quench_assets`,
with the render integration `SceneAssets` in `quench_render`) and the asset cooker `quench_cook` (a host tool). The
engine demo's "Assets" section shows them ([Engine demo](demo.md)). Shader hot reload is not built yet (see
[Limitations](#limitations)). Stage 7 adds sounds as an asset type and the cook's `sound` command
([Audio](audio.md)); stage 8 skeletons, clip sets and animation graphs, skins in `.qmodel` and the cook's
`skeleton` and `animation` commands ([Animation](animation.md)).

## Job system

`quench::JobSystem` (`include/quench/core/Jobs.h`) is the engine's one thread pool, on enkiTS. Everything that runs
work off the calling thread uses it: asset reads and decodes, Jolt's physics jobs, the cook's BC7 encoding, and game
work.

```cpp
quench::JobSystem& jobs = app.jobs(); // the app's; tests and tools make their own: JobSystem jobs({.workerThreads = 2});
quench::Job decode = jobs.run([&] { decodeSomething(); }, quench::JobPriority::Low);
quench::Job rows = jobs.parallelFor(rowCount, 4, [&](std::uint32_t begin, std::uint32_t end) { encode(begin, end); });
quench::Job after = jobs.run([&] { combine(); }, quench::JobPriority::Normal, std::array{decode, rows});
jobs.wait(after); // runs queued jobs on this thread meanwhile
```

- **Jobs** have three priorities (`High`, `Normal`, `Low`) and may depend on other jobs (`after`); a `Job` is a cheap
  handle, `done()` can be asked from any thread. `parallelFor` calls its function over ranges of at least `minRange`
  items. Submitting and waiting is allowed on the creating thread and inside jobs; `wait` runs other jobs while it
  waits, so a job that waits does not deadlock the pool. Jobs must not throw.
- **Threads**: by default hardware threads - 1 workers (at most 15); `--worker-threads N` sets the app's. Every worker
  calls `raiseWorkerThreadPriority()` (user-interactive QoS on Apple platforms) from enkiTS's thread start callback:
  the frame thread waits for the workers, and a lower QoS would be a priority inversion.
- **Without threads** (`workerThreads = 0`, and always in builds without threads such as the single-threaded web
  build) jobs run on the creating thread inside `wait`, `waitAll` and `runFor(seconds)`. Code that must progress
  across frames without blocking calls `runFor` once per frame; the asset manager does (4 ms per update by default).
  The demo runs the same way with `--worker-threads 0`.
- **Jolt**: `PhysicsWorld(JobSystem*)` runs Jolt's jobs on the job system through `JoltJobSystem`
  (`src/physics/`, a `JPH::JobSystemWithBarrier`), so there is no second pool; Jolt splits a step over at most
  `Settings::maxConcurrency` threads (default: the job system's threads, at most 5). With `nullptr` or a job system
  without workers, Jolt runs single-threaded on the thread that calls `step()`. Jolt's results do not depend on the
  thread count (the demo's physics capture is byte-identical with Jolt's own pool, single-threaded and on the job
  system).

## Assets

### IDs and the manager

An `AssetId` is the XXH64 hash of the asset's path as `quench::fs` takes it (`"models/crate.qmodel"`), the same on
every platform and in the cook. `AssetManager` (`include/quench/assets/AssetManager.h`) caches assets of every type by
ID; the app owns one (`App::assets()`, updated by the app every frame before `Game::update`).

```cpp
quench::render::SceneAssets sceneAssets(scene, app.assets()); // registers "texture" and "model"
auto crate = sceneAssets.loadModel("models/crate.qmodel");  // AssetRef<ModelAsset>, loading
quench::render::ModelInstanceId placed = sceneAssets.addModel(crate, transform);
// every frame, after the app's AssetManager::update(): sceneAssets.update();
```

- **References**: `load<T>(path, priority)` returns an `AssetRef<T>` (reference counted, main thread only). A path
  already cached returns the same asset. Unreferenced assets stay cached until their type's memory budget is exceeded,
  then the least recently released go first (`setBudget(type, {cpu, gpu})`; `evictUnreferenced()` drops them all).
  Failed loads are dropped as soon as nobody refers to them.
- **States**: loading, ready or failed. A load reads the file through `quench::fs` and decodes it on a job; the
  main-thread part (`Asset::finish()`, the GPU upload of textures and meshes) runs in `update()` while the frame's
  upload budget lasts (`AssetManagerSettings::uploadBytesPerFrame`, 8 MB by default; at least one upload per update).
  At most `maxLoadsInFlight` loads (4) run at once; lower priorities load first (`setPriority` reorders). A failed load
  (missing file, corrupt data, unsupported format) logs one line and leaves the placeholder; it never crashes.
- **Placeholders** are the asset type's: `TextureAsset::texture()` is a valid texture handle from the load on,
  white while loading and a magenta checker after a failure; a placed model shows a gray box while loading and a
  magenta box after a failure.
- **Reload**: `reload(id)` reads the file again and swaps the new data in under the same ID and handles; the old data
  stays until then, and also if the reload fails. With hot reload on (`--hot-reload`,
  `AssetManagerSettings::hotReload`) the manager polls the modification time of every loaded file twice a second and
  reloads what changed.
- **Simulated latency** (`--asset-latency S`, `setSimulatedLatency`): every load waits S seconds before it starts, to
  see and test the placeholders.
- **Stats**: `stats()` gives counts (loading, ready, failed, queued, decoding, waiting for upload), totals (loads,
  reloads, evictions) and per type the cached count, memory and budget; `forEach` lists the assets.
- **A new asset type** derives from `Asset` (`decode` on a job, `uploadBytes`, `finish` and `unload` on the main
  thread, `residentMemory`) and is registered with `registerType<T>(name, factory, budget)`. Sounds are one
  (`audio::SoundAsset`, type `"sound"`, registered by `audio::AudioSystem`: decoded to float PCM on the load job, CPU
  memory only, budget `AudioSettings::soundBudget`; [Audio](audio.md#sounds)), and stage 8 skeletons, clip sets and
  animation graphs (`animation::SkeletonAsset`, `ClipSetAsset`, `GraphAsset`, types `"skeleton"`, `"animation"`,
  `"animgraph"`, registered by `animation::AnimationSystem`; CPU memory only, budgets in `AnimationSettings`;
  [Animation](animation.md)).

### Textures and models in a scene

`SceneAssets` (`include/quench/render/SceneAssets.h`) loads cooked files into a `render::Scene`:

- `TextureAsset`: a KTX2 file (below). Where the device cannot sample BC7 (`gpu::Caps`), BC7 is decoded to RGBA8 on
  the CPU during the load (`decodedOnCpu()`). `SceneAssetsSettings::textureSkipLevels` leaves out the largest mip
  levels.
- `ModelAsset`: a `.qmodel` file: one scene mesh and material per part, its textures loaded as `TextureAsset`s.
  `SceneAssetsSettings::modelLod` picks the cooked level of detail to upload. `addModel` places a model (any number
  of times); the placed instances follow its state, and are rebuilt after a reload. Skinned parts (stage 8) become
  skinned scene meshes; `ModelAsset::skins()` names their joints and inverse bind matrices (for
  `animation::AnimationSystem::addSkin`), and `setJointPalette(instance, skin, palette)` poses them (kept across a
  rebuild).
- One `SceneAssets` per manager, because it registers the types `"texture"` and `"model"` there (registering one
  twice asserts): a game with two scenes gives the second its own `AssetManager` on the same job system and calls
  its `update()` every frame (the demo's Animation section does). It must be destroyed before the scene, after every
  reference to its assets.

### Streaming by distance

`StreamingGrid` (`include/quench/assets/StreamingGrid.h`): square cells on a grid in the XZ plane around any number
of focus points. A cell is loaded when a focus comes within `loadRadius` of it and unloaded when every focus is
farther than `keepRadius`; `update(focuses)` returns the cells to load (nearest first) and to unload. What a cell
holds is the game's, typically `AssetRef`s loaded with the distance as priority, so the manager's in-flight limit,
priorities and budgets apply. Generalized from TrailForge's `TerrainTiles` (streamed physics terrain tiles); no
synchronous loads.

## Cooked formats

- **Textures**: KTX 2.0 files of one 2D image with its mip chain, without supercompression, in RGBA8, BC7 (both
  with or without sRGB) or ASTC 4x4 (read, not written yet), at most 32768 x 32768 and 16 levels. `readKtx2`,
  `writeKtx2` and `decodeBc7` are in `include/quench/assets/Texture.h`; the reader checks every size and offset
  against the file before it allocates, so a corrupt file is an error, not a crash or a huge allocation.
- **Models**: `.qmodel` (`include/quench/assets/Model.h`): the meshes of a glTF scene with the node transforms
  applied, the engine's 64-byte vertex (position, normal, uv, linear color, tangent), index ranges per level of
  detail, and glTF metallic-roughness materials that name their textures by asset path (base color,
  metallic-roughness, normal, emissive). Vertex and index streams are compressed with meshoptimizer's codecs. The
  format has a version; files of another version fail to load. Version 2 (stage 8) added skins: per skin the joint
  names and inverse bind matrices, per skinned part a skin index and per vertex four 16-bit joint indices and
  weights (`ModelSkinVertex`, weights summing to 65535). Skinned parts keep their vertices in the skin's bind space
  (no node transform applied). Version 3 (stage 11) adds the tangent: xyz along +u, w the sign of the bitangent
  (`w * cross(normal, tangent)` points toward -v, the normal map's +y), as glTF's `TANGENT`. Versions 1 and 2 are
  rejected with an error that says to cook the source again (they have no tangents; a cooked file is rebuilt from
  its source by `quench_cook_assets()` whenever the cook changes, so only files cooked by hand need it).
- **Tangents**: the cook keeps a glTF's `TANGENT` (turned with the node transform, w flipped under a mirroring
  transform) and otherwise generates MikkTSpace tangents (`assets::generateTangents`, `include/quench/assets/
  Tangents.h`), the frame normal map bakers use; vertices that need two tangents (mirrored uv seams) are split.
  Procedural meshes get the same through `render::computeTangents`; the engine's mesh builders write theirs
  analytically (they match MikkTSpace's, `render.mesh_builder_tangents_match_mikktspace`).
- **Skeletons and clip sets**: `.qskel` and `.qanim` ([Animation](animation.md#files)): ozz-animation archives in a
  container with a magic, a version, the payload size and an XXH64 checksum, so a truncated or corrupt file fails to
  load (ozz itself checks only with asserts). A clip set holds every clip of one source with its events, flags and
  root motion, and the signature of its skeleton.

## quench_cook

The cook (`tools/cook/`) turns source files into cooked files. It is a host tool: games do not link it.

```sh
quench_cook texture tiles/stone.png assets/tiles/stone.ktx2 [--format bc7|rgba8] [--linear] [--no-mips] [--max-size N]
quench_cook model models/crate.gltf assets/models/crate.qmodel --asset-root assets [--lods N] [--lod-ratio R]
            [--lod-error E] [--scale S]
quench_cook sound sounds/hit.flac assets/sounds/hit.flac            # checked, copied as it is
quench_cook sound sounds/engine.mp3 assets/sounds/engine.wav [--channels 1|2]
quench_cook skeleton characters/hero.gltf assets/characters/hero.qskel
quench_cook animation characters/hero.gltf assets/characters/hero.qanim [--skeleton FILE] [--clips FILE]
# all: [--depfile FILE] [--verbose]; --version, --help
```

- **Textures**: PNG and JPEG (stb_image) or KTX2 (block formats are passed through; RGBA8 is cooked again with the
  file's transfer function). Mips are box-filtered (sRGB images in linear light). BC7 through bc7enc_rdo, block rows
  spread over the job system; level 0 needs both sides to be multiples of 4 (Metal's rule), otherwise the texture is
  written as RGBA8 with a note. `--linear` for data (normals, metallic-roughness).
- **Models**: glTF and GLB (fastgltf; external buffers and images, data URIs, embedded images; the extensions
  `KHR_mesh_quantization` and `KHR_materials_emissive_strength`; files with `KHR_texture_transform` or
  `KHR_materials_unlit` are accepted, the extension is ignored with a note). Primitives of the default scene are merged per material with their node transforms; smooth
  normals are computed where they are missing; meshoptimizer orders vertices and indices for the vertex cache and
  fetch, and `meshopt_simplify` makes the levels of detail (each about `--lod-ratio` of the one before, while the
  error stays below `--lod-error`). The model's images are cooked next to it as `<model>_<image>.ktx2` (sRGB) or
  `<model>_<image>_linear.ktx2` (data), and the materials name them by asset path (relative to `--asset-root`).
  Skinned meshes (stage 8) keep their skin: joint names, inverse bind matrices, four joints and weights per vertex
  (`JOINTS_0` and `WEIGHTS_0`, renormalized; a `JOINTS_1` set is dropped with a note); `--scale` is refused for
  them (scale the source).
- **Skeletons** (stage 8): the joints of the file's skins with their ancestors and the non-mesh nodes below joints
  (attachment points such as a hand socket), or the animated nodes of a file without skins; rest transforms from
  the nodes.
- **Animations** (stage 8): every animation of the file becomes a clip of one `.qanim`, for the skeleton of
  `--skeleton` (another glTF with the same joints) or of the input. Step and linear keys are kept, cubic splines
  sampled at 30 Hz, linear rotation steps wider than 0.2 rad subdivided spherically (glTF slerps, ozz does not);
  ozz's optimizer reduces the keys (1 mm at 10 cm by default). Clip settings (loop, additive, events, root motion,
  the tolerance) come from `<input stem>.anim.json` or `--clips` ([Animation](animation.md#cooking)); unknown keys
  and unknown clips are errors.
- **Sounds**: WAV, FLAC or MP3 (miniaudio's decoders). The file is decoded completely to check it (a file that does
  not decode, or decodes to no frames, fails the cook). An output with the input's extension gets the input's bytes;
  a `.wav` output from another format, or with `--channels 1|2` (mix down to mono or up to stereo), is a 16-bit PCM
  WAV at the source's sample rate. No resampling, normalization or Vorbis output: the mixer resamples per voice, and
  files of other formats are refused ("a sound is cooked to its own format or to .wav").
- **Deterministic**: the output depends only on the inputs and the options (no time stamps, no thread order; a test
  cooks with 0 and 3 worker threads and compares). Files are written to a temporary file and renamed, so a running
  game with hot reload never reads half a file; a model's textures are rewritten only when they change.
- **Depfile**: `--depfile` lists every file read (the glTF, its buffers and images, the clip settings), so Ninja
  cooks again when any of them changes.

### In CMake

```cmake
quench_cook_assets(my_game_cooked SOURCE_DIR assets_src
    TEXTURES tiles/stone.png tiles/grass.png
    DATA_TEXTURES masks/splat.png          # linear
    MODELS models/crate.gltf models/rock.glb
    SOUNDS sounds/hit.wav music/theme.mp3  # checked, same name
    SKELETONS characters/hero.gltf         # .qskel
    ANIMATIONS characters/hero.gltf characters/hero_extra.glb  # .qanim, settings from <stem>.anim.json
    [SKELETON characters/hero.gltf]        # the skeleton the ANIMATIONS are for (default: their own)
    [FILES credits.txt levels/one.json]    # copied as they are
    [OUTPUT_DIR dir] [TEXTURE_FORMAT bc7|rgba8] [TEXTURE_MAX_SIZE n] [MODEL_OPTIONS --lods 2 ...])
quench_add_game(my_game ... ASSETS assets COOKED_ASSETS my_game_cooked)
```

`quench_cook_assets` (`cmake/QuenchCook.cmake`) adds a target that cooks each file into the same relative path (with
`.ktx2`, `.qmodel`, `.qskel` or `.qanim`; sounds keep their name and format). `FILES` are copied unchanged
(texts, settings, licences; paths relative to `SOURCE_DIR`, no `..`), so a game can ship files from outside its
`ASSETS` directory: the demo reads the engine's `LICENSE` and `THIRD_PARTY_LICENSES` that way. A call with `FILES`
only needs no `quench_cook` (it works in cross builds without `host-tools`). `quench_add_game(... COOKED_ASSETS ...)`
merges the cooked directories with `ASSETS` into the game's `assets/`: next to the executable on macOS, into the
bundle on iOS (Ninja), and through a staging directory in Xcode projects. Once (2026-11-06, not reproduced since) an
Xcode build left files merged into the staging directory in the same build out of the app; `tools/package_demo.sh`
builds `quench_demo_assets` first and compares the two. On macOS the copy is `copy_directory_if_different`:
rebuilding the game's `<game>_assets` target while the game runs with `--hot-reload` cooks what changed and the game
reloads exactly those files.

The cook is built from source in host builds. Cross builds (iOS) take it from the `host-tools` preset through
`QUENCH_HOST_COOK` (the iOS presets set it; build `host-tools` first). The web build cooks nothing. The cook's own
dependencies (fastgltf with its bundled simdjson and simdutf, stb) are fetched at configure time in host builds and
built only when the cook is (`EXCLUDE_FROM_ALL`).

## Demo content and tests

The demo's sources (`examples/demo/assets_src/`) are procedural, written by `tools/assets/make_demo_assets.py`
(CC0, `CREDITS.md` there): eight 64 x 64 tile patterns, a crate (glTF, external buffer and PNG), a pillar (GLB with
an embedded JPEG and PNG) and a ball (glTF, data URI, vertex colors, no normals). `examples/demo/assets/` adds a
deliberately broken `.qmodel`. The demo's sounds are not in git: `tools/assets/make_demo_sounds.py` (Python standard
library, procedural, CC0) writes five WAV files into the build tree at build time and `quench_cook_assets(...
SOUNDS ...)` checks and copies them ([Audio](audio.md#demo-and-tests)). Its animated character is not in git either:
`tools/assets/make_demo_character.py` (procedural, CC0) writes a mannequin with its clips as glTF and its clip
settings at build time, and `quench_cook_assets(... MODELS ... SKELETONS ... ANIMATIONS ...)` cooks it
([Animation](animation.md#demo-and-tests)).

Tests: `core.jobs_*` (also without workers and under stress), `physics.*` (Jolt on the job system, single-threaded,
two worlds on one pool, worker priority), `assets.*` (the manager with a test asset type, KTX2,
BC7 decoding, `.qmodel`, the streaming grid), `audio.sound_assets` (the sound type: loading, plays of a loading
sound, missing and broken files, a late one-shot dropped), `animation.missing_and_corrupt_assets_hold_the_rest_pose`
and `animation.reload_rebinds_and_keeps_parameters`, `cook.*` and `cook_cli` (sounds, skeletons, clips and skinned
models included),
`render.scene_assets_*` and `render_vulkan.scene_assets_*` (pixels of placeholders, loaded textures and models,
failures; Vulkan through MoltenVK), and the demo's `demo_assets_metal` / `demo_assets_vulkan`.

## Limitations

- **Shader hot reload** is not built (stage 6 remaining work); only asset files reload. Hot reload works where the
  asset directory is a directory of the file system (the Mac); on the iPhone it needs an asset server (later).
- **Texture formats**: the cook writes BC7 and RGBA8 only. ASTC (written by no cook yet) and Basis Universal
  transcoding are needed for Android and the web; until then a device without BC sampling gets BC7 decoded to RGBA8
  on the CPU (four times the memory).
- **Materials**: normal and emissive textures are cooked and named in the model, but the standard surface does not
  read them yet (no normal maps in `quench_render`); occlusion textures, `KHR_texture_transform` and unlit materials
  are dropped with a note; morph targets, cameras and lights of glTF files are not cooked.
- **One scene per manager**: `SceneAssets` registers its asset types on its manager, so a second scene needs a
  second `AssetManager` (with its own cache: a file used by both scenes loads twice).
- **Streaming** loads and unloads by distance; there is no memory budget across cells beyond the manager's per-type
  budgets of unreferenced assets, and the demo's streaming is of textures and models only (no physics bodies).
- **Timing**: loads finish in real time, so a scripted run whose capture depends on when a load finished is not
  byte-identical with TAA (the demo's assets check uses FXAA).
