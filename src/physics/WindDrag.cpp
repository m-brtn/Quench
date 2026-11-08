#include <quench/physics/WindDrag.h>

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>

#include <glm/geometric.hpp>

#include <algorithm>

namespace quench {

glm::vec3 WindDrag::force(glm::vec3 wind, glm::vec3 velocity, float area, float dragCoefficient, float airDensity) {
    const glm::vec3 relative = wind - velocity;
    return (0.5f * airDensity * dragCoefficient * area * glm::length(relative)) * relative;
}

void WindDrag::add(JPH::BodyID body, float area, float dragCoefficient) {
    area = std::max(area, 0.0f);
    dragCoefficient = std::max(dragCoefficient, 0.0f);
    for (Entry& e : m_bodies)
        if (e.body == body) {
            e.area = area;
            e.drag = dragCoefficient;
            return;
        }
    m_bodies.push_back({body, area, dragCoefficient});
}

bool WindDrag::remove(JPH::BodyID body) {
    return std::erase_if(m_bodies, [body](const Entry& e) { return e.body == body; }) > 0;
}

void WindDrag::apply(PhysicsWorld& world, const std::function<glm::vec3(glm::vec3)>& windAt, float airDensity) const {
    const JPH::BodyLockInterfaceNoLock& locks = world.system().GetBodyLockInterfaceNoLock();
    const float gravity = world.system().GetGravity().Length();
    for (const Entry& e : m_bodies) {
        if (e.area <= 0.0f || e.drag <= 0.0f) continue;
        JPH::BodyLockWrite lock(locks, e.body);
        if (!lock.Succeeded()) continue;
        JPH::Body& body = lock.GetBody();
        if (!body.IsInBroadPhase() || !body.IsDynamic()) continue;
        const JPH::RVec3 c = body.GetCenterOfMassPosition();
        const JPH::Vec3 v = body.GetLinearVelocity();
        const glm::vec3 f = force(windAt(glm::vec3(float(c.GetX()), float(c.GetY()), float(c.GetZ()))),
                                  glm::vec3(v.GetX(), v.GetY(), v.GetZ()), e.area, e.drag, airDensity);
        const JPH::Vec3 jf(f.x, f.y, f.z);
        if (body.IsActive()) {
            body.AddForce(jf);
            continue;
        }
        const float inverseMass = body.GetMotionProperties()->GetInverseMass();
        if (inverseMass > 0.0f && glm::length(f) * inverseMass <= kWakeShare * gravity) continue;
        world.system().GetBodyInterfaceNoLock().ActivateBody(e.body);
        body.AddForce(jf);
    }
}

} // namespace quench
