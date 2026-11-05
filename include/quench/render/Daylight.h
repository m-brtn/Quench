#pragma once

// A daylight palette keyed by the sun's elevation: night, twilight, sunrise colors and day (TrailForge's time of day,
// without its calendar, moon and game tuning). A game drives the sun; this gives the matching sky, fog, ambient light
// and sun color.

#include <quench/render/Scene.h>

#include <glm/vec3.hpp>

namespace quench::render {

struct Daylight {
    Environment environment;
    glm::vec3 sunColor{0.0f}; // irradiance of the sun (LightDesc::color with intensity 1); 0 below the horizon
};

// towardSun: unit direction toward the sun (its elevation picks the palette).
Daylight daylightFor(const glm::vec3& towardSun);

// Unit direction toward a body at the given elevation and azimuth (degrees; azimuth 0 = -z, 90 = +x).
glm::vec3 skyDirection(float elevationDegrees, float azimuthDegrees);

} // namespace quench::render
