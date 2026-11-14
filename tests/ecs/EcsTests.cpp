#include "Test.h"

#include <quench/core/Log.h>
#include <quench/ecs/World.h>

#include <string>
#include <vector>

using namespace quench;

namespace {

struct Position {
    float x = 0.0F;
};
struct Velocity {
    float x = 0.0F;
};
struct Doomed {};

} // namespace

QUENCH_TEST(fixed_step_and_update_run_their_own_systems) {
    ecs::World world;
    int fixedRuns = 0;
    int updateRuns = 0;
    float fixedDt = 0.0F;
    float updateDt = 0.0F;
    world.flecs().system("Fixed").kind<ecs::FixedStep>().run([&](flecs::iter& it) {
        ++fixedRuns;
        fixedDt = it.delta_time();
    });
    world.flecs().system("Frame").run([&](flecs::iter& it) {
        ++updateRuns;
        updateDt = it.delta_time();
    });

    for (int i = 0; i < 3; ++i) world.fixedStep(1.0F / 120.0F);
    CHECK(fixedRuns == 3, "fixed system ran %d times", fixedRuns);
    CHECK(updateRuns == 0, "frame system ran %d times in fixed steps", updateRuns);
    CHECK(fixedDt > 0.0083F && fixedDt < 0.0084F, "fixed dt %f", double(fixedDt));

    for (int i = 0; i < 2; ++i) world.update(0.016F);
    CHECK(fixedRuns == 3, "fixed system ran %d times after updates", fixedRuns);
    CHECK(updateRuns == 2, "frame system ran %d times", updateRuns);
    CHECK(updateDt > 0.0159F && updateDt < 0.0161F, "frame dt %f", double(updateDt));

    const ecs::Stats stats = world.stats();
    CHECK(stats.fixedSteps == 3 && stats.updates == 2, "steps %llu updates %llu",
          static_cast<unsigned long long>(stats.fixedSteps), static_cast<unsigned long long>(stats.updates));
    CHECK(stats.fixedSystems == 1, "fixed systems %d", stats.fixedSystems);
    CHECK(stats.systems >= 2, "systems %d", stats.systems);
}

QUENCH_TEST(fixed_systems_run_in_the_order_they_were_made) {
    ecs::World world;
    std::string order;
    for (const char* name : {"a", "b", "c", "d"}) {
        world.flecs().system(name).kind<ecs::FixedStep>().run([&order, name](flecs::iter&) { order += name; });
    }
    world.fixedStep(0.01F);
    world.fixedStep(0.01F);
    CHECK(order == "abcdabcd", "order %s", order.c_str());
}

QUENCH_TEST(systems_move_and_delete_entities) {
    ecs::World world;
    flecs::world& ecs = world.flecs();
    ecs.component<Position>();
    ecs.component<Velocity>();
    ecs.component<Doomed>();
    ecs.system<Position, const Velocity>("Move").kind<ecs::FixedStep>().each(
        [](flecs::iter& it, size_t, Position& p, const Velocity& v) { p.x += v.x * it.delta_time(); });
    // Deleting inside a system is deferred to the end of the step.
    ecs.system<const Position>("Reap").kind<ecs::FixedStep>().each([](flecs::entity e, const Position& p) {
        if (p.x > 1.0F) e.add<Doomed>();
    });
    ecs.system("Bury").kind<ecs::FixedStep>().with<Doomed>().each([](flecs::entity e) { e.destruct(); });

    const int before = world.stats().entities;
    std::vector<flecs::entity> entities;
    for (int i = 0; i < 100; ++i) {
        entities.push_back(ecs.entity().set<Position>({0.0F}).set<Velocity>({i < 40 ? 4.0F : 1.0F}));
    }
    CHECK(world.stats().entities - before == 100, "made %d", world.stats().entities - before);

    // 0.5 s in steps of 0.1 s: the fast ones pass x = 1 after the third step, are marked in it and gone a step later.
    for (int i = 0; i < 5; ++i) world.fixedStep(0.1F);
    CHECK(world.stats().entities - before == 60, "left %d", world.stats().entities - before);
    CHECK(!entities[0].is_alive(), "a fast entity is still alive");
    REQUIRE(entities[99].is_alive(), "a slow entity is gone");
    const float x = entities[99].get<Position>().x;
    CHECK(x > 0.49F && x < 0.51F, "x %f", double(x));
}

QUENCH_TEST(flecs_has_no_threads_and_logs_to_the_engine) {
    ecs::World world;
    CHECK(!ecs_os_has_threading(), "Flecs could start threads");

    std::vector<std::string> messages;
    const int sink = log::addSink([&](log::Level level, std::string_view category, std::string_view message) {
        if (category == "ecs" && level == log::Level::Warning) messages.emplace_back(message);
    });
    log::setStderr(false);
    ecs_warn("a warning from Flecs %d", 7);
    log::setStderr(true);
    log::removeSink(sink);
    REQUIRE(messages.size() == 1, "%zu messages", messages.size());
    CHECK(messages[0].find("a warning from Flecs 7") != std::string::npos, "message %s", messages[0].c_str());
}

QUENCH_TEST(two_worlds_are_independent) {
    ecs::World first;
    ecs::World second;
    first.flecs().component<Position>();
    second.flecs().component<Position>();
    for (int i = 0; i < 10; ++i) first.flecs().entity().set<Position>({1.0F});
    for (int i = 0; i < 3; ++i) second.flecs().entity().set<Position>({2.0F});
    CHECK(first.flecs().count<Position>() == 10, "first %d", first.flecs().count<Position>());
    CHECK(second.flecs().count<Position>() == 3, "second %d", second.flecs().count<Position>());
}
