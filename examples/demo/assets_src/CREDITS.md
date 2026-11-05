# Credits: demo source assets

Every file in this directory is procedural: written by `tools/assets/make_demo_assets.py` from code in this
repository (the one JPEG inside `models/pillar.glb` is converted from the script's PNG by macOS `sips`). No
third-party content. Licence: CC0 1.0 (public domain dedication).

| File | Content |
| --- | --- |
| `tiles/tile_0.png` ... `tiles/tile_7.png` | 64 x 64 patterns, one color each, a light border |
| `models/crate.gltf`, `models/crate.bin`, `models/crate_planks.png` | a 1 m box with a planks texture (external buffer and image) |
| `models/pillar.glb` | a cylinder with an embedded JPEG base color and an embedded PNG metallic-roughness texture |
| `models/ball.gltf` | an icosphere (1280 triangles) with vertex colors, no normals, its buffer in a data URI |

quench_cook turns them into KTX2 textures and .qmodel models at build time (`examples/demo/CMakeLists.txt`).
