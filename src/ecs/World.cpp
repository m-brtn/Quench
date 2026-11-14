#include <quench/ecs/World.h>

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <algorithm>
#include <chrono>

namespace quench::ecs {

namespace {

// Flecs's levels: above 0 debug tracing, 0 tracing, -2 a warning, -3 an error, -4 a fatal error.
void logMessage(std::int32_t level, const char* file, std::int32_t line, const char* message) {
    const char* text = message ? message : "";
    if (level <= -3) {
        log::error("ecs", "{} ({}:{})", text, file ? file : "?", line);
    } else if (level == -2) {
        log::warning("ecs", "{} ({}:{})", text, file ? file : "?", line);
    } else if (level == 0) {
        log::info("ecs", "{}", text);
    } else {
        log::debug("ecs", "{}", text);
    }
}

// A time step of 0 means "measure it yourself" to Flecs, which a deterministic run must never get.
constexpr float kMinStep = 1.0e-6F;

double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

World::OsApi::OsApi() {
    // Flecs takes an OS layer once per process, before its first world.
    static bool installed = false;
    if (installed) return;
    installed = true;
    ecs_os_set_api_defaults();
    ecs_os_api_t api = ecs_os_get_api();
    api.log_ = logMessage;
    // Without these Flecs reports that threading is unavailable and refuses worker threads: threads are the job
    // system's (AGENTS.md, "Threads").
    api.thread_new_ = nullptr;
    api.thread_join_ = nullptr;
    api.task_new_ = nullptr;
    api.task_join_ = nullptr;
    ecs_os_set_api(&api);
    ecs_log_enable_colors(false);
}

World::World() {
    m_world.component<FixedStep>();
    m_fixedPipeline = m_world.pipeline().with(flecs::System).with<FixedStep>().build();
    m_baseEntities = ecs_get_entities(m_world.c_ptr()).alive_count;
}

World::~World() = default;

void World::fixedStep(float dt) {
    QUENCH_ASSERT(dt > 0.0F, "ecs::World::fixedStep needs a time step above 0");
    const auto start = std::chrono::steady_clock::now();
    m_world.run_pipeline(m_fixedPipeline, std::max(dt, kMinStep));
    m_fixedStepSeconds += secondsSince(start);
    ++m_fixedSteps;
}

void World::update(float dt) {
    QUENCH_ASSERT(dt > 0.0F, "ecs::World::update needs a frame time above 0");
    const auto start = std::chrono::steady_clock::now();
    m_world.progress(std::max(dt, kMinStep));
    m_updateSeconds += secondsSince(start);
    ++m_updates;
}

Stats World::stats() const {
    Stats stats;
    stats.entities = ecs_get_entities(m_world.c_ptr()).alive_count - m_baseEntities;
    stats.tables = m_world.get_info()->table_count;
    stats.fixedSystems = m_world.count<FixedStep>();
    stats.systems = m_world.count(flecs::System);
    stats.fixedSteps = m_fixedSteps;
    stats.updates = m_updates;
    stats.fixedStepSeconds = m_fixedStepSeconds;
    stats.updateSeconds = m_updateSeconds;
    return stats;
}

} // namespace quench::ecs
