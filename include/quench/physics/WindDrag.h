#pragma once

#include <quench/physics/PhysicsWorld.h>

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Body/BodyID.h>

#include <glm/vec3.hpp>

#include <cstddef>
#include <functional>
#include <vector>

namespace quench {

// The wind's drag on chosen bodies (docs/weather.md, "Wind"): F = 1/2 rho Cd A |w - v| (w - v), with w the air's
// velocity at a body's centre of mass and v the body's, added as a force at the centre of mass before a step. Only
// the bodies added here feel it: a game picks what the wind moves (leaves, crates, a sail, a balloon) and gives each
// its area facing the wind and its drag coefficient. The wind comes from the game (weather::Weather::windAt for the
// weather's, with its gusts): quench_physics does not know the weather. No torque, no lift, no shelter behind other
// bodies; the area does not turn with the body.
class WindDrag {
public:
    static constexpr float kAirDensity = 1.225f; // kg/m^3: dry air at sea level and 15 degrees C

    // The drag of air moving at `wind` on a body moving at `velocity` (m/s): N.
    static glm::vec3 force(glm::vec3 wind, glm::vec3 velocity, float area, float dragCoefficient,
                           float airDensity = kAirDensity);

    // A body the wind pushes: its area facing the wind (m^2) and its drag coefficient (about 0.47 for a sphere, 1.05
    // for a cube face on, 1.2 for a flat plate across the flow). Adding it again changes its values.
    void add(JPH::BodyID body, float area, float dragCoefficient);
    bool remove(JPH::BodyID body);
    void clear() { m_bodies.clear(); }
    std::size_t size() const { return m_bodies.size(); }

    // Before each PhysicsWorld::step, on the thread that steps: adds every dynamic body's drag for that step (Jolt
    // clears forces after a step), with `windAt` the air's velocity (m/s) at a world position. A body that is not in
    // the world is skipped; a sleeping body wakes when its drag is above kWakeShare of its weight.
    static constexpr float kWakeShare = 0.01f;
    void apply(PhysicsWorld& world, const std::function<glm::vec3(glm::vec3)>& windAt,
               float airDensity = kAirDensity) const;

private:
    struct Entry {
        JPH::BodyID body;
        float area = 0.0f;
        float drag = 0.0f;
    };
    std::vector<Entry> m_bodies;
};

} // namespace quench
