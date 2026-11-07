#pragma once

// Agents moving on a NavMesh together (DetourCrowd underneath): each follows a path to its target, steers around the
// others (local avoidance and separation) and crosses off-mesh links. Crowd::update() runs on the game's fixed step;
// positions and velocities are read after it. Agents keep moving while tiles are rebuilt: an agent whose polygon went
// away is put back on the nearest polygon, and paths through rebuilt tiles are planned again.
//
// An agent crosses a link in DetourCrowd's own move: a straight line from start to end at about twice its maximum
// speed (half the link's horizontal length over maxSpeed). The game hears of it through events and AgentState::link
// (to play a jump, for example).

#include <quench/core/Result.h>
#include <quench/navigation/NavMesh.h>
#include <quench/navigation/NavQuery.h>

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace quench::navigation {

struct CrowdSettings {
    int maxAgents = 128;
    float maxAgentRadius = 0.6f; // the largest AgentParams::radius used (it sizes the neighbor searches)
};

// How hard an agent looks for a velocity that misses the others: Off saves the most time, High the most collisions.
enum class Avoidance : std::uint8_t { Off, Low, Medium, Good, High };

struct AgentParams {
    float radius = 0.4f;
    float height = 1.8f;
    float maxSpeed = 3.5f;
    float maxAcceleration = 8.0f;
    float separation = 2.0f;     // how strongly agents keep apart; 0: not at all
    float collisionRange = 0.0f; // the neighbors and walls it steers around; 0: radius * 12
    // How far ahead it shortcuts its path along straight lines; 0: radius * 30. Only with a filter whose areas all
    // cost the same (a straight line does not weigh the areas it crosses).
    float pathOptimizationRange = 0.0f;
    Avoidance avoidance = Avoidance::Medium;
    int filter = 0;               // Crowd::setFilter's index, 0 to 15
    float arrivalDistance = 0.0f; // closer than this to its target it has arrived; 0: radius
};

struct AgentId {
    std::uint32_t index = ~0u;
    std::uint32_t generation = 0;

    bool valid() const { return index != ~0u; }
    bool operator==(const AgentId&) const = default;
};

enum class AgentMove : std::uint8_t {
    Walking, // on the mesh
    OnLink,  // crossing an off-mesh link
    Lost,    // off the mesh (no polygon near it): it stands still until the mesh comes back
};

struct AgentState {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 desiredVelocity{0.0f}; // where its path leads, before avoidance
    AgentMove move = AgentMove::Walking;
    bool hasTarget = false;
    glm::vec3 target{0.0f};
    bool arrived = false;
    bool partial = false; // the target is not reachable: it walks to the nearest reachable point
    // OnLink: the link's id, its ends as crossed and how far along it is (0 to 1).
    std::uint32_t link = 0;
    glm::vec3 linkStart{0.0f};
    glm::vec3 linkEnd{0.0f};
    float linkProgress = 0.0f;
};

struct CrowdEvent {
    enum class Type : std::uint8_t { LinkStarted, LinkFinished, Arrived };
    Type type = Type::Arrived;
    AgentId agent;
    std::uint32_t link = 0; // links: the Link::id
    glm::vec3 start{0.0f};  // links: the ends as crossed
    glm::vec3 end{0.0f};
};

class Crowd {
public:
    // The mesh outlives the crowd.
    explicit Crowd(NavMesh& mesh, const CrowdSettings& settings = {});
    Crowd(Crowd&&) noexcept;
    Crowd& operator=(Crowd&&) noexcept;
    ~Crowd();

    // Puts an agent on the mesh near `position`: an error when the crowd is full. An agent with no mesh near it (its
    // tiles not built yet) is Lost until there is one.
    Result<AgentId> addAgent(glm::vec3 position, const AgentParams& params = {});
    void removeAgent(AgentId id);
    bool contains(AgentId id) const;
    int agentCount() const;
    std::vector<AgentId> agents() const;

    void setParams(AgentId id, const AgentParams& params);
    const AgentParams* params(AgentId id) const;
    // Sends the agent to the point of the mesh nearest to `target`; false when no mesh is near it.
    bool setTarget(AgentId id, glm::vec3 target);
    void clearTarget(AgentId id);
    // Steers the agent by a velocity instead of a target (a player's input, for example).
    void setVelocity(AgentId id, glm::vec3 velocity);
    // Filters by index (AgentParams::filter); all start as Filter::of(mesh).
    void setFilter(int index, const Filter& filter);

    // Moves every agent by dt seconds and collects this step's events.
    void update(float dt);
    std::span<const CrowdEvent> events() const;

    AgentState state(AgentId id) const;
    // The corners of the agent's path ahead, from its position (at most maxPoints).
    std::vector<glm::vec3> route(AgentId id, int maxPoints = 32) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace quench::navigation
