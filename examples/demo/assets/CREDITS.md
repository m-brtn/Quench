# Credits: demo assets

The demo's run-time asset directory. At build time quench_cook adds the cooked files of `../assets_src` (see the
`CREDITS.md` there), the sounds and the character below; the files here are copied as they are.

| File | Content | Licence |
| --- | --- | --- |
| `models/broken.qmodel` | a line of text, deliberately not a cooked model (the Assets section's failed load) | CC0 1.0 |
| `sounds/motor.wav`, `siren.wav`, `ping.wav`, `music_a.wav`, `music_b.wav` (not in git) | sine harmonics, drones and note patterns computed by `tools/assets/make_demo_sounds.py` at build time (the Audio section); no recorded or third-party audio | CC0 1.0 |
| `sounds/step.wav` (not in git) | a 90 Hz thump under a low-passed noise scuff computed by `tools/assets/make_demo_sounds.py` at build time (the Animation section's footsteps); no recorded audio | CC0 1.0 |
| `characters/mannequin.qmodel`, `.qskel`, `.qanim` (not in git) | a jointed mannequin of limb tubes, spheres and boxes, its skeleton and its clips (idle, walk, run, jump, wave, lean), computed by `tools/assets/make_demo_character.py` at build time as glTF and cooked (the Animation section); no scanned, downloaded or third-party content | CC0 1.0 |
| `characters/mannequin.graph.json` | the mannequin's animation graph (state machine), written for the demo | CC0 1.0 |
