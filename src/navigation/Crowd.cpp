#include <quench/navigation/Crowd.h>

#include "Internal.h"

#include <quench/core/Log.h>

#include <DetourCrowd.h>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

namespace quench::navigation {
namespace {

struct CrowdFree {
    void operator()(dtCrowd* crowd) const { dtFreeCrowd(crowd); }
};
struct QueryFree {
    void operator()(dtNavMeshQuery* query) const { dtFreeNavMeshQuery(query); }
};

glm::vec3 vec(const float* v) { return {v[0], v[1], v[2]}; }

float distance2D(glm::vec3 a, glm::vec3 b) { return glm::length(glm::vec2(a.x - b.x, a.z - b.z)); }

struct Slot {
    std::uint32_t generation = 0;
    bool used = false;
    AgentParams params;
    bool hasTarget = false;
    glm::vec3 target{0.0f};
    bool arrived = false;
    unsigned char lastState = DT_CROWDAGENT_STATE_INVALID;
    dtPolyRef nextLink = 0; // the link the agent's corners lead into before the step
    std::uint32_t link = 0;
    glm::vec3 linkStart{0.0f};
    glm::vec3 linkEnd{0.0f};
};

} // namespace

struct Crowd::Impl {
    dtNavMesh* mesh = nullptr;
    CrowdSettings settings;
    std::unique_ptr<dtCrowd, CrowdFree> crowd;
    std::unique_ptr<dtNavMeshQuery, QueryFree> query;
    Filter defaultFilter;
    // Detour's visibility shortcut (DT_CROWD_OPTIMIZE_VIS) takes any straight way the filter lets through, whatever
    // its areas cost: only for filters whose areas all cost the same.
    std::array<bool, DT_CROWD_MAX_QUERY_FILTER_TYPE> shortcuts{};
    std::vector<Slot> slots;
    int count = 0;
    std::vector<CrowdEvent> events;

    int index(AgentId id) const {
        if (id.index >= slots.size()) return -1;
        const Slot& slot = slots[id.index];
        return slot.used && slot.generation == id.generation ? int(id.index) : -1;
    }

    dtCrowdAgentParams toDetour(const AgentParams& p) const {
        dtCrowdAgentParams out{};
        out.radius = std::min(p.radius, settings.maxAgentRadius);
        out.height = p.height;
        out.maxAcceleration = p.maxAcceleration;
        out.maxSpeed = p.maxSpeed;
        out.collisionQueryRange = p.collisionRange > 0.0f ? p.collisionRange : p.radius * 12.0f;
        out.pathOptimizationRange = p.pathOptimizationRange > 0.0f ? p.pathOptimizationRange : p.radius * 30.0f;
        out.separationWeight = p.separation;
        out.queryFilterType = (unsigned char)std::clamp(p.filter, 0, DT_CROWD_MAX_QUERY_FILTER_TYPE - 1);
        out.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_TOPO;
        if (shortcuts[out.queryFilterType]) out.updateFlags |= DT_CROWD_OPTIMIZE_VIS;
        if (p.avoidance != Avoidance::Off) out.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE;
        if (p.separation > 0.0f) out.updateFlags |= DT_CROWD_SEPARATION;
        out.obstacleAvoidanceType = (unsigned char)(p.avoidance == Avoidance::Off ? 0 : int(p.avoidance) - 1);
        return out;
    }

    // Puts a lost agent back on the nearest polygon (its own was rebuilt away, or the mesh was not there yet).
    void recover(int i) {
        dtCrowdAgent* agent = crowd->getEditableAgent(i);
        const float* extents = crowd->getQueryHalfExtents();
        const float search[3] = {extents[0] * 2.0f, extents[1] * 2.0f, extents[2] * 2.0f};
        dtPolyRef ref = 0;
        float nearest[3];
        if (dtStatusFailed(query->findNearestPoly(agent->npos, search, crowd->getFilter(agent->params.queryFilterType),
                                                  &ref, nearest)) ||
            ref == 0)
            return;
        agent->corridor.reset(ref, nearest);
        agent->boundary.reset();
        std::memcpy(agent->npos, nearest, sizeof(nearest));
        std::memset(agent->vel, 0, sizeof(agent->vel));
        std::memset(agent->dvel, 0, sizeof(agent->dvel));
        std::memset(agent->nvel, 0, sizeof(agent->nvel));
        agent->nneis = 0;
        agent->ncorners = 0;
        agent->topologyOptTime = 0.0f;
        agent->state = DT_CROWDAGENT_STATE_WALKING;
        Slot& slot = slots[std::size_t(i)];
        if (slot.hasTarget) requestTarget(i, slot.target);
    }

    bool requestTarget(int i, glm::vec3 target) {
        const dtCrowdAgent* agent = crowd->getAgent(i);
        dtPolyRef ref = 0;
        float nearest[3];
        if (dtStatusFailed(query->findNearestPoly(&target.x, crowd->getQueryHalfExtents(),
                                                  crowd->getFilter(agent->params.queryFilterType), &ref, nearest)) ||
            ref == 0)
            return false;
        return crowd->requestMoveTarget(i, ref, nearest);
    }
};

Crowd::Crowd(NavMesh& mesh, const CrowdSettings& settings) : m_impl(std::make_unique<Impl>()) {
    Impl& m = *m_impl;
    m.mesh = detail::NavMeshAccess::mesh(mesh);
    m.settings = settings;
    m.settings.maxAgents = std::max(1, settings.maxAgents);
    m.crowd.reset(dtAllocCrowd());
    m.query.reset(dtAllocNavMeshQuery());
    // Both fail only when memory runs out.
    const bool crowdReady = m.crowd && m.crowd->init(m.settings.maxAgents, m.settings.maxAgentRadius, m.mesh);
    const bool queryReady = m.query && !dtStatusFailed(m.query->init(m.mesh, 2048));
    QUENCH_ASSERT(crowdReady && queryReady, "dtCrowd::init or dtNavMeshQuery::init failed");
    (void)crowdReady;
    (void)queryReady;
    m.slots.resize(std::size_t(m.settings.maxAgents));
    m.defaultFilter = Filter::of(mesh);
    for (int i = 0; i < DT_CROWD_MAX_QUERY_FILTER_TYPE; ++i) setFilter(i, m.defaultFilter);

    // DetourCrowd's sample settings for four qualities (RecastDemo's CrowdTool).
    dtObstacleAvoidanceParams params = *m.crowd->getObstacleAvoidanceParams(0);
    const unsigned char quality[4][3] = {{5, 2, 1}, {5, 2, 2}, {7, 2, 3}, {7, 3, 3}};
    for (int i = 0; i < 4; ++i) {
        params.velBias = 0.5f;
        params.adaptiveDivs = quality[i][0];
        params.adaptiveRings = quality[i][1];
        params.adaptiveDepth = quality[i][2];
        m.crowd->setObstacleAvoidanceParams(i, &params);
    }
}

Crowd::Crowd(Crowd&&) noexcept = default;
Crowd& Crowd::operator=(Crowd&&) noexcept = default;
Crowd::~Crowd() = default;

Result<AgentId> Crowd::addAgent(glm::vec3 position, const AgentParams& params) {
    Impl& m = *m_impl;
    const dtCrowdAgentParams p = m.toDetour(params);
    const int i = m.crowd->addAgent(&position.x, &p);
    if (i < 0) return Error{std::format("the crowd is full ({} agents)", m.settings.maxAgents)};
    Slot& slot = m.slots[std::size_t(i)];
    const std::uint32_t generation = slot.generation;
    slot = Slot{};
    slot.generation = generation;
    slot.used = true;
    slot.params = params;
    slot.lastState = m.crowd->getAgent(i)->state;
    ++m.count;
    return AgentId{std::uint32_t(i), generation};
}

void Crowd::removeAgent(AgentId id) {
    Impl& m = *m_impl;
    const int i = m.index(id);
    if (i < 0) return;
    m.crowd->removeAgent(i);
    m.slots[std::size_t(i)].used = false;
    ++m.slots[std::size_t(i)].generation;
    --m.count;
}

bool Crowd::contains(AgentId id) const { return m_impl->index(id) >= 0; }

int Crowd::agentCount() const { return m_impl->count; }

std::vector<AgentId> Crowd::agents() const {
    std::vector<AgentId> out;
    for (std::size_t i = 0; i < m_impl->slots.size(); ++i)
        if (m_impl->slots[i].used) out.push_back({std::uint32_t(i), m_impl->slots[i].generation});
    return out;
}

void Crowd::setParams(AgentId id, const AgentParams& params) {
    Impl& m = *m_impl;
    const int i = m.index(id);
    if (i < 0) return;
    m.slots[std::size_t(i)].params = params;
    const dtCrowdAgentParams p = m.toDetour(params);
    m.crowd->updateAgentParameters(i, &p);
}

const AgentParams* Crowd::params(AgentId id) const {
    const int i = m_impl->index(id);
    return i < 0 ? nullptr : &m_impl->slots[std::size_t(i)].params;
}

bool Crowd::setTarget(AgentId id, glm::vec3 target) {
    Impl& m = *m_impl;
    const int i = m.index(id);
    if (i < 0) return false;
    Slot& slot = m.slots[std::size_t(i)];
    if (!m.requestTarget(i, target)) return false;
    slot.hasTarget = true;
    slot.target = target;
    slot.arrived = false;
    return true;
}

void Crowd::clearTarget(AgentId id) {
    Impl& m = *m_impl;
    const int i = m.index(id);
    if (i < 0) return;
    m.crowd->resetMoveTarget(i);
    m.slots[std::size_t(i)].hasTarget = false;
    m.slots[std::size_t(i)].arrived = false;
}

void Crowd::setVelocity(AgentId id, glm::vec3 velocity) {
    Impl& m = *m_impl;
    const int i = m.index(id);
    if (i < 0) return;
    m.crowd->requestMoveVelocity(i, &velocity.x);
    m.slots[std::size_t(i)].hasTarget = false;
    m.slots[std::size_t(i)].arrived = false;
}

void Crowd::setFilter(int index, const Filter& filter) {
    Impl& m = *m_impl;
    dtQueryFilter* f = m.crowd->getEditableFilter(index);
    if (!f) return;
    *f = detail::toDetour(filter);
    bool uniform = true;
    float cost = -1.0f;
    for (int area = 0; area < kMaxAreas; ++area) {
        if (!(filter.includeAreas & (1u << area)) || (filter.excludeAreas & (1u << area))) continue;
        if (cost < 0.0f) cost = filter.costs[std::size_t(area)];
        uniform = uniform && filter.costs[std::size_t(area)] == cost;
    }
    if (m.shortcuts[std::size_t(index)] == uniform) return;
    m.shortcuts[std::size_t(index)] = uniform;
    for (std::size_t i = 0; i < m.slots.size(); ++i) {
        if (!m.slots[i].used || m.crowd->getAgent(int(i))->params.queryFilterType != index) continue;
        const dtCrowdAgentParams p = m.toDetour(m.slots[i].params);
        m.crowd->updateAgentParameters(int(i), &p);
    }
}

void Crowd::update(float dt) {
    Impl& m = *m_impl;
    m.events.clear();
    for (std::size_t i = 0; i < m.slots.size(); ++i) {
        Slot& slot = m.slots[i];
        if (!slot.used) continue;
        const dtCrowdAgent* agent = m.crowd->getAgent(int(i));
        if (agent->state == DT_CROWDAGENT_STATE_INVALID) m.recover(int(i));
        slot.nextLink = 0;
        if (agent->ncorners > 0 && (agent->cornerFlags[agent->ncorners - 1] & DT_STRAIGHTPATH_OFFMESH_CONNECTION))
            slot.nextLink = agent->cornerPolys[agent->ncorners - 1];
        slot.lastState = agent->state;
    }

    m.crowd->update(dt, nullptr);

    for (std::size_t i = 0; i < m.slots.size(); ++i) {
        Slot& slot = m.slots[i];
        if (!slot.used) continue;
        const dtCrowdAgent* agent = m.crowd->getAgent(int(i));
        const AgentId id{std::uint32_t(i), slot.generation};
        const glm::vec3 position = vec(agent->npos);
        if (agent->state == DT_CROWDAGENT_STATE_OFFMESH && slot.lastState != DT_CROWDAGENT_STATE_OFFMESH) {
            slot.link = 0;
            slot.linkStart = slot.linkEnd = position;
            if (const dtOffMeshConnection* link = m.mesh->getOffMeshConnectionByRef(slot.nextLink)) {
                slot.link = link->userId;
                const glm::vec3 a = vec(&link->pos[0]), b = vec(&link->pos[3]);
                const bool forward = glm::distance(position, a) <= glm::distance(position, b);
                slot.linkStart = forward ? a : b;
                slot.linkEnd = forward ? b : a;
            }
            m.events.push_back({CrowdEvent::Type::LinkStarted, id, slot.link, slot.linkStart, slot.linkEnd});
        } else if (agent->state != DT_CROWDAGENT_STATE_OFFMESH && slot.lastState == DT_CROWDAGENT_STATE_OFFMESH) {
            m.events.push_back({CrowdEvent::Type::LinkFinished, id, slot.link, slot.linkStart, slot.linkEnd});
        }
        if (slot.hasTarget && !slot.arrived && agent->state == DT_CROWDAGENT_STATE_WALKING &&
            agent->targetState == DT_CROWDAGENT_TARGET_VALID) {
            const glm::vec3 end = vec(agent->corridor.getTarget());
            const float reach = slot.params.arrivalDistance > 0.0f ? slot.params.arrivalDistance : slot.params.radius;
            if ((agent->corridor.getLastPoly() == agent->targetRef || agent->partial) &&
                distance2D(position, end) <= reach && std::abs(position.y - end.y) <= slot.params.height) {
                slot.arrived = true;
                m.events.push_back({CrowdEvent::Type::Arrived, id, 0, position, end});
            }
        }
    }
}

std::span<const CrowdEvent> Crowd::events() const { return m_impl->events; }

AgentState Crowd::state(AgentId id) const {
    const Impl& m = *m_impl;
    AgentState out;
    const int i = m.index(id);
    if (i < 0) return out;
    const dtCrowdAgent* agent = m.crowd->getAgent(i);
    const Slot& slot = m.slots[std::size_t(i)];
    out.position = vec(agent->npos);
    out.velocity = vec(agent->vel);
    out.desiredVelocity = vec(agent->dvel);
    out.move = agent->state == DT_CROWDAGENT_STATE_OFFMESH   ? AgentMove::OnLink
               : agent->state == DT_CROWDAGENT_STATE_WALKING ? AgentMove::Walking
                                                             : AgentMove::Lost;
    out.hasTarget = slot.hasTarget;
    out.target = slot.target;
    out.arrived = slot.arrived;
    out.partial = agent->partial;
    if (out.move == AgentMove::OnLink) {
        out.link = slot.link;
        out.linkStart = slot.linkStart;
        out.linkEnd = slot.linkEnd;
        const glm::vec3 along = slot.linkEnd - slot.linkStart;
        const float length2 = glm::dot(along, along);
        out.linkProgress =
            length2 > 0.0f ? std::clamp(glm::dot(out.position - slot.linkStart, along) / length2, 0.0f, 1.0f) : 1.0f;
    }
    return out;
}

std::vector<glm::vec3> Crowd::route(AgentId id, int maxPoints) const {
    const Impl& m = *m_impl;
    std::vector<glm::vec3> out;
    const int i = m.index(id);
    if (i < 0 || maxPoints <= 0) return out;
    const dtCrowdAgent* agent = m.crowd->getAgent(i);
    if (agent->state != DT_CROWDAGENT_STATE_WALKING || agent->corridor.getPathCount() == 0) return out;
    std::vector<float> points(std::size_t(maxPoints) * 3);
    int count = 0;
    if (dtStatusFailed(m.query->findStraightPath(agent->npos, agent->corridor.getTarget(), agent->corridor.getPath(),
                                                 agent->corridor.getPathCount(), points.data(), nullptr, nullptr,
                                                 &count, maxPoints, 0)))
        return out;
    for (int k = 0; k < count; ++k) out.push_back(vec(&points[std::size_t(k) * 3]));
    return out;
}

} // namespace quench::navigation
