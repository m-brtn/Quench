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

## What was verified

- `weather.weather_cell_in_clear_sky`: `coverAbove` 1.000 under the base (equal to `cover`), 0.500 halfway up the
  cell, 0 over its top. `tools/check.sh quick weather render demo` 260 of 260 (the demo's captures, Metal and the
  native backend, unchanged), `step wasm` and `step new-game` passed.
- A camera over the clouds was looked at in captures of the game, volumetric and 2D layer (Mac, native Metal).

## What was not verified

- Nothing of it on a phone; the clouds' shadow map still lies on the plane y = 0 (a game above the clouds turns
  `cloudShadows` off).

## Changes for games

None: `coverAbove` is new; `cover` is unchanged.
