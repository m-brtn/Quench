#include "Test.h"

#include <quench/assets/AssetManager.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Jobs.h>
#include <quench/core/Log.h>
#include <quench/navigation/Crowd.h>
#include <quench/navigation/DebugGeometry.h>
#include <quench/navigation/NavMesh.h>
#include <quench/navigation/NavMeshAsset.h>
#include <quench/navigation/NavQuery.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace quench;
using namespace quench::navigation;

namespace {

// Procedural levels: quads counter-clockwise seen from above, boxes with all six faces.
struct Level {
    Geometry g;

    void quad(glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, AreaId area = kDefaultArea) {
        const auto base = std::uint32_t(g.vertices.size());
        g.vertices.insert(g.vertices.end(), {a, b, c, d});
        g.indices.insert(g.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        g.areas.insert(g.areas.end(), {area, area});
    }
    void box(glm::vec3 lo, glm::vec3 hi, AreaId area = kDefaultArea) {
        quad({lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z}, area);
        quad({lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z}, area);
        quad({lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, lo.y, lo.z}, area);
        quad({lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}, area);
        quad({lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z}, area);
        quad({hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {hi.x, lo.y, hi.z}, area);
    }
};

BuildSettings settings(int tileSize = 32) {
    BuildSettings s;
    s.agent.radius = 0.3f;
    s.agent.height = 1.8f;
    s.agent.maxClimb = 0.35f;
    s.agent.maxSlope = 40.0f;
    s.cellSize = 0.2f;
    s.cellHeight = 0.1f;
    s.tileSize = tileSize;
    s.areas = {{"ground", 1.0f}, {"mud", 1.0f}};
    return s;
}

// A ground floor with a raised platform (A) reached by a ramp, a second platform (C) across a canyon and a narrow
// bridge from A to C; the canyon floor passes under the bridge.
Level rampAndBridge() {
    Level l;
    l.box({-10.0f, -0.5f, -10.0f}, {10.0f, 0.0f, 10.0f});
    l.box({-10.0f, 0.0f, 4.0f}, {-2.0f, 3.0f, 10.0f});
    l.box({2.0f, 0.0f, 4.0f}, {10.0f, 3.0f, 10.0f});
    l.quad({-9.0f, 0.0f, -2.0f}, {-9.0f, 3.0f, 4.0f}, {-5.0f, 3.0f, 4.0f}, {-5.0f, 0.0f, -2.0f});
    l.box({-2.0f, 2.8f, 6.0f}, {2.0f, 3.0f, 7.6f});
    return l;
}

NavMesh built(Level level, const BuildSettings& s, JobSystem& jobs) {
    Result<NavMesh> mesh = NavMesh::build(std::move(level.g), s, jobs);
    QUENCH_ASSERT(mesh.ok(), "NavMesh::build failed");
    mesh->finish();
    return std::move(*mesh);
}

bool corridorHas(const Path& path, std::uint64_t polygon) {
    return std::find(path.corridor.begin(), path.corridor.end(), polygon) != path.corridor.end();
}

// Counts the log lines of a level and category.
struct LogCounter {
    int lines = 0;
    std::string last;
    int id;
    LogCounter(log::Level level, std::string category)
        : id(log::addSink([this, level, category](log::Level l, std::string_view c, std::string_view message) {
              if (l == level && c == category) {
                  ++lines;
                  last = message;
              }
          })) {}
    ~LogCounter() { log::removeSink(id); }
};

} // namespace

QUENCH_TEST(build_small_level) {
    JobSystem jobs;
    Level level;
    level.box({-10.0f, -0.5f, -10.0f}, {10.0f, 0.0f, 10.0f});
    level.box({-1.0f, 0.0f, -1.0f}, {1.0f, 2.0f, 1.0f});
    Result<NavMesh> mesh = NavMesh::build(level.g, settings(), jobs);
    REQUIRE(mesh.ok(), "build: %s", mesh.error().c_str());
    CHECK(!mesh->ready(), "ready before any tile was added");
    mesh->finish();
    const NavMeshStats s = mesh->stats();
    CHECK(mesh->ready() && s.pendingTiles == 0, "%d tiles pending", s.pendingTiles);
    CHECK(s.tilesX == 4 && s.tilesZ == 4, "%d x %d tiles (20 m of 6.4 m tiles)", s.tilesX, s.tilesZ);
    CHECK(s.tiles >= 16 && s.polygons > 16 && s.memoryBytes > 0, "%d tiles, %d polygons, %llu bytes", s.tiles,
          s.polygons, (unsigned long long)s.memoryBytes);
    CHECK(mesh->revision() > 0, "revision not bumped");

    NavQuery query(*mesh);
    Result<NavPoint> onFloor = query.nearestPoint({5.0f, 1.0f, 5.0f});
    REQUIRE(onFloor.ok(), "nearest point: %s", onFloor.error().c_str());
    CHECK(std::abs(onFloor->position.y) < 0.15f && onFloor->area == 0, "floor point at y %.2f, area %d",
          onFloor->position.y, int(onFloor->area));
    // The pillar: the straight line is blocked, the path goes around it, a radius off its sides.
    Result<Path> path = query.findPath({-5.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f});
    REQUIRE(path.ok(), "path: %s", path.error().c_str());
    CHECK(!path->partial && path->points.size() >= 3, "%zu points, partial %d", path->points.size(),
          int(path->partial));
    CHECK(path->length() > 10.05f && path->length() < 11.5f, "length %.2f around a 2 m pillar", path->length());
    for (const PathPoint& p : path->points)
        CHECK(std::max(std::abs(p.position.x), std::abs(p.position.z)) > 1.2f, "corner (%.2f, %.2f) in the pillar",
              p.position.x, p.position.z);
}

QUENCH_TEST(bad_input_is_an_error) {
    JobSystem jobs(JobSystemDesc{.workerThreads = 0});
    Level level;
    level.box({-2.0f, -0.5f, -2.0f}, {2.0f, 0.0f, 2.0f});
    Geometry badIndex = level.g;
    badIndex.indices.push_back(9999);
    badIndex.indices.push_back(0);
    badIndex.indices.push_back(1);
    badIndex.areas.push_back(0);
    CHECK(!NavMesh::build(badIndex, settings(), jobs).ok(), "an index out of range was accepted");
    Geometry badArea = level.g;
    badArea.areas[0] = 5;
    CHECK(!NavMesh::build(badArea, settings(), jobs).ok(), "an undefined area type was accepted");
    BuildSettings badTile = settings();
    badTile.tileSize = 300;
    CHECK(!NavMesh::build(level.g, badTile, jobs).ok(), "a 300-cell tile was accepted");
    BuildSettings badAgent = settings();
    badAgent.agent.height = 0.0f;
    CHECK(!NavMesh::build(level.g, badAgent, jobs).ok(), "an agent without height was accepted");
    CHECK(!NavMesh::build(Geometry{}, settings(), jobs).ok(), "empty geometry was accepted");
}

QUENCH_TEST(path_over_ramp_and_bridge) {
    JobSystem jobs;
    NavMesh mesh = built(rampAndBridge(), settings(), jobs);
    NavQuery query(mesh);
    // Two floors in one tile column: the bridge and the canyon under it.
    Result<NavPoint> bridge = query.nearestPoint({0.0f, 3.2f, 6.8f});
    Result<NavPoint> canyon = query.nearestPoint({0.0f, 0.2f, 6.8f});
    REQUIRE(bridge.ok() && canyon.ok(), "bridge: %s; canyon: %s", bridge.error().c_str(), canyon.error().c_str());
    CHECK(std::abs(bridge->position.y - 3.0f) < 0.2f && std::abs(canyon->position.y) < 0.2f,
          "bridge at y %.2f, canyon at y %.2f", bridge->position.y, canyon->position.y);
    CHECK(bridge->polygon != canyon->polygon, "one polygon for both floors");
    CHECK(mesh.stats().tiles > mesh.stats().tilesX * mesh.stats().tilesZ, "%d tiles in %d columns: no second layer",
          mesh.stats().tiles, mesh.stats().tilesX * mesh.stats().tilesZ);

    // Ground to platform C: up the ramp, over platform A and the bridge.
    Result<Path> up = query.findPath({0.0f, 0.0f, -6.0f}, {6.0f, 3.0f, 8.0f});
    REQUIRE(up.ok(), "path: %s", up.error().c_str());
    CHECK(!up->partial, "the platform was not reached");
    CHECK(std::abs(up->points.back().position.y - 3.0f) < 0.2f, "path ends at y %.2f", up->points.back().position.y);
    CHECK(corridorHas(*up, bridge->polygon), "the path does not cross the bridge");
    bool onRamp = false;
    for (const std::uint64_t polygon : up->corridor)
        for (const PathPoint& p : up->points)
            if (p.polygon == polygon && p.position.x < -4.5f && p.position.y > 0.5f && p.position.y < 2.9f)
                onRamp = true;
    const auto rampCorner = std::find_if(up->points.begin(), up->points.end(), [](const PathPoint& p) {
        return p.position.x > -9.0f && p.position.x < -4.5f && p.position.z > -2.5f && p.position.z < 4.5f;
    });
    CHECK(onRamp || rampCorner != up->points.end(), "no corner on the ramp");
    CHECK(up->length() > 20.0f, "length %.2f: too short to go by the ramp", up->length());

    // Under the bridge on the ground: the bridge is not on the way.
    Result<Path> under = query.findPath({0.0f, 0.0f, -6.0f}, {0.0f, 0.0f, 8.5f});
    REQUIRE(under.ok() && !under->partial, "path under the bridge: %s", under.ok() ? "partial" : under.error().c_str());
    CHECK(!corridorHas(*under, bridge->polygon), "the ground path climbs the bridge");
    for (const PathPoint& p : under->points) CHECK(p.position.y < 0.3f, "ground path point at y %.2f", p.position.y);
}

QUENCH_TEST(area_costs_change_the_path) {
    JobSystem jobs;
    Level level;
    level.box({-10.0f, -0.5f, -10.0f}, {10.0f, 0.0f, 10.0f});
    level.g.boxes.push_back({{-3.0f, -1.0f, -3.0f}, {3.0f, 1.0f, 3.0f}, 1});
    BuildSettings s = settings();
    NavMesh mesh = built(level, s, jobs);
    NavQuery query(mesh);
    Result<NavPoint> mud = query.nearestPoint({0.0f, 0.0f, 0.0f});
    REQUIRE(mud.ok() && mud->area == 1, "the box's area: %d", mud.ok() ? int(mud->area) : -1);

    const auto throughMud = [](const Path& path) {
        for (std::size_t i = 1; i < path.points.size(); ++i)
            for (int k = 0; k <= 20; ++k) {
                const glm::vec3 p = glm::mix(path.points[i - 1].position, path.points[i].position, float(k) / 20.0f);
                if (std::abs(p.x) < 2.8f && std::abs(p.z) < 2.8f) return true;
            }
        return false;
    };
    const glm::vec3 a(-8.0f, 0.0f, 0.0f), b(8.0f, 0.0f, 0.0f);
    Result<Path> cheap = query.findPath(a, b);
    REQUIRE(cheap.ok(), "path: %s", cheap.error().c_str());
    CHECK(throughMud(*cheap) && cheap->length() < 16.2f, "mud at cost 1: length %.2f", cheap->length());

    Filter costly = Filter::of(mesh);
    costly.costs[1] = 5.0f;
    Result<Path> around = query.findPath(a, b, &costly);
    REQUIRE(around.ok(), "path: %s", around.error().c_str());
    CHECK(!throughMud(*around) && around->length() > 16.5f, "mud at cost 5: length %.2f", around->length());

    Filter excluded = Filter::of(mesh).exclude(1);
    Result<Path> avoided = query.findPath(a, b, &excluded);
    REQUIRE(avoided.ok(), "path: %s", avoided.error().c_str());
    CHECK(!throughMud(*avoided) && !avoided->partial, "mud excluded: length %.2f", avoided->length());
    CHECK(!query.nearestPoint({0.0f, 0.0f, 0.0f}, &excluded).ok() ||
              query.nearestPoint({0.0f, 0.0f, 0.0f}, &excluded)->area != 1,
          "an excluded area was found");
}

QUENCH_TEST(link_is_used_and_reported) {
    JobSystem jobs;
    Level level;
    level.box({-10.0f, -0.5f, -10.0f}, {10.0f, 0.0f, 10.0f});
    level.box({-10.0f, 0.0f, -5.0f}, {0.0f, 2.0f, 5.0f});
    level.g.links.push_back({{-0.6f, 2.0f, 0.0f}, {1.5f, 0.0f, 0.0f}, 0.5f, false, 0, 7});
    NavMesh mesh = built(level, settings(), jobs);
    REQUIRE(mesh.links().size() == 1, "%zu links", mesh.links().size());
    NavQuery query(mesh);

    Result<Path> down = query.findPath({-5.0f, 2.0f, 2.0f}, {5.0f, 0.0f, 2.0f});
    REQUIRE(down.ok(), "path: %s", down.error().c_str());
    CHECK(!down->partial, "the ground was not reached");
    const auto link =
        std::find_if(down->points.begin(), down->points.end(), [](const PathPoint& p) { return p.linkStart; });
    REQUIRE(link != down->points.end(), "the path does not use the link");
    CHECK(link->link == 7 && std::next(link) != down->points.end() && std::abs(std::next(link)->position.y) < 0.2f,
          "link id %u", link->link);
    // One way: nothing leads back up.
    Result<Path> up = query.findPath({5.0f, 0.0f, 2.0f}, {-5.0f, 2.0f, 2.0f});
    REQUIRE(up.ok(), "path: %s", up.error().c_str());
    CHECK(up->partial, "a one-way link was walked backwards");

    Crowd crowd(mesh);
    Result<AgentId> agent = crowd.addAgent({-5.0f, 2.0f, 2.0f});
    REQUIRE(agent.ok(), "agent: %s", agent.error().c_str());
    REQUIRE(crowd.setTarget(*agent, {5.0f, 0.0f, 2.0f}), "no target");
    bool started = false, finished = false, arrived = false, sawOnLink = false;
    for (int step = 0; step < 60 * 20 && !arrived; ++step) {
        crowd.update(1.0f / 60.0f);
        for (const CrowdEvent& e : crowd.events()) {
            if (e.type == CrowdEvent::Type::LinkStarted) {
                started = true;
                CHECK(e.link == 7 && e.agent == *agent && std::abs(e.start.y - 2.0f) < 0.2f && std::abs(e.end.y) < 0.2f,
                      "link started: id %u, start y %.2f, end y %.2f", e.link, e.start.y, e.end.y);
            }
            if (e.type == CrowdEvent::Type::LinkFinished) finished = started && e.link == 7;
            if (e.type == CrowdEvent::Type::Arrived) arrived = finished;
        }
        const AgentState s = crowd.state(*agent);
        if (s.move == AgentMove::OnLink) {
            sawOnLink = true;
            CHECK(s.link == 7 && s.linkProgress >= 0.0f && s.linkProgress <= 1.0f, "on link %u at %.2f", s.link,
                  s.linkProgress);
        }
    }
    CHECK(started && finished && arrived && sawOnLink, "started %d, finished %d, arrived %d, on link %d", int(started),
          int(finished), int(arrived), int(sawOnLink));
    const AgentState end = crowd.state(*agent);
    CHECK(glm::distance(end.position, glm::vec3(5.0f, 0.0f, 2.0f)) < 0.5f, "agent ends at (%.2f, %.2f, %.2f)",
          end.position.x, end.position.y, end.position.z);
}

QUENCH_TEST(obstacle_blocks_and_unblocks) {
    JobSystem jobs;
    Level level;
    level.box({-10.0f, -0.5f, -2.0f}, {10.0f, 0.0f, 2.0f});
    NavMesh mesh = built(level, settings(), jobs);
    NavQuery query(mesh);
    const glm::vec3 a(-8.0f, 0.0f, 0.0f), b(8.0f, 0.0f, 0.0f);
    const auto path = [&] {
        Result<Path> p = query.findPath(a, b);
        QUENCH_ASSERT(p.ok(), "no path");
        return *p;
    };
    REQUIRE(!path().partial, "the corridor is blocked from the start");

    // A beam along the corridor leaves its sides free; turned across it (yaw), it closes it.
    Obstacle beam;
    beam.shape = ObstacleShape::Box;
    beam.position = {0.0f, 0.5f, 0.0f};
    beam.halfExtents = {2.0f, 0.5f, 0.2f};
    const std::uint64_t before = mesh.revision();
    const ObstacleId id = mesh.addObstacle(beam);
    const NavMeshStats pending = mesh.stats();
    CHECK(pending.pendingTiles > 0 && pending.pendingTiles < pending.tilesX * pending.tilesZ,
          "%d of %d tiles to rebuild", pending.pendingTiles, pending.tilesX * pending.tilesZ);
    mesh.finish();
    CHECK(mesh.revision() > before, "no tile changed");
    const Path along = path();
    CHECK(!along.partial && along.length() > 16.05f, "beam along: partial %d, length %.2f", int(along.partial),
          along.length());
    beam.yaw = glm::half_pi<float>();
    mesh.setObstacle(id, beam);
    mesh.finish();
    CHECK(path().partial, "beam across: the corridor is still open");
    REQUIRE(mesh.obstacle(id) != nullptr && mesh.stats().obstacles == 1, "obstacle lost");

    mesh.removeObstacle(id);
    CHECK(mesh.obstacle(id) == nullptr, "removed obstacle still there");
    mesh.finish();
    const Path open = path();
    CHECK(!open.partial && open.length() < 16.05f, "after removal: partial %d, length %.2f", int(open.partial),
          open.length());

    // A cylinder does the same.
    Obstacle post;
    post.position = {0.0f, 0.0f, 0.0f};
    post.radius = 1.8f;
    post.height = 2.0f;
    const ObstacleId postId = mesh.addObstacle(post);
    mesh.finish();
    CHECK(path().partial, "a 1.8 m post leaves the 4 m corridor open");
    mesh.removeObstacle(postId);
    mesh.finish();
    CHECK(!path().partial, "the corridor stays closed after the post went");
}

QUENCH_TEST(geometry_rebuild_changes_tiles) {
    JobSystem jobs;
    Level level;
    level.box({-10.0f, -0.5f, -2.0f}, {10.0f, 0.0f, 2.0f});
    NavMesh mesh = built(level, settings(), jobs);
    NavQuery query(mesh);
    REQUIRE(!query.findPath({-8.0f, 0.0f, 0.0f}, {8.0f, 0.0f, 0.0f})->partial, "blocked at first");
    Level walled = level;
    walled.box({-0.3f, 0.0f, -2.5f}, {0.3f, 3.0f, 2.5f});
    REQUIRE(mesh.rebuild(walled.g, {-0.3f, 0.0f, -2.5f}, {0.3f, 3.0f, 2.5f}).ok(), "rebuild refused");
    const NavMeshStats s = mesh.stats();
    CHECK(s.pendingTiles > 0 && s.pendingTiles < s.tilesX * s.tilesZ, "%d of %d tiles", s.pendingTiles,
          s.tilesX * s.tilesZ);
    mesh.finish();
    CHECK(query.findPath({-8.0f, 0.0f, 0.0f}, {8.0f, 0.0f, 0.0f})->partial, "the wall did not close the corridor");
}

QUENCH_TEST(crowd_reaches_targets_without_overlap) {
    JobSystem jobs;
    Level level;
    level.box({-15.0f, -0.5f, -15.0f}, {15.0f, 0.0f, 15.0f});
    NavMesh mesh = built(level, settings(), jobs);
    Crowd crowd(mesh);
    std::vector<AgentId> agents;
    std::vector<glm::vec3> targets;
    AgentParams params;
    params.radius = 0.4f;
    params.maxSpeed = 3.0f;
    for (int i = 0; i < 8; ++i) {
        const float z = -7.0f + 2.0f * float(i);
        Result<AgentId> agent = crowd.addAgent({-10.0f, 0.0f, z}, params);
        REQUIRE(agent.ok(), "agent %d: %s", i, agent.error().c_str());
        agents.push_back(*agent);
        targets.push_back({10.0f, 0.0f, -z}); // every path crosses the middle
        REQUIRE(crowd.setTarget(*agent, targets.back()), "target %d", i);
    }
    float closest = 1e9f;
    int arrived = 0, steps = 0;
    for (; steps < 60 * 40 && arrived < int(agents.size()); ++steps) {
        crowd.update(1.0f / 60.0f);
        for (const CrowdEvent& e : crowd.events()) arrived += e.type == CrowdEvent::Type::Arrived;
        for (std::size_t i = 0; i < agents.size(); ++i)
            for (std::size_t k = i + 1; k < agents.size(); ++k) {
                const glm::vec3 d = crowd.state(agents[i]).position - crowd.state(agents[k]).position;
                closest = std::min(closest, std::sqrt(d.x * d.x + d.z * d.z));
            }
    }
    CHECK(arrived == int(agents.size()), "%d of %zu arrived in %d steps", arrived, agents.size(), steps);
    for (std::size_t i = 0; i < agents.size(); ++i) {
        const AgentState s = crowd.state(agents[i]);
        CHECK(s.arrived && glm::distance(s.position, targets[i]) < 0.6f, "agent %zu %.2f m from its target", i,
              glm::distance(s.position, targets[i]));
    }
    // Two radii apart is touching; the crowd's collision resolution lets them press a little.
    CHECK(closest > 0.8f * 0.9f, "agents came %.2f m close (radius 0.4)", closest);
    std::printf("crowd of 8 crossing: %d steps, closest %.3f m\n", steps, closest);
}

QUENCH_TEST(crowd_follows_rebuilt_tiles) {
    JobSystem jobs;
    Level level;
    level.box({-10.0f, -0.5f, -2.0f}, {10.0f, 0.0f, 2.0f});
    NavMesh mesh = built(level, settings(), jobs);
    Crowd crowd(mesh);
    Result<AgentId> agent = crowd.addAgent({-8.0f, 0.0f, 0.0f});
    REQUIRE(agent.ok() && crowd.setTarget(*agent, {8.0f, 0.0f, 0.0f}), "agent");
    // The agent walks while a post comes and goes in front of it; it gets there in the end.
    Obstacle post;
    post.position = {0.0f, 0.0f, 0.0f};
    post.radius = 0.5f;
    const ObstacleId id = mesh.addObstacle(post);
    bool arrived = false;
    for (int step = 0; step < 60 * 20 && !arrived; ++step) {
        if (step == 60) mesh.removeObstacle(id);
        mesh.update();
        crowd.update(1.0f / 60.0f);
        arrived = crowd.state(*agent).arrived;
        CHECK(crowd.state(*agent).move != AgentMove::Lost || step < 5, "agent lost at step %d", step);
    }
    CHECK(arrived, "the agent did not arrive");
}

QUENCH_TEST(crowd_keeps_area_costs) {
    JobSystem jobs;
    Level level;
    level.box({-10.0f, -0.5f, -10.0f}, {10.0f, 0.0f, 10.0f});
    level.g.boxes.push_back({{-3.0f, -1.0f, -3.0f}, {3.0f, 1.0f, 3.0f}, 1});
    NavMesh mesh = built(level, settings(), jobs);
    // Across the mud at cost 1 the agent walks straight through it; at cost 5 around it, also where a straight
    // shortcut would be free to take.
    for (const float cost : {1.0f, 5.0f}) {
        Crowd crowd(mesh);
        Filter filter = Filter::of(mesh);
        filter.costs[1] = cost;
        crowd.setFilter(0, filter);
        Result<AgentId> agent = crowd.addAgent({-8.0f, 0.0f, 0.5f});
        REQUIRE(agent.ok() && crowd.setTarget(*agent, {8.0f, 0.0f, 0.5f}), "agent");
        int inMud = 0;
        bool arrived = false;
        for (int step = 0; step < 60 * 20 && !arrived; ++step) {
            crowd.update(1.0f / 60.0f);
            const AgentState s = crowd.state(*agent);
            inMud += std::abs(s.position.x) < 2.5f && std::abs(s.position.z) < 2.5f;
            arrived = s.arrived;
        }
        CHECK(arrived, "cost %.0f: the agent did not arrive", double(cost));
        CHECK(cost == 1.0f ? inMud > 30 : inMud == 0, "cost %.0f: %d steps in the mud", double(cost), inMud);
    }
}

QUENCH_TEST(raycast_and_random_points) {
    JobSystem jobs;
    Level level;
    level.box({-10.0f, -0.5f, -10.0f}, {10.0f, 0.0f, 10.0f});
    level.box({2.0f, 0.0f, -10.0f}, {3.0f, 2.0f, 10.0f});
    NavMesh mesh = built(level, settings(), jobs);
    NavQuery query(mesh);
    Result<RaycastHit> wall = query.raycast({-5.0f, 0.0f, 0.0f}, {8.0f, 0.0f, 0.0f});
    REQUIRE(wall.ok(), "raycast: %s", wall.error().c_str());
    CHECK(wall->hit && std::abs(wall->position.x - 1.7f) < 0.25f && wall->normal.x < -0.9f,
          "hit %d at x %.2f, normal x %.2f", int(wall->hit), wall->position.x, wall->normal.x);
    Result<RaycastHit> free = query.raycast({-5.0f, 0.0f, 0.0f}, {-5.0f, 0.0f, 8.0f});
    REQUIRE(free.ok(), "raycast: %s", free.error().c_str());
    CHECK(!free->hit && free->t == 1.0f, "an open line hit at t %.2f", free->t);

    std::uint64_t seedA = 42, seedB = 42;
    for (int i = 0; i < 20; ++i) {
        Result<NavPoint> a = query.randomPoint(seedA);
        Result<NavPoint> b = query.randomPoint(seedB);
        REQUIRE(a.ok() && b.ok(), "random point");
        CHECK(a->position == b->position, "seed 42 gave two points");
        CHECK(std::abs(a->position.y) < 0.15f && std::abs(a->position.x) <= 9.8f, "random point (%.2f, %.2f, %.2f)",
              a->position.x, a->position.y, a->position.z);
    }
    Result<NavPoint> near = query.randomPointAround({-5.0f, 0.0f, 0.0f}, 2.0f, seedA);
    REQUIRE(near.ok(), "random point around: %s", near.error().c_str());
    CHECK(near->position.x < 1.8f, "a point around (-5, 0) behind the wall at x %.2f", near->position.x);
}

QUENCH_TEST(debug_geometry) {
    JobSystem jobs;
    Level level;
    level.box({-6.0f, -0.5f, -6.0f}, {6.0f, 0.0f, 6.0f});
    level.g.boxes.push_back({{-2.0f, -1.0f, -2.0f}, {2.0f, 1.0f, 2.0f}, 1});
    level.g.links.push_back({{-3.0f, 0.0f, 4.0f}, {3.0f, 0.0f, 4.0f}, 0.4f, true, 0, 3});
    NavMesh mesh = built(level, settings(), jobs);
    Obstacle post;
    post.position = {4.0f, 0.0f, -4.0f};
    mesh.addObstacle(post);
    mesh.finish();
    DebugGeometry debug;
    addNavMesh(debug, mesh);
    addObstacles(debug, mesh, {1.0f, 0.0f, 0.0f, 1.0f});
    REQUIRE(!debug.triangles.empty() && debug.triangles.size() % 3 == 0 && debug.lines.size() % 2 == 0,
            "%zu triangle vertices, %zu line vertices", debug.triangles.size(), debug.lines.size());
    int mud = 0;
    for (std::size_t i = 0; i < debug.triangles.size(); i += 3) {
        const glm::vec3 n = glm::cross(debug.triangles[i + 1].position - debug.triangles[i].position,
                                       debug.triangles[i + 2].position - debug.triangles[i].position);
        CHECK(n.y >= 0.0f, "triangle %zu faces down", i / 3);
        mud += debug.triangles[i].color == areaColor(1);
    }
    CHECK(mud > 0, "no triangle in the mud's color");
    const std::size_t meshLines = debug.lines.size();
    Crowd crowd(mesh);
    REQUIRE(crowd.addAgent({0.0f, 0.0f, -4.0f}).ok(), "agent");
    addAgents(debug, crowd, {1.0f, 1.0f, 1.0f, 1.0f});
    CHECK(debug.lines.size() > meshLines, "no agent lines");
}

QUENCH_TEST(tiled_build_is_deterministic) {
    const auto build = [](int workers) {
        JobSystem jobs(JobSystemDesc{.workerThreads = workers});
        Level level = rampAndBridge();
        level.g.boxes.push_back({{-8.0f, -1.0f, -8.0f}, {-2.0f, 1.0f, -4.0f}, 1});
        Result<NavMeshData> data = buildNavMeshData(level.g, settings(24), jobs);
        QUENCH_ASSERT(data.ok(), "build failed");
        NavMesh mesh = built(level, settings(24), jobs);
        return std::make_pair(writeNavMeshData(*data), mesh.contentHash());
    };
    const auto one = build(0);
    const auto many = build(4);
    const auto again = build(4);
    CHECK(one.first == many.first && many.first == again.first, "cooked bytes differ: %zu, %zu, %zu", one.first.size(),
          many.first.size(), again.first.size());
    CHECK(one.second == many.second && many.second == again.second, "tile hashes differ: %llx, %llx, %llx",
          (unsigned long long)one.second, (unsigned long long)many.second, (unsigned long long)again.second);
}

QUENCH_TEST(cooked_round_trip_through_asset_manager) {
    const std::filesystem::path dir = std::filesystem::path(quench::test::tmpDir()) / "navigation_round_trip";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "levels");
    JobSystem jobs;
    Level level = rampAndBridge();
    level.g.links.push_back({{-2.6f, 3.0f, 5.0f}, {-1.0f, 0.0f, 5.0f}, 0.4f, false, 0, 11});
    Result<NavMeshData> data = buildNavMeshData(level.g, settings(), jobs);
    REQUIRE(data.ok(), "build: %s", data.error().c_str());
    const std::vector<std::uint8_t> bytes = writeNavMeshData(*data);
    std::ofstream(dir / "levels/yard.qnav", std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));

    Result<std::shared_ptr<const NavMeshData>> read = readNavMeshData(bytes);
    REQUIRE(read.ok(), "read: %s", read.error().c_str());
    CHECK(writeNavMeshData(**read) == bytes, "read and written again: other bytes");
    CHECK((*read)->settings.areas.size() == 2 && (*read)->settings.areas[1].name == "mud" &&
              (*read)->links.size() == 1 && (*read)->links[0].id == 11,
          "settings or links lost");

    fs::setBackend(fs::makeStdBackend(dir, {}));
    {
        assets::AssetManager manager(jobs);
        registerNavMeshAsset(manager);
        assets::AssetRef<NavMeshAsset> asset = manager.load<NavMeshAsset>("levels/yard.qnav");
        CHECK(!asset->data(), "data before the load");
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!manager.idle() && std::chrono::steady_clock::now() < end) {
            manager.update();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        REQUIRE(asset.ready() && asset->data(), "asset state %d: %s", int(asset.state()), asset->error().c_str());
        CHECK(asset->memory().cpu > bytes.size() / 2, "resident %llu bytes", (unsigned long long)asset->memory().cpu);
        Result<NavMesh> cooked = NavMesh::create(asset->data(), jobs);
        REQUIRE(cooked.ok(), "create: %s", cooked.error().c_str());
        cooked->finish();
        NavMesh direct = built(level, settings(), jobs);
        CHECK(cooked->contentHash() == direct.contentHash(), "cooked and built meshes differ");
        NavQuery query(*cooked);
        Result<Path> path = query.findPath({0.0f, 0.0f, -6.0f}, {6.0f, 3.0f, 8.0f});
        CHECK(path.ok() && !path->partial, "no path on the cooked mesh");
        asset = {};
        unregisterNavMeshAsset(manager);
    }
    fs::setBackend(nullptr);
}

QUENCH_TEST(corrupt_navmesh_is_an_error) {
    JobSystem jobs;
    Level level;
    level.box({-4.0f, -0.5f, -4.0f}, {4.0f, 0.0f, 4.0f});
    Result<NavMeshData> data = buildNavMeshData(level.g, settings(), jobs);
    REQUIRE(data.ok(), "build: %s", data.error().c_str());
    const std::vector<std::uint8_t> good = writeNavMeshData(*data);

    std::vector<std::uint8_t> flipped = good;
    flipped[flipped.size() / 2] ^= 0x5a;
    Result<std::shared_ptr<const NavMeshData>> r = readNavMeshData(flipped);
    CHECK(!r.ok() && r.error().find("checksum") != std::string::npos, "flipped byte: %s",
          r.ok() ? "read" : r.error().c_str());
    r = readNavMeshData(std::span(good).first(good.size() - 7));
    CHECK(!r.ok(), "a truncated file was read");
    r = readNavMeshData(std::span(good).first(10));
    CHECK(!r.ok(), "a header alone was read");
    std::vector<std::uint8_t> other = good;
    other[0] = 'X';
    CHECK(!readNavMeshData(other).ok(), "a wrong magic was read");
    // Valid checksum, broken layer: the layer checks catch it.
    NavMeshData broken = *data;
    REQUIRE(!broken.layers.empty(), "no layers");
    broken.layers[0].bytes.resize(broken.layers[0].bytes.size() - 4);
    r = readNavMeshData(writeNavMeshData(broken));
    CHECK(!r.ok(), "a short layer was read");
    broken = *data;
    broken.layers[0].tileX = 99;
    r = readNavMeshData(writeNavMeshData(broken));
    CHECK(!r.ok(), "a layer outside the grid was read");

    // Through the asset manager: a failed asset, an error line, no data.
    const std::filesystem::path dir = std::filesystem::path(quench::test::tmpDir()) / "navigation_corrupt";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "bad.qnav", std::ios::binary)
        .write(reinterpret_cast<const char*>(flipped.data()), std::streamsize(flipped.size()));
    fs::setBackend(fs::makeStdBackend(dir, {}));
    {
        LogCounter errors(log::Level::Error, "assets");
        assets::AssetManager manager(jobs);
        registerNavMeshAsset(manager);
        assets::AssetRef<NavMeshAsset> asset = manager.load<NavMeshAsset>("bad.qnav");
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!manager.idle() && std::chrono::steady_clock::now() < end) {
            manager.update();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        CHECK(asset.state() == assets::AssetState::Failed && !asset->data(), "state %d", int(asset.state()));
        CHECK(asset->error().find("checksum") != std::string::npos, "error: %s", asset->error().c_str());
        CHECK(errors.lines == 1 && errors.last.find("bad.qnav") != std::string::npos, "%d error lines: %s",
              errors.lines, errors.last.c_str());
        CHECK(!NavMesh::create(asset->data(), jobs).ok(), "a mesh of no data");
        asset = {};
        unregisterNavMeshAsset(manager);
    }
    fs::setBackend(nullptr);
}

QUENCH_TEST(timings) {
    // Numbers for the devlog (not checked): a level of 40 m x 40 m with two floors, paths, crowds of 25 and 100.
    JobSystem jobs;
    Level level;
    level.box({-20.0f, -0.5f, -20.0f}, {20.0f, 0.0f, 20.0f});
    level.box({-20.0f, 0.0f, 8.0f}, {-4.0f, 3.0f, 20.0f});
    level.box({4.0f, 0.0f, 8.0f}, {20.0f, 3.0f, 20.0f});
    level.quad({-18.0f, 0.0f, 0.0f}, {-18.0f, 3.0f, 8.0f}, {-14.0f, 3.0f, 8.0f}, {-14.0f, 0.0f, 0.0f});
    level.box({-4.0f, 2.8f, 12.0f}, {4.0f, 3.0f, 13.6f});
    for (int i = 0; i < 12; ++i) {
        const float x = -16.0f + 3.0f * float(i), z = -12.0f + float(i % 4) * 4.0f;
        level.box({x, 0.0f, z}, {x + 1.0f, 2.0f, z + 1.0f});
    }
    const auto t0 = std::chrono::steady_clock::now();
    NavMesh mesh = built(level, settings(48), jobs);
    const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    NavQuery query(mesh);
    std::uint64_t seed = 7;
    int paths = 0;
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) {
        Result<NavPoint> a = query.randomPoint(seed), b = query.randomPoint(seed);
        if (a.ok() && b.ok()) paths += query.findPath(a->position, b->position).ok();
    }
    const double pathUs =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count() / 200.0;
    CHECK(paths > 150, "%d of 200 paths", paths);
    std::printf("timings: build %.2f ms (%d jobs threads, %d polygons); path with two random points %.1f us\n", buildMs,
                jobs.threadCount(), mesh.stats().polygons, pathUs);
    for (const int count : {25, 100}) {
        Crowd crowd(mesh, CrowdSettings{.maxAgents = count});
        std::vector<AgentId> agents;
        for (int i = 0; i < count; ++i) {
            Result<NavPoint> p = query.randomPoint(seed);
            if (!p.ok()) continue;
            Result<AgentId> agent = crowd.addAgent(p->position);
            if (agent.ok()) agents.push_back(*agent);
        }
        double total = 0.0;
        for (int step = 0; step < 600; ++step) {
            if (step % 120 == 0)
                for (const AgentId id : agents)
                    if (Result<NavPoint> t = query.randomPoint(seed); t.ok()) crowd.setTarget(id, t->position);
            const auto s = std::chrono::steady_clock::now();
            crowd.update(1.0f / 60.0f);
            total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s).count();
        }
        std::printf("timings: crowd of %zu agents, update %.3f ms (mean of 600 steps)\n", agents.size(), total / 600.0);
    }
}
