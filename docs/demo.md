# Engine demo

`examples/demo` (target `quench_demo`, executable `quench-demo`, app name "Quench Demo") is one app on `quench_main`
and `quench_app` for the Mac and iOS: a menu of sections, each a small interactive scene of primitives and
procedural content that shows one area of the engine, with ImGui controls and the frame, CPU and GPU times. It was
started in stage 5 (user decision, 2026-11-05); every later stage adds its section, and stage 10 of the
[Roadmap](roadmap.md) finishes and polishes it. `examples/hello` stays the minimal sample.

Every section has a scripted check and golden captures, so the demo is also a test of the engine, and every stage
ends with a packaged build of it in `dist/` (`tools/package_demo.sh`, below).

## Sections

| # | Id | Shows | Controls besides the camera |
|---|---|---|---|
| 1 | `materials` | Dielectric and metal spheres from smooth to rough, a textured box, glass (alpha blend), an alpha-tested fence, an emissive lamp; the sun, three point lights and a spot; a game surface shader (the checker ground) and a game render pass (the light discs) | lights on and off, light intensity, animation, discs, checker |
| 2 | `shadows` | Sun shadow cascades over 90 m of columns, thin bars, a spinning torus, a bouncing ball | cascades (0 to 4), map size, distance, filter (1, 4, 12 taps), sun elevation and azimuth, turning sun |
| 3 | `sky` | The daylight palette for any sun elevation, the atmosphere model and its gradient fallback, the clear color without a sky, height fog; the sun disc above its glow (in SDR at a low sun both are white) | sun, run the day, sky, atmosphere, sun disc, fog density, falloff, base, maximum, sun scatter |
| 4 | `post` | Bloom, tone mapping and anti-aliasing at dusk: glowing rings and lamps, thin poles, spinning blades | none, FXAA, TAA (with the fallback shown), render scale, sharpen, bloom strength and threshold, ACES or AgX, exposure, vignette, dither |
| 5 | `hdr` | HDR display output: a sun at 14 degrees, seven emissive panels from 0.5 to 32 times the radiance at the HDR knee, a white card in the shade; with HDR the sun and the brighter panels go above paper white, the card and the UI do not | output mode (off, auto, on), the display's headroom and support and the output in use, the SDR curve for comparison, a custom peak instead of the display's, panel brightness, sun elevation |
| 6 | `physics` | Jolt through `quench::PhysicsWorld` at 120 Hz, drawn interpolated: a stack of boxes and balls | drop a box (B, gamepad X), drop a ball (N, gamepad Y), blast (P, gamepad B, or a click or tap on the floor), reset (R, gamepad Back) |
| 7 | `input` | Named actions over every device, shown live: a character that walks, runs, jumps and fires | WASD, left stick or a finger on the left half: walk; Left Shift or the left stick button: run; J, gamepad A or a tap on the right half: jump; F, gamepad X, a click or a tap: fire |

Every section has a "Frame and fallbacks" panel: the 3D resolution, draws, culled instances, lights, shadow draws,
what runs after the device fallbacks (cascades, depth clamp, anti-aliasing), the HDR and depth formats and the device
capabilities.

Common controls:

- Sections: `]` / `[`, Page Down / Page Up, the digits 1 to 7, the gamepad shoulder buttons, a click or tap on the
  list. ImGui keyboard and gamepad navigation is on (the demo's keys keep working while a window has focus).
- Camera (all sections but input): drag with the left or right mouse button or one finger to orbit, the wheel or a
  pinch to zoom, the right stick and the triggers on a gamepad.
- H or the gamepad Start button hides the windows.

Flags: `--section ID` starts in a section (an unknown id fails with a message); the engine's flags (`--help`, App
shell) work as everywhere.

## Checks

| CTest test | What it runs |
|---|---|
| `demo_<section>_metal`, `demo_<section>_vulkan` | `--section <id> --script examples/demo/scenarios/<id>.txt` at 960 x 540 (`--window` and `--frame-size`), with GPU validation; the scenario switches the section's options and checks the renderer's and the section's values, puts the start state back, and the frame at 2 s (input: 3 s) is compared with `tests/golden/<backend>/<device>/demo_<id>.png`. The scenarios point at the window in percent or at named points (`@menu:<section>`, `@floor`), so their checks also pass in a portrait phone window (the captures are compared on the Mac only) |
| `demo_hdr_on_metal`, `demo_hdr_on_vulkan` | the HDR section with `--hdr on --hdr-headroom 4` and `scenarios/hdr_on.txt`: the output is HDR extended linear with headroom 4 and the capture's peak is above 2 and at most 4 times paper white; the capture (clipped at paper white) is compared with `demo_hdr_on.png` |
| `demo_menu` | `scenarios/menu.txt`: switching sections by keys, gamepad, a click and a tap on the list; hiding the windows |
| `demo_version`, `demo_bad_section` | `--version` prints "Quench Demo <version>"; an unknown `--section` fails |

The scenarios turn dithering off (`set dither 0`), which keeps the golden PNGs at 120 to 380 KB. Game values a
scenario can read: `section`, `sections`, `ui`, `aa` (0 none, 1 FXAA, 2 TAA, after fallbacks), `cascades`, `bloom`,
`draws`, `instances`, `culled`, `shadow_draws`, `shadow_instances`, `moved`, `lights`, `render_w`, `render_h`,
`camera_yaw`, `camera_pitch`, `camera_distance`, `clicks`, then the section's own (for example `bodies`, `blasts`,
`player_x`, `jumps`, `marker_draws`, `exposure`). Game commands: `section ID`, `camera YAW PITCH DISTANCE` (degrees,
meters), `set NAME VALUE` (render settings: `aa none|fxaa|taa`, `tone aces|agx`, `cascades`, `shadow_size`,
`shadow_filter`, `shadow_distance`, `scale`, `bloom`, `bloom_strength`, `sky`, `dither`, `sharpen`, `vignette`;
otherwise the section's options).

## Benchmark

`scenarios/benchmark.txt` measures the materials, shadows, sky, post-processing, HDR (in SDR, then with HDR output on)
and physics sections for 5 s each in real time (budgets and numbers: [Performance](performance.md)):

```sh
build/release/examples/demo/quench-demo --frame-size 1920x1080 --script examples/demo/scenarios/benchmark.txt
```

## Packaged builds: `tools/package_demo.sh`

```sh
tools/package_demo.sh          # macOS: dist/QuenchDemo-<version>-<hash>-macos.zip
tools/package_demo.sh --ios    # also iOS: dist/QuenchDemo-<version>-<hash>-ios.zip
```

`<hash>` is the short commit hash, with `-dirty` when the working tree has changes (the build is then not exactly
that commit). The script builds `quench_demo` with the `release` preset (`--preset NAME` for another), runs the
`demo_*` tests, and then:

- **macOS**: assembles `QuenchDemo.app` (the executable, an Info.plist with the version and the binary's minimum
  macOS as `LSMinimumSystemVersion`, an icns made from `examples/demo/icon/icon.png`, `LICENSE` and
  `THIRD_PARTY_LICENSES` in `Contents/Resources`), checks that it links only system libraries, signs it ad hoc and
  zips it with `LICENSE` and `THIRD_PARTY_LICENSES` next to it. The archives carry no extended attributes: `ditto`
  would store them as AppleDouble `._` files, which a plain `unzip` turns into files inside the bundle that break its
  signature; the script fails on any `._` entry. It then unzips the archive with a plain `unzip` into a temporary
  directory, verifies the signature and starts the app from `/` with `scenarios/smoke.txt` (every section once):
  exit code 0, the scenario
  passed, and a capture (`dist/<name>-macos-smoke.png`) to look at. The app is signed ad hoc and not notarized: a
  copy downloaded on another Mac is quarantined, and Gatekeeper opens it only after it is allowed in System Settings,
  Privacy & Security (not tried on another Mac yet).
- **iOS** (`--ios [--team ID]`): builds `quench_demo` with the `ios-xcode` preset (Automatic signing with the team:
  `--team`, else `QUENCH_IOS_TEAM` from the environment, else the one `build/ios-xcode` is configured with; an Apple
  ID signed in to Xcode, which creates the development profile on its own) and packs the signed app as
  `QuenchDemo.ipa`, with the licences, into the zip. The team must be a paid one: the script refuses a free Personal
  Team (Xcode's account data marks it), whose profiles expire after 7 days, and checks that the app is signed by the
  team (`codesign`, `TeamIdentifier`), also after a plain `unzip` of the `.ipa`. It runs only on the devices in the
  profile, and the profile expires (the paid
  team's wildcard profile made on 2026-11-05 lasts until 2027-11-05, with 5 devices). The script prints the team, the
  signature, the profile's name, expiry and device count. Nothing is installed on a device by the script.

`dist/` is git-ignored.
