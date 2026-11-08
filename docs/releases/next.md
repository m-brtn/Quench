# Quench (next)

Changes since [0.11.0](v0.11.0.md) (tag `v0.11.0`), not released yet: what the first game on the engine (Cloudsong,
grassy isles above a sea of clouds) needed from it.

## What changed

### Weather and clouds

- **A camera above the clouds** ([Weather](../weather.md)). `Weather::coverAbove(point, radius)`: the cover of the
  clouds above a point (each sample's coverage times the share of its clouds' height above it), the same as `cover`
  at and under the cloud base, 0 over the tops. The renderer dims the sun and greys the sky's light by it, so a
  camera flying over a sea of clouds sees the sun and the clear sky (before, the clouds below dimmed them too). The
  sky pass draws the volumetric clouds below the horizon when the camera is above the cloud base, and the 2D layer,
  seen from above its height, is drawn below the horizon and lit on its tops. Under the cloud base nothing changes.
- **The clouds' shadow on high ground** ([Weather](../weather.md), "Light under the clouds").
  `RenderSettings::cloudShadowHeight` (0 m by default): the cloud shadow map lies on that plane and holds only the
  clouds above it, so ground above a sea of clouds is no longer shaded by the clouds below it. With 0 nothing
  changes.

## What was verified

- `weather.weather_cell_in_clear_sky`: `coverAbove` 1.000 under the base (equal to `cover`), 0.500 halfway up the
  cell, 0 over its top. `tools/check.sh quick weather render demo` 260 of 260 (the demo's captures, Metal and the
  native backend, unchanged), `step wasm` and `step new-game` passed.
- A camera over the clouds was looked at in captures of the game, volumetric and 2D layer (Mac, native Metal).
- `render.renderer_cloud_shadow_height` (and on Vulkan through MoltenVK and the native backend): a raining cell's
  shadow on a ground plane at 9500 m, over the clouds' top, is 0.000 of the sunlit 0.775 with the plane at 0 and
  0.775 with the plane at the ground's height; at 300 m, under the cell, 0.073 with the plane at 300 m.
  `tools/check.sh quick render demo` 241 of 241 (the demo's captures unchanged).

## What was not verified

- Nothing of it on a phone. The shadow map has one plane: ground far above or below it is still shaded by the
  column its sun ray crosses at that height.

## Changes for games

None: `coverAbove` and `cloudShadowHeight` are new; `cover` is unchanged, and the shadow with the default height is
the same as before.
