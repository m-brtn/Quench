# Jobs, assets and the cook

Stage 6 of the [Roadmap](roadmap.md): the engine's job system (`quench_core`), the asset system (`quench_assets`,
with the render integration `SceneAssets` in `quench_render`) and the asset cooker `quench_cook` (a host tool). The
engine demo's "Assets" section shows them ([Engine demo](demo.md)). Shader hot reload is the app's and the GPU
layer's ([App shell](app.md#shader-hot-reload)). Stage 7 adds sounds as an asset type and the cook's `sound` command
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
  reloads what changed. On a device, see [Hot reload on a device](#hot-reload-on-a-device).
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

### Hot reload on a device

A device's app bundle is read-only and out of reach of the Mac's build. Development builds therefore read an asset
from the **asset overlay**, `asset_overlay/<path>` in the user directory, when that file exists, and from the bundle
otherwise (`fs::makeOverlayBackend`, installed by the app; reads, streamed opens, `exists` and modification times
all go to the overlay first). Development builds are the ones without `NDEBUG` (Debug); the CMake option
`QUENCH_ASSET_OVERLAY` (`AUTO`, `ON`, `OFF`) turns it on in another build (`-DQUENCH_ASSET_OVERLAY=ON` for a Release
build on the phone) or off. The app logs "asset overlay: files in asset_overlay/ of the user directory replace
bundled assets" at startup when it is on. Release builds never read the overlay.

`tools/push_assets.sh` copies changed files from the Mac into the overlay through `devicectl` (Xcode's device tool;
the device unlocked and paired):

```sh
xcodebuild -project build/ios-xcode/Quench.xcodeproj -target quench_demo_assets -configuration Release -sdk iphoneos build
tools/push_assets.sh --device <udid> --bundle-id org.quench.demo --app "Quench Demo" \
    build/ios-xcode/examples/demo/quench_game/quench_demo/assets   # --watch: again every second
```

The first run on a device and app records the SHA-256 of every file and copies nothing (`--all` copies everything);
later runs copy each file whose content changed, one `devicectl device copy to` per file (about a second each;
devicectl keeps the file's modification time). Content, not time, decides: a build that cooks a file again with the
same result gives it a new time (the first version of the script copied 32 files for one change). With hot reload
on (`--hot-reload`, or the switch in the demo's Assets section) the running game sees the overlay file's time and
loads it. Checked on the iPhone 17 (2026-11-07, the demo built with `-DQUENCH_ASSET_OVERLAY=ON`,
`examples/demo/scenarios/hot_reload_device.txt`): a tile texture turned by 90 degrees, cooked and pushed (1 file,
under a second) was reloaded about a second later; the reload counter went from 0 to 1 and the captures before and
after show the tile's stripes turned. The script removes nothing on the device: `devicectl device copy to
--remove-existing-content true` deleted files outside its destination folder (on 2026-11-07 the demo's whole user
directory on the iPhone 17; reproduced in devicectl's `temporary` domain). To read the bundled files again, push the
original files or delete the app.

### Textures and models in a scene

`SceneAssets` (`include/quench/render/SceneAssets.h`) loads cooked files into a `render::Scene`:

- `TextureAsset`: a KTX2 file (below). Where the device cannot sample BC7 (`gpu::Caps`), BC7 is decoded to RGBA8 on
  the CPU during the load (`decodedOnCpu()`). ASTC 4x4 is sampled where `gpu::Caps::astcTextures` is set (the Mac's
  three backends, the iOS Simulator and the iPhone); elsewhere the load fails with an error and the texture stays the
  placeholder (the runtime has no ASTC decoder). A Basis Universal file is transcoded on its loading job to what the
  device samples ([Basis Universal](#basis-universal)): `basisCodec()` names its codec, `format()` the result,
  `decodeMilliseconds()` how long the job's read, decode or transcode took; `SceneAssets::textureSupport()` gives
  the formats `gpu::Caps` allow, and one log line per `SceneAssets` says what each codec becomes on the device.
  `SceneAssetsSettings::textureSkipLevels` leaves out the largest mip levels.
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
  with or without sRGB) or ASTC 4x4 (LDR, with or without sRGB), at most 32768 x 32768 and 16 levels. `readKtx2`,
  `writeKtx2` and `decodeBc7` are in `include/quench/assets/Texture.h`; the reader checks every size and offset
  against the file before it allocates, so a corrupt file is an error, not a crash or a huge allocation. Or KTX 2.0
  files of Basis Universal data, ETC1S (BasisLZ supercompression) or UASTC LDR 4x4 (Zstandard supercompression),
  one 2D image with its mips ([Basis Universal](#basis-universal)): `readBasisInfo`, `basisTarget`, `transcodeBasis`
  and `prepareTexture` (which takes either kind) in the same header. ETC2 RGB8 and RGBA8 exist as `PixelFormat`s
  for what Basis Universal files are transcoded to; the cook does not write them.
- **Models**: `.qmodel` (`include/quench/assets/Model.h`): the meshes of a glTF scene with the node transforms
  applied, the engine's 64-byte vertex (position, normal, uv, linear color, tangent), index ranges per level of
  detail, and glTF metallic-roughness materials that name their textures by asset path (base color,
  metallic-roughness, normal, emissive). Vertex and index streams are compressed with meshoptimizer's codecs. The
  format has a version; files of another version fail to load. Version 2 (stage 8) added skins: per skin the joint
  names and inverse bind matrices, per skinned part a skin index and per vertex four 16-bit joint indices and
  weights (`ModelSkinVertex`, weights summing to 65535). Skinned parts keep their vertices in the skin's bind space
  (no node transform applied). Version 3 (stage 11) adds the tangent: xyz along +u, w the sign of the bitangent
  (`w * cross(normal, tangent)` points toward -v, the normal map's +y), as glTF's `TANGENT`. Version 4 (stage 8's
  blend shapes, 2026-11-07) adds per mesh its morph targets: per target a name, a default weight and one offset per
  vertex (`ModelMorphVertex`: position, normal and tangent offsets, 36 bytes, compressed with the vertex codec).
  The cook writes version 4; version 3 files are still read (as models without blend shapes), so files cooked by
  an earlier engine keep loading, but an engine before version 4 cannot read the new files. Versions 1 and 2 are
  rejected with an error that says to cook the source again (they have no tangents; a cooked file is rebuilt from
  its source by `quench_cook_assets()` whenever the cook changes, so only files cooked by hand need it). At most
  256 targets per mesh.
- **Tangents**: the cook keeps a glTF's `TANGENT` (turned with the node transform, w flipped under a mirroring
  transform) and otherwise generates MikkTSpace tangents (`assets::generateTangents`, `include/quench/assets/
  Tangents.h`), the frame normal map bakers use; vertices that need two tangents (mirrored uv seams) are split.
  Procedural meshes get the same through `render::computeTangents`; the engine's mesh builders write theirs
  analytically (they match MikkTSpace's, `render.mesh_builder_tangents_match_mikktspace`).
- **Skeletons and clip sets**: `.qskel` and `.qanim` ([Animation](animation.md#files)): ozz-animation archives in a
  container with a magic, a version, the payload size and an XXH64 checksum, so a truncated or corrupt file fails to
  load (ozz itself checks only with asserts). A clip set holds every clip of one source with its events, flags and
  root motion, and the signature of its skeleton.
- **Navmeshes**: `.qnav` ([Navigation](navigation.md#cooking), 2026-11-07): every tile's voxel layers (before
  obstacles), the build settings with the area types and the off-mesh links, in a container with a magic, a version,
  the payload size and an XXH64 checksum; the `"navmesh"` asset type (`navigation::registerNavMeshAsset`) reads and
  checks it on the loading job, and `NavMesh::create` builds the tiles from it on jobs.

## quench_cook

The cook (`tools/cook/`) turns source files into cooked files. It is a host tool: games do not link it.

```sh
quench_cook texture tiles/stone.png assets/tiles/stone.ktx2 [--format bc7|astc|rgba8|basis]
            [--basis-codec auto|etc1s|uastc] [--linear] [--no-mips] [--max-size N]
quench_cook model models/crate.gltf assets/models/crate.qmodel --asset-root assets [--lods N] [--lod-ratio R]
            [--lod-error E] [--scale S]
quench_cook sound sounds/hit.flac assets/sounds/hit.flac            # checked, copied as it is
quench_cook sound sounds/engine.mp3 assets/sounds/engine.wav [--channels 1|2]
quench_cook skeleton characters/hero.gltf assets/characters/hero.qskel
quench_cook animation characters/hero.gltf assets/characters/hero.qanim [--skeleton FILE] [--clips FILE]
            [--retarget FILE]
quench_cook navmesh levels/yard.gltf assets/levels/yard.qnav [--settings FILE]   # also a .qmodel
# all: [--depfile FILE] [--verbose]; --version, --help
```

- **Textures**: PNG and JPEG (stb_image) or KTX2 (block formats are passed through; RGBA8 is cooked again with the
  file's transfer function). Mips are box-filtered (sRGB images in linear light). BC7 (desktops) through bc7enc_rdo,
  ASTC 4x4 (phones) through astc-encoder 5.7.1 (its medium preset, the LDR sRGB or linear profile), both at 8 bits
  per texel with block rows spread over the job system; level 0 needs both sides to be multiples of 4 (Metal's
  rule), otherwise the texture is written as RGBA8 with a note. `--linear` for data (normals, metallic-roughness).
  ASTC is the closer of the two to the source: 57.1 and 56.3 dB PSNR against BC7's 43.6 and 44.4 dB on the test
  gradients (sRGB, linear), 54.9 and 54.1 dB against 50.3 and 51.2 dB on the demo's crate planks
  (`cook.cook_astc_matches_the_source`). The cook times of the two were not compared. `--format basis` writes a
  Basis Universal file from the same levels ([Basis Universal](#basis-universal)): `--basis-codec etc1s` (small,
  lower quality), `uastc` (about BC7's quality) or `auto` (the default: UASTC for `--linear` data and normal maps,
  ETC1S for color). Basis Universal's encoder v2.50 with its defaults: ETC1S at quality level 128 of 255, UASTC at
  pack level 2 without rate-distortion optimization, Zstandard at level 6; one thread per texture (the build cooks
  files in parallel). A level 0 that is not a multiple of 4 is written all the same (a note says that devices get
  RGBA8 from it). A Basis Universal KTX2 input is copied unchanged with `--format basis`, and transcoded with the
  other formats; a BC7 or ASTC input is decoded and encoded again (a note says it is compressed twice).
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
- **Blend shapes** (morph targets, 2026-11-07): a primitive's `targets` (`POSITION`, `NORMAL` and `TANGENT`
  offsets) become the mesh's morph targets, named by the glTF mesh's `extras.targetNames` (as Blender and three.js
  write them; otherwise `<mesh>.target<index>`) with the node's `weights` (else the mesh's) as their defaults. The
  offsets are turned like the vertices: positions by the node transform's linear part, normal and tangent offsets so
  that the renderer's normalized sum is the transformed morphed vector (an offset is scaled by 1 over the length of
  its transformed base vector). Without the file's normals the cook computes the targets' normals as it does the
  base's, from the moved positions; without the file's tangents (MikkTSpace) the tangent offsets are 0. A mesh with
  blend shapes is a part of its own per node (its weights are the node's); its vertices are welded with their
  offsets, so two vertices alike except in a target stay apart. Weight animations (`weights` channels) are cooked
  by `quench_cook animation` into the clips' float channels, named like the blend shapes
  ([Animation](animation.md#cooking)). `quench_cook model --verbose` counts the blend shapes.
- **Skeletons** (stage 8): the joints of the file's skins with their ancestors and the non-mesh nodes below joints
  (attachment points such as a hand socket), or the animated nodes of a file without skins; rest transforms from
  the nodes.
- **Animations** (stage 8): every animation of the file becomes a clip of one `.qanim`, for the skeleton of
  `--skeleton` (another glTF with the same joints) or of the input. Step and linear keys are kept, cubic splines
  sampled at 30 Hz, linear rotation steps wider than 0.2 rad subdivided spherically (glTF slerps, ozz does not);
  ozz's optimizer reduces the keys (1 mm at 10 cm by default). Morph target weights (glTF `weights` channels,
  since 2026-11-07) become float channels named like the model's blend shapes (`.qanim` version 2 when a clip has
  them). Clip settings (loop, additive, events, root motion,
  the tolerance) come from `<input stem>.anim.json` or `--clips` ([Animation](animation.md#cooking)); unknown keys
  and unknown clips are errors. With `--retarget` (a bone map, since 2026-11-07) the clips are retargeted onto
  `--skeleton`'s skeleton, which may have other joint names, proportions and rest pose
  ([Animation](animation.md#retargeting)).
- **Navmeshes** (2026-11-07): the triangles of a glTF or GLB file's default scene (static meshes; skinned meshes and
  other primitives are left out with a note) or of a `.qmodel`, with their node transforms, built into tiles on the
  job system and written as `.qnav`. The settings (agent, cell and tile sizes, area types and costs, which node,
  mesh or material names are which area, `"none"` or `"ignore"`, area boxes, off-mesh links) come from
  `<input stem>.nav.json` or `--settings` ([Navigation](navigation.md#cooking)); unknown keys and undefined area
  names are errors, surface names that match nothing are notes.
- **Sounds**: WAV, FLAC, MP3 or Ogg Vorbis (miniaudio's decoders, Vorbis through stb_vorbis). The file is decoded
  completely to check it (a file that does not decode, or decodes to no frames, fails the cook). An output with the
  input's extension gets the input's bytes; a `.wav` output from another format, or with `--channels 1|2` (mix down
  to mono or up to stereo), is a 16-bit PCM WAV at the source's sample rate (`cook_cli`: the test loop's `.ogg` is
  copied byte for byte, and cooked to a mono WAV of its 48000 frames). No resampling, normalization or encoding to
  Vorbis: the mixer resamples per voice, and files of other formats are refused ("a sound is cooked to its own
  format or to .wav").
- **Deterministic**: the output depends only on the inputs and the options (no time stamps, no thread order; a test
  cooks with 0 and 3 worker threads and compares). Files are written to a temporary file and renamed, so a running
  game with hot reload never reads half a file; a model's textures are rewritten only when they change.
- **Depfile**: `--depfile` lists every file read (the glTF, its buffers and images, the clip settings, the bone
  map, the navmesh settings), so Ninja cooks again when any of them changes.

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
    [RETARGET characters/hero.map.json]    # retargeted onto SKELETON: <path>.<SKELETON's name>.qanim
    [NAVMESHES levels/yard.gltf]           # .qnav, settings from <stem>.nav.json
    [FILES credits.txt levels/one.json]    # copied as they are
    [OUTPUT_DIR dir] [TEXTURE_FORMAT auto|bc7|astc|rgba8|basis] [BASIS_CODEC auto|etc1s|uastc]
    [TEXTURE_MAX_SIZE n] [MODEL_OPTIONS --lods 2 ...])
quench_add_game(my_game ... ASSETS assets COOKED_ASSETS my_game_cooked)
```

`quench_cook_assets` (`cmake/QuenchCook.cmake`) adds a target that cooks each file into the same relative path (with
`.ktx2`, `.qmodel`, `.qskel`, `.qanim` or `.qnav`; sounds keep their name and format). `TEXTURE_FORMAT auto` (the
default) cooks ASTC 4x4 in iOS builds and BC7 elsewhere, also the textures of models. `TEXTURE_FORMAT basis` cooks Basis
Universal files for every device (also the textures of models), with `BASIS_CODEC` (only with `basis`): `auto` (the
default; UASTC for `DATA_TEXTURES` and a model's linear textures, ETC1S for color), `etc1s` or `uastc` for the whole
call; a game that wants both codecs for color makes two calls. `FILES` are copied unchanged
(texts, settings, licences; paths relative to `SOURCE_DIR`, no `..`), so a game can ship files from outside its
`ASSETS` directory: the demo reads the engine's `LICENSE` and `THIRD_PARTY_LICENSES` that way. A call with `FILES`
only needs no `quench_cook` (it works in cross builds without `host-tools`). `quench_add_game(... COOKED_ASSETS ...)`
merges the cooked directories with `ASSETS` into the game's `assets/`: next to the executable on macOS, into the
bundle on iOS (Ninja), and in Xcode projects through a staging directory that a script phase copies into the bundle
(a folder reference reached the bundle one build late, 2026-11-07; [App](app.md#ios-runs));
`tools/package_demo.sh` compares the two. On macOS the copy is `copy_directory_if_different`:
rebuilding the game's `<game>_assets` target while the game runs with `--hot-reload` cooks what changed and the game
reloads exactly those files.

The cook is built from source in host builds. Cross builds (iOS) take it from the `host-tools` preset through
`QUENCH_HOST_COOK` (the iOS presets set it; build `host-tools` first). The web build cooks nothing. The cook's own
dependencies (fastgltf with its bundled simdjson and simdutf, stb) are fetched at configure time in host builds and
built only when the cook is (`EXCLUDE_FROM_ALL`).

## Basis Universal

One cooked set for every device: a Basis Universal file (KTX 2.0, ETC1S or UASTC LDR 4x4) is transcoded at load
time, on the loading job, to a format the device samples. Basis Universal v2.50 (Binomial, Apache-2.0;
[Dependencies](dependencies.md)): its transcoder is in `quench_assets` (0.96 MB in a stripped arm64 program, the
targets the engine never asks for left out), its encoder only in `quench_cook`.

What a file becomes (`assets::basisTarget`, from `gpu::Caps` through `SceneAssets::textureSupport()`), the first the
device samples of:

| Codec | Targets in order | Why |
|---|---|---|
| UASTC | ASTC 4x4, BC7, ETC2, RGBA8 | UASTC is a subset of ASTC (exact); BC7 is close; ETC2 only about ETC1S's quality |
| ETC1S | ETC2, ASTC 4x4, BC7, RGBA8 | ETC1S is a subset of ETC1 (ETC2 RGB8 is exact, 4 bits per texel); ASTC and BC7 give the same quality, ASTC transcodes faster |

ETC2 is RGB8 for an opaque file, RGBA8 (EAC alpha) for one with alpha; the sRGB variant when the file is sRGB. RGBA8
also where level 0 is not a multiple of 4 on both sides (Metal's rule for block formats), with one log line per
`SceneAssets` the first time a texture falls back to RGBA8 for a missing format. On the Mac: SDL GPU's Metal driver
and Vulkan through MoltenVK sample BC7 and ASTC, not ETC2 (SDL GPU 3.4.18 has no ETC2 formats), so both codecs
become ASTC 4x4; the native Metal backend also samples ETC2, so ETC1S becomes ETC2 RGB8 there (half the memory of
ASTC). iOS devices sample ASTC, and ETC2 on the native backend (the backend reports ETC2 on every iOS GPU), so the
same holds there (not run on an iPhone yet). Hot reload and the asset overlay read Basis Universal files like any
other (`render.scene_assets_basis_textures` reloads one).

**Measured** (2026-11-07, M4 Pro, Release; `cook.cook_basis_compared_on_demo_textures`): the demo's 19 textures
(the 8 tiles, the crate's planks and the tunnel's 10, procedural; 1.88 Mtexels in level 0, all with mips) cooked
directly and through Basis Universal. Quality against the source over every level: PSNR in dB from the squared
error of all texels of a group (color and data), the mean angle of the normals in degrees (normal maps).

| | Bytes (bits per level-0 texel) | Color, 12 textures | Data, 3 | Normal maps, 4 |
|---|---|---|---|---|
| BC7 | 2,519,504 (10.7) | 56.7 dB | 56.1 dB | 0.77° |
| ASTC 4x4 | 2,519,504 (10.7) | 60.0 dB | 66.6 dB | 0.51° |
| ETC1S, to any target | 188,980 (0.80) | 37.9 dB | 29.3 to 29.9 dB | 3.3 to 3.4° |
| UASTC to ASTC 4x4 | 1,255,728 (5.33) | 58.2 dB | 64.3 dB | 0.68° |
| UASTC to BC7 | | 55.2 dB | 57.4 dB | 0.70° |
| UASTC to ETC2 | | 40.8 dB | 27.8 dB | 2.68° |

With the default codecs (ETC1S for color, UASTC for data and normal maps) the set is 911,065 bytes. Packages are
compressed: with `gzip -9` (zip's method) BC7 shrinks to 1,142,874 bytes and ASTC to 1,274,141 (`zstd -19`:
1,040,458 and 1,158,761), Basis Universal files hardly (ETC1S 183,250, UASTC 1,251,611): UASTC saves no download
over a compressed BC7 set, ETC1S saves about 6 times.

Cooking the 19 files, one `quench_cook` process each, CPU time: BC7 0.70 s, ASTC 1.26 s, ETC1S 2.17 s, UASTC
5.14 s. Transcoding the set (every level, one thread): ETC1S to ETC2 3.3 ms, to ASTC 5.1 ms, to RGBA8 5.2 ms, to
BC7 12.4 ms (1.8, 2.7, 2.8 and 6.6 ms per Mtexel of level 0); UASTC, Zstandard included, to RGBA8 10.4 ms, ASTC
12.0 ms, BC7 15.9 ms, ETC2 16.5 ms (5.5, 6.4, 8.5 and 8.8 ms per Mtexel). A 64 x 64 file with mips takes 0.03 to
0.10 ms (`assets.basis_transcode_speed`, the median of 101; as WebAssembly under Node.js 0.02 to 0.18 ms). The first
transcode of a process also builds the transcoder's tables: 17 to 34 ms more, once, on the loading job.

**The default stays `auto`** (BC7 on the Mac, ASTC 4x4 on iOS): on the platforms the engine runs on, both are
sampled directly, at a quality Basis Universal does not reach (ETC1S 19 dB below BC7 on color; UASTC 1.5 dB below
BC7 and 1.8 dB below ASTC on color), with no transcoding at load time and a cook 3 to 7 times faster, and a
compressed BC7 package is not larger than a UASTC one. Basis Universal is the option for a set that must serve
devices with other formats (Android's ETC2-only GPUs, the web once it renders), and ETC1S where the download size
matters more than the quality. The default codecs follow the measurements: ETC1S on data and normal maps is far
worse (29 dB, 3.3°) than on color.

## Demo content and tests

The demo's sources (`examples/demo/assets_src/`) are procedural, written by `tools/assets/make_demo_assets.py`
(CC0, `CREDITS.md` there): eight 64 x 64 tile patterns, a crate (glTF, external buffer and PNG), a pillar (GLB with
an embedded JPEG and PNG) and a ball (glTF, data URI, vertex colors, no normals). `examples/demo/assets/` adds a
deliberately broken `.qmodel`. The demo's sounds are not in git: `tools/assets/make_demo_sounds.py` (Python standard
library, procedural, CC0) writes five WAV files into the build tree at build time and `quench_cook_assets(...
SOUNDS ...)` checks and copies them ([Audio](audio.md#demo-and-tests)). Its animated character is not in git either:
`tools/assets/make_demo_character.py` (procedural, CC0) writes a mannequin with its clips as glTF and its clip
settings at build time, and `quench_cook_assets(... MODELS ... SKELETONS ... ANIMATIONS ...)` cooks it
([Animation](animation.md#demo-and-tests)); `tools/assets/make_demo_blob.py` writes the blend shape creature the same
way (four targets, no normals in the file). The Assets section's tiles also come as two Basis Universal sets,
cooked from the same PNG files with `TEXTURE_FORMAT basis` (`tiles_etc1s/`, `tiles_uastc/`), and its "Tile
textures" choice loads them ([Engine demo](demo.md#sections)).

Tests: `core.jobs_*` (also without workers and under stress), `physics.*` (Jolt on the job system, single-threaded,
two worlds on one pool, worker priority), `assets.*` (the manager with a test asset type, KTX2,
BC7 decoding, `.qmodel` with blend shapes and the version 3 reader, the streaming grid; `assets.basis_*`: the
header, the target by device support, every test file to RGBA8, BC7, ASTC and ETC2 with its levels and sizes and
its quality where it can be decoded, the alpha channel, a 6 x 5 file to RGBA8, corrupt and truncated files, the
transcode time; the files are cooked by the cook's `cook.cook_basis_test_data` into `tests/assets/data/` and
embedded, so the web build tests them too), `audio.sound_assets` (the
sound type: loading, plays of a loading
sound, missing and broken files, a late one-shot dropped), `animation.missing_and_corrupt_assets_hold_the_rest_pose`
and `animation.reload_rebinds_and_keeps_parameters`, `cook.*` and `cook_cli` (sounds, skeletons, clips and skinned
models included; `cook.cook_model_morph_targets`: names, default weights, offsets under a non-uniform node scale,
welding, computed normals),
`cook.cook_basis_round_trips` (each codec from color, alpha and normal maps to every target, with floors),
`cook.cook_basis_compared_on_demo_textures` (above; Release only), `render.scene_assets_*` and
`render_vulkan.scene_assets_*` (pixels of placeholders, loaded textures and models, failures;
`scene_assets_blend_shapes`: weights by name across parts, the file's defaults, kept across a reload;
`scene_assets_basis_textures`: an ETC1S and a UASTC file in the scene, transcoded to the device's target, and a
reload of one file with the other; Vulkan through MoltenVK), and the demo's `demo_assets_<backend>` and
`demo_assets_basis_<backend>`.

## Limitations

- **Hot reload** of assets works where the
  asset directory is a directory of the file system (the Mac) and, in development builds, through the asset overlay
  on a device: files are pushed from the Mac by `devicectl` (iOS only; not over the network, no asset server).
  A file removed from the overlay goes back to the bundled one, but the script does not remove files.
- **Texture formats**: the cook writes BC7, ASTC 4x4 (iOS builds), RGBA8 and Basis Universal (opt-in). A device
  without BC sampling gets BC7 decoded to RGBA8 on the CPU (four times the memory), and a device without ASTC
  sampling gets placeholders for ASTC files (no device the engine has run on lacks it, so that path is checked only
  where the test sees no ASTC support, which has not happened); a set for such devices is cooked as Basis
  Universal. ASTC is cooked at 4x4 only (8 bits per texel, as BC7), LDR only; HDR images and larger blocks are not
  offered.
- **Basis Universal**: LDR only (no UASTC HDR or ASTC HDR 6x6), one 2D image per file (no arrays, cube maps or
  video); the codec is chosen per call of `quench_cook_assets` or per file by `quench_cook`, with the encoder's
  default quality (no rate-distortion optimization for smaller UASTC files, no ETC1S quality option). ETC2 is
  sampled only on the native Metal backend; SDL GPU has no ETC2 formats, so on SDL GPU a device without BC7 and ASTC
  (none so far) gets RGBA8. The measurements are of the demo's procedural textures, on the M4 Pro only; the
  transcode time on a phone and the look of ETC1S and UASTC on a phone's screen are not measured yet.
- **Materials**: normal and emissive textures are cooked, named in the model and read by the standard surface
  (`SceneAssets` puts them into a material's textures 2 and 3; [Render module](render.md)); ray hits do not read
  normal maps. Occlusion textures, `KHR_texture_transform` and unlit materials are dropped with a note; cameras and
  lights of glTF files are not cooked.
- **Blend shapes**: the offsets are dense (one per vertex and target, also where a target moves nothing: 36 bytes
  per vertex and target in the file, 48 on the GPU). Sparse target accessors go through fastgltf's accessor reading
  like every other, but no test file has one. The levels of detail are simplified from the base shape only.
- **One scene per manager**: `SceneAssets` registers its asset types on its manager, so a second scene needs a
  second `AssetManager` (with its own cache: a file used by both scenes loads twice).
- **Streaming** loads and unloads by distance; there is no memory budget across cells beyond the manager's per-type
  budgets of unreferenced assets, and the demo's streaming is of textures and models only (no physics bodies).
- **Timing**: loads finish in real time, so a scripted run whose capture depends on when a load finished is not
  byte-identical with TAA (the demo's assets check uses FXAA).
