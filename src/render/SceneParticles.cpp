// The particle systems and trails of render::Scene (docs/particles.md); stepped and drawn by ParticleRenderer.

#include <quench/render/Scene.h>

#include "SceneImpl.h"

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <vector>

namespace quench::render {

using detail::ParticleEntry;
using detail::ParticlePool;
using detail::TrailEntry;

namespace {

// Stale or null handles are programming errors: asserted, then ignored in release builds.
template <class Map, class Id>
auto* lookup(Map& map, Id id, [[maybe_unused]] const char* what) {
    auto* entry = map.get(id);
    QUENCH_ASSERT(entry, what);
    return entry;
}

constexpr std::uint32_t kMaxSparklesPerMove = 256;

std::uint32_t slotsOf(const particles::ParticleDesc& desc) {
    return std::clamp(desc.maxParticles, 1u, particles::kMaxSystemParticles);
}

} // namespace

namespace detail {

std::optional<std::uint32_t> ParticlePool::allocate(std::uint32_t size) {
    for (std::size_t i = 0; i < free.size(); ++i)
        if (free[i].size >= size) {
            const std::uint32_t offset = free[i].offset;
            free[i].offset += size;
            free[i].size -= size;
            if (free[i].size == 0) free.erase(free.begin() + std::ptrdiff_t(i));
            used += size;
            return offset;
        }
    if (std::uint64_t(end) + size > particles::kMaxSceneParticles) return std::nullopt;
    const std::uint32_t offset = end;
    end += size;
    used += size;
    return offset;
}

void ParticlePool::release(std::uint32_t offset, std::uint32_t size) {
    used -= size;
    const auto at = std::lower_bound(free.begin(), free.end(), offset,
                                     [](const Range& r, std::uint32_t o) { return r.offset < o; });
    auto it = free.insert(at, Range{offset, size});
    // Merged with the free ranges next to it, and given back to `end` when it reaches it.
    if (it + 1 != free.end() && it->offset + it->size == (it + 1)->offset) {
        it->size += (it + 1)->size;
        free.erase(it + 1);
    }
    if (it != free.begin() && (it - 1)->offset + (it - 1)->size == it->offset) {
        (it - 1)->size += it->size;
        it = free.erase(it) - 1;
    }
    if (it->offset + it->size == end) {
        end = it->offset;
        free.erase(it);
    }
}

} // namespace detail

Result<ParticleSystemId> Scene::addParticles(const particles::ParticleDesc& desc, const glm::mat4& transform) {
    const std::uint32_t slots = slotsOf(desc);
    const std::optional<std::uint32_t> offset = m_impl->particlePool.allocate(slots);
    if (!offset)
        return Error{std::format("render: particle system of {} slots: the scene has {} of {} in use", slots,
                                 m_impl->particlePool.used, particles::kMaxSceneParticles)};
    ParticleEntry e{.emitter = particles::Emitter(desc), .uniforms = particles::uniformsOf(desc), .offset = *offset};
    e.emitter.setTransform(transform, true);
    return m_impl->particleSystems.add(std::move(e));
}

Result<void> Scene::setParticles(ParticleSystemId system, const particles::ParticleDesc& desc) {
    ParticleEntry* e = lookup(m_impl->particleSystems, system, "setParticles: unknown particle system");
    if (!e) return Error{"setParticles: stale or null particle system"};
    const std::uint32_t slots = slotsOf(desc);
    if (slots != e->emitter.capacity()) {
        ParticlePool& pool = m_impl->particlePool;
        pool.release(e->offset, e->emitter.capacity());
        const std::optional<std::uint32_t> offset = pool.allocate(slots);
        if (!offset) {
            const std::uint32_t old = e->emitter.capacity();
            e->offset = *pool.allocate(old); // the range it just gave back
            return Error{std::format("render: particle system of {} slots: the scene has {} of {} in use", slots,
                                     pool.used, particles::kMaxSceneParticles)};
        }
        e->offset = *offset;
        e->home = detail::ParticleHome::None;
        e->cpu.reset();
    }
    e->emitter.setDesc(desc);
    e->uniforms = particles::uniformsOf(desc);
    return {};
}

void Scene::setParticleTransform(ParticleSystemId system, const glm::mat4& transform, bool teleport) {
    if (ParticleEntry* e = lookup(m_impl->particleSystems, system, "setParticleTransform: unknown particle system"))
        e->emitter.setTransform(transform, teleport);
}

void Scene::setParticlesEmitting(ParticleSystemId system, bool emitting) {
    if (ParticleEntry* e = lookup(m_impl->particleSystems, system, "setParticlesEmitting: unknown particle system"))
        e->emitter.setEmitting(emitting);
}

void Scene::setParticlesVisible(ParticleSystemId system, bool visible) {
    if (ParticleEntry* e = lookup(m_impl->particleSystems, system, "setParticlesVisible: unknown particle system"))
        e->visible = visible;
}

void Scene::burstParticles(ParticleSystemId system, std::uint32_t count) {
    if (ParticleEntry* e = lookup(m_impl->particleSystems, system, "burstParticles: unknown particle system"))
        e->emitter.burst(count);
}

void Scene::emitParticles(ParticleSystemId system, std::span<const glm::vec3> positions, const glm::vec3& velocity) {
    if (ParticleEntry* e = lookup(m_impl->particleSystems, system, "emitParticles: unknown particle system"))
        e->emitter.emitAt(positions, velocity);
}

void Scene::restartParticles(ParticleSystemId system) {
    if (ParticleEntry* e = lookup(m_impl->particleSystems, system, "restartParticles: unknown particle system"))
        e->emitter.restart();
}

void Scene::removeParticles(ParticleSystemId system) {
    const ParticleEntry* e = lookup(m_impl->particleSystems, system, "removeParticles: unknown particle system");
    if (!e) return;
    m_impl->particlePool.release(e->offset, e->emitter.capacity());
    m_impl->particleSystems.remove(system);
}

std::uint32_t Scene::particlesAlive(ParticleSystemId system) const {
    const ParticleEntry* e = m_impl->particleSystems.get(system);
    return e ? e->emitter.alive() : 0;
}

const particles::ParticleDesc* Scene::particleDesc(ParticleSystemId system) const {
    const ParticleEntry* e = m_impl->particleSystems.get(system);
    return e ? &e->emitter.desc() : nullptr;
}

TrailId Scene::addTrail(const particles::TrailDesc& desc) {
    return m_impl->trails.add(TrailEntry{.trail = particles::Trail(desc)});
}

void Scene::setTrail(TrailId trail, const particles::TrailDesc& desc) {
    if (TrailEntry* e = lookup(m_impl->trails, trail, "setTrail: unknown trail")) e->trail.setDesc(desc);
}

void Scene::moveTrail(TrailId trail, const glm::vec3& head, double time, bool teleport) {
    TrailEntry* e = lookup(m_impl->trails, trail, "moveTrail: unknown trail");
    if (!e) return;
    particles::Trail& t = e->trail;
    // The sparkles along the move, at their share of it (none for a jump or the first move).
    if (e->sparklesPerMeter > 0.0f && t.emitting() && t.started() && !teleport) {
        ParticleEntry* system = m_impl->particleSystems.get(e->sparkles);
        const glm::vec3 from = t.head();
        const float carry = e->sparkleCarry;
        const float added = glm::length(head - from) * e->sparklesPerMeter;
        const float count = carry + added;
        const auto n = std::uint32_t(std::min(count, float(kMaxSparklesPerMove)));
        e->sparkleCarry = count - std::floor(count);
        if (system && n > 0) {
            const double dt = time - t.time();
            const glm::vec3 velocity =
                dt > 1e-6 ? (head - from) / float(dt) * system->emitter.desc().inherit : glm::vec3(0.0f);
            // Sparkle k where the count along the move reaches k.
            std::array<glm::vec3, kMaxSparklesPerMove> places;
            for (std::uint32_t k = 0; k < n; ++k)
                places[k] = from + (head - from) * std::min((float(k + 1) - carry) / added, 1.0f);
            system->emitter.emitAt(std::span(places).first(n), velocity);
        }
    }
    t.move(head, time, teleport);
    ++e->moves;
}

void Scene::setTrailEmitting(TrailId trail, bool emitting) {
    if (TrailEntry* e = lookup(m_impl->trails, trail, "setTrailEmitting: unknown trail"))
        e->trail.setEmitting(emitting);
}

void Scene::setTrailVisible(TrailId trail, bool visible) {
    if (TrailEntry* e = lookup(m_impl->trails, trail, "setTrailVisible: unknown trail")) e->visible = visible;
}

void Scene::clearTrail(TrailId trail) {
    if (TrailEntry* e = lookup(m_impl->trails, trail, "clearTrail: unknown trail")) e->trail.clear();
}

void Scene::setTrailSparkles(TrailId trail, ParticleSystemId system, float perMeter) {
    if (TrailEntry* e = lookup(m_impl->trails, trail, "setTrailSparkles: unknown trail")) {
        e->sparkles = system;
        e->sparklesPerMeter = std::max(perMeter, 0.0f);
    }
}

void Scene::removeTrail(TrailId trail) {
    if (lookup(m_impl->trails, trail, "removeTrail: unknown trail")) m_impl->trails.remove(trail);
}

const particles::Trail* Scene::trail(TrailId trail) const {
    const TrailEntry* e = m_impl->trails.get(trail);
    return e ? &e->trail : nullptr;
}

bool Scene::contains(ParticleSystemId system) const { return m_impl->particleSystems.get(system) != nullptr; }
bool Scene::contains(TrailId trail) const { return m_impl->trails.get(trail) != nullptr; }
std::uint32_t Scene::particleSystemCount() const { return m_impl->particleSystems.size(); }
std::uint32_t Scene::particleSlots() const { return m_impl->particlePool.used; }
std::uint32_t Scene::trailCount() const { return m_impl->trails.size(); }

} // namespace quench::render
