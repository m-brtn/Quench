#include "Test.h"

#include <quench/core/Jobs.h>
#include <quench/weather/Weather.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace quench;
using namespace quench::weather;

namespace {

// The mean of a field over a grid of 160 x 160 points about 1 km apart (prime spacings: no alignment with the noise).
template <class F>
float meanOver(const Weather& w, F f) {
    double sum = 0.0;
    constexpr int kSide = 160;
    for (int z = 0; z < kSide; ++z)
        for (int x = 0; x < kSide; ++x)
            sum += f(w.sample({float(x) * 997.0f - 80000.0f, float(z) * 991.0f - 80000.0f}));
    return float(sum / double(kSide * kSide));
}

float meanCoverage(const Weather& w) {
    return meanOver(w, [](const Sample& s) { return s.coverage; });
}

bool sameSample(const Sample& a, const Sample& b, float tolerance) {
    return std::abs(a.coverage - b.coverage) <= tolerance && std::abs(a.type - b.type) <= tolerance &&
           std::abs(a.precipitation - b.precipitation) <= tolerance && std::abs(a.cirrus - b.cirrus) <= tolerance;
}

Settings clearSky() {
    Settings s;
    s.coverage = 0.0f;
    s.cirrus = 0.0f;
    return s;
}

} // namespace

// The share of the sky the field covers follows Settings::coverage: none at 0, overcast at 1, within 0.06 between.
QUENCH_TEST(weather_coverage_follows_settings) {
    float previous = -1.0f;
    for (float amount : {0.0f, 0.2f, 0.35f, 0.5f, 0.65f, 0.8f, 1.0f}) {
        Settings s;
        s.coverage = amount;
        s.seed = 11;
        Weather w(s);
        w.setTime(500.0);
        const float mean = meanCoverage(w);
        std::printf("  coverage %.2f: mean %.3f\n", amount, mean);
        CHECK(mean > previous, "coverage %.2f: mean %.3f, not above %.3f", amount, mean, previous);
        if (amount == 0.0f)
            CHECK(mean == 0.0f, "clear sky with mean %.4f", mean);
        else if (amount == 1.0f)
            CHECK(mean == 1.0f, "overcast with mean %.4f", mean);
        else
            CHECK(std::abs(mean - amount) < 0.06f, "coverage %.2f: mean %.3f", amount, mean);
        previous = mean;
    }
}

// The same seed, settings, cells and time give the same weather; update() in steps arrives where setTime() jumps.
QUENCH_TEST(weather_deterministic) {
    Settings s;
    s.coverage = 0.5f;
    s.seed = 3;
    s.precipitation = 0.6f;
    s.cirrus = 0.4f;
    s.wind = {{0.6f, 0.8f}, 15.0f};
    Weather a(s);
    Weather b(s);
    const Cell cell{.position = {2000.0f, -3000.0f}, .radius = 3000.0f, .precipitation = 1.0f};
    a.addCell(cell);
    b.addCell(cell);
    for (int i = 0; i < 600; ++i) a.update(1.0 / 60.0);
    b.setTime(10.0);
    CHECK(std::abs(a.time() - 10.0) < 1e-9, "time %.9f", a.time());
    CHECK(glm::length(a.airOffset() - b.airOffset()) < 0.01f, "air offsets %.4f %.4f and %.4f %.4f", a.airOffset().x,
          a.airOffset().y, b.airOffset().x, b.airOffset().y);
    CHECK(std::abs(a.evolutionTime() - b.evolutionTime()) < 1e-9, "evolution %.9f and %.9f", a.evolutionTime(),
          b.evolutionTime());
    int differing = 0;
    for (int i = 0; i < 400; ++i) {
        const glm::vec2 p{float(i % 20) * 1300.0f - 13000.0f, float(i / 20) * 1300.0f - 13000.0f};
        if (!sameSample(a.sample(p), b.sample(p), 1e-3f)) ++differing;
    }
    CHECK(differing == 0, "%d of 400 samples differ between steps and a jump", differing);

    // Exactly the same for the same calls.
    Weather c(s);
    c.addCell(cell);
    c.setTime(10.0);
    for (int i = 0; i < 400; ++i) {
        const glm::vec2 p{float(i % 20) * 1300.0f - 13000.0f, float(i / 20) * 1300.0f - 13000.0f};
        const Sample x = b.sample(p);
        const Sample y = c.sample(p);
        REQUIRE(x.coverage == y.coverage && x.type == y.type && x.precipitation == y.precipitation &&
                    x.cirrus == y.cirrus,
                "sample %d differs", i);
    }
}

// Without evolution the field only moves: what was at p is at p + wind * dt later.
QUENCH_TEST(weather_drifts_with_wind) {
    Settings s;
    s.coverage = 0.5f;
    s.evolution = 0.0f;
    s.wind = {{1.0f, 1.0f}, 20.0f};
    Weather w(s);
    w.setTime(100.0);
    std::vector<Sample> before;
    for (int i = 0; i < 100; ++i) before.push_back(w.sample({float(i) * 431.0f, float(i % 10) * 677.0f}));
    w.setTime(160.0);
    const glm::vec2 moved = glm::normalize(glm::vec2(1.0f, 1.0f)) * (20.0f * 60.0f);
    int differing = 0;
    for (int i = 0; i < 100; ++i)
        if (!sameSample(before[std::size_t(i)], w.sample(glm::vec2(float(i) * 431.0f, float(i % 10) * 677.0f) + moved),
                        1e-3f))
            ++differing;
    CHECK(differing == 0, "%d of 100 samples did not move with the wind", differing);
}

// The field changes shape over Weather::kEvolutionPeriod at evolution 1, and hardly within a second.
QUENCH_TEST(weather_evolves) {
    Settings s;
    s.coverage = 0.5f;
    s.wind.speed = 0.0f;
    Weather w(s);
    const auto field = [&] {
        std::vector<float> v;
        for (int i = 0; i < 400; ++i)
            v.push_back(w.sample({float(i % 20) * 1700.0f, float(i / 20) * 1700.0f}).coverage);
        return v;
    };
    const auto meanDifference = [](const std::vector<float>& a, const std::vector<float>& b) {
        double d = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) d += std::abs(a[i] - b[i]);
        return float(d / double(a.size()));
    };
    w.setTime(1000.0);
    const std::vector<float> start = field();
    w.setTime(1001.0);
    const float second = meanDifference(start, field());
    w.setTime(1000.0 + Weather::kEvolutionPeriod);
    const float period = meanDifference(start, field());
    std::printf("  mean coverage change: %.4f after 1 s, %.4f after %.0f s\n", second, period,
                Weather::kEvolutionPeriod);
    CHECK(second < 0.01f, "changed by %.4f in a second", second);
    CHECK(period > 0.1f, "changed by only %.4f in a period", period);

    // Faster with a higher evolution, frozen at 0.
    s.evolution = 0.0f;
    w.setSettings(s);
    w.setTime(1000.0);
    const std::vector<float> frozen = field();
    w.setTime(5000.0);
    CHECK(meanDifference(frozen, field()) == 0.0f, "a frozen field changed");
}

QUENCH_TEST(weather_seed_changes_field) {
    Settings s;
    s.coverage = 0.5f;
    Weather a(s);
    s.seed = 2;
    Weather b(s);
    int differing = 0;
    for (int i = 0; i < 200; ++i) {
        const glm::vec2 p{float(i % 20) * 1900.0f, float(i / 20) * 1900.0f};
        if (std::abs(a.sample(p).coverage - b.sample(p).coverage) > 0.2f) ++differing;
    }
    CHECK(differing > 40, "only %d of 200 samples differ between seeds", differing);
}

// The field's type follows the setting; it rains only where the settings ask for it.
QUENCH_TEST(weather_type_and_precipitation) {
    for (CloudType type : {CloudType::Stratus, CloudType::Cumulus, CloudType::Towering, CloudType::Cumulonimbus}) {
        Settings s;
        s.coverage = 0.6f;
        s.type = type;
        s.typeVariation = 0.1f;
        Weather w(s);
        const float mean = meanOver(w, [](const Sample& x) { return x.type; });
        CHECK(std::abs(mean - typeValue(type)) < 0.08f, "type %d: mean %.3f", int(type), mean);
        CHECK(nearestType(typeValue(type)) == type, "nearestType %d", int(type));
        const float rain = meanOver(w, [](const Sample& x) { return x.precipitation; });
        CHECK(rain == 0.0f, "type %d rains %.4f without precipitation", int(type), rain);
    }
    Settings s;
    s.coverage = 0.8f;
    s.precipitation = 1.0f;
    s.type = CloudType::Cumulonimbus;
    Weather w(s);
    const float raining = meanOver(w, [](const Sample& x) { return x.precipitation > 0.5f ? 1.0f : 0.0f; });
    const float wet =
        meanOver(w, [](const Sample& x) { return x.precipitation > 0.0f && x.coverage < 0.5f ? 1.0f : 0.0f; });
    std::printf("  precipitation over 0.5 on %.3f of the map, under thin cloud on %.4f\n", raining, wet);
    CHECK(raining > 0.2f && raining < 0.9f, "rain over %.3f of the map", raining);
    CHECK(wet == 0.0f, "rain from thin cloud on %.4f of the map", wet);
}

// One raining cumulonimbus in a clear sky: the cloud, its rain under the core, blue sky around it.
QUENCH_TEST(weather_cell_in_clear_sky) {
    Weather w(clearSky());
    const CellId id = w.addCell({.position = {5000.0f, 0.0f},
                                 .radius = 3000.0f,
                                 .type = CloudType::Cumulonimbus,
                                 .coverage = 1.0f,
                                 .precipitation = 1.0f});
    REQUIRE(id != kNoCell, "no cell");
    const Sample centre = w.sample({5000.0f, 0.0f});
    CHECK(centre.coverage == 1.0f && centre.type > 0.99f && centre.precipitation == 1.0f,
          "centre: coverage %.3f type %.3f precipitation %.3f", centre.coverage, centre.type, centre.precipitation);
    for (float a = 0.0f; a < 6.28f; a += 0.5f) {
        const Sample out = w.sample(glm::vec2(5000.0f, 0.0f) + glm::vec2(std::cos(a), std::sin(a)) * 3700.0f);
        CHECK(out.coverage == 0.0f && out.precipitation == 0.0f, "at 1.23 radii (angle %.1f): coverage %.3f", a,
              out.coverage);
        const Sample edge = w.sample(glm::vec2(5000.0f, 0.0f) + glm::vec2(std::cos(a), std::sin(a)) * 2100.0f);
        CHECK(edge.precipitation < 0.05f, "rain at 0.7 radii (angle %.1f): %.3f", a, edge.precipitation);
    }
    CHECK(w.precipitation({5000.0f, 0.0f, 0.0f}) == 1.0f, "no rain on the ground under the core");
    CHECK(w.precipitation({5000.0f, 1600.0f, 0.0f}) == 0.0f, "rain above the cloud base");
    CHECK(w.cover({5000.0f, 0.0f}, 1000.0f) > 0.95f, "cover under the cell %.3f", w.cover({5000.0f, 0.0f}, 1000.0f));
    CHECK(w.cover({-20000.0f, 0.0f}, 5000.0f) == 0.0f, "cover in the clear sky");

    // Distance to the rain: 0 inside, about 3.2 to 3.8 km from the origin (rain of at least 0.1 reaches about 0.47
    // radii from the centre, 0.39 to 0.59 with the lobes), the maximum where nothing rains.
    CHECK(w.distanceToPrecipitation({5000.0f, 0.0f}) == 0.0f, "distance inside the rain");
    const auto start = std::chrono::steady_clock::now();
    const float d = w.distanceToPrecipitation({0.0f, 0.0f}, 0.1f, 20000.0f);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("  distance to the rain %.0f m (%.3f ms)\n", d, ms);
    CHECK(d > 3200.0f && d < 4800.0f, "distance %.0f m", d);
    CHECK(std::abs(w.distanceToPrecipitation({-30000.0f, 0.0f}, 0.1f, 5000.0f) - 5000.0f) < 1e-3f,
          "no rain within 5 km");
    Weather dry(clearSky());
    CHECK(dry.distanceToPrecipitation({0.0f, 0.0f}, 0.1f, 9000.0f) == 9000.0f, "a dry sky");
}

// Drifting cells move with the air, anchored ones stay; at most kMaxCells.
QUENCH_TEST(weather_cells_drift_and_anchor) {
    Settings s = clearSky();
    s.wind = {{0.0f, -1.0f}, 10.0f};
    Weather w(s);
    const CellId drifting = w.addCell({.position = {0.0f, 0.0f}, .radius = 2000.0f});
    const CellId anchored = w.addCell({.position = {9000.0f, 0.0f}, .radius = 2000.0f, .drifts = false});
    w.update(30.0);
    const std::optional<Cell> a = w.cell(drifting);
    const std::optional<Cell> b = w.cell(anchored);
    REQUIRE(a && b, "cells lost");
    CHECK(glm::length(a->position - glm::vec2(0.0f, -300.0f)) < 1e-3f, "drifted to %.2f %.2f", a->position.x,
          a->position.y);
    CHECK(glm::length(b->position - glm::vec2(9000.0f, 0.0f)) < 1e-3f, "anchored at %.2f %.2f", b->position.x,
          b->position.y);
    CHECK(w.sample({0.0f, -300.0f}).coverage == 1.0f, "the drifted cell is not where it went");

    // Set again where it is now: it stays there.
    Cell moved = *a;
    moved.radius = 1000.0f;
    CHECK(w.setCell(drifting, moved), "setCell");
    CHECK(glm::length(w.cell(drifting)->position - a->position) < 1e-3f, "setCell moved the cell");
    CHECK(w.removeCell(anchored) && !w.cell(anchored) && w.cellCount() == 1, "removeCell");
    CHECK(!w.removeCell(anchored) && !w.setCell(anchored, moved), "a removed cell");
    for (std::size_t i = w.cellCount(); i < Weather::kMaxCells; ++i)
        CHECK(w.addCell({.position = {float(i) * 1000.0f, 0.0f}}) != kNoCell, "cell %zu", i);
    CHECK(w.addCell({}) == kNoCell, "more than kMaxCells cells");
    w.clearCells();
    CHECK(w.cellCount() == 0 && w.sample({0.0f, -300.0f}).coverage == 0.0f, "clearCells");
}

QUENCH_TEST(weather_wind_and_heights) {
    Settings s;
    s.wind = {{3.0f, 4.0f}, 12.0f};
    Weather w(s);
    const glm::vec3 low = w.windAt({0.0f, 2.0f, 0.0f});
    const glm::vec3 mid = w.windAt({0.0f, 100.0f, 0.0f});
    const glm::vec3 high = w.windAt({0.0f, s.cloudBase, 0.0f});
    CHECK(glm::length(low) < glm::length(mid) && glm::length(mid) < glm::length(high), "no profile: %.2f %.2f %.2f",
          glm::length(low), glm::length(mid), glm::length(high));
    CHECK(std::abs(glm::length(high) - 12.0f) < 1e-3f, "%.3f m/s at the cloud base", glm::length(high));
    CHECK(std::abs(high.x / high.z - 0.75f) < 1e-4f && high.y == 0.0f, "direction");
    CHECK(glm::length(low) > 0.3f * 12.0f && glm::length(low) < 0.5f * 12.0f, "%.2f m/s at 2 m", glm::length(low));

    float previous = 0.0f;
    for (float type : {0.0f, 0.2f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 0.9f, 1.0f}) {
        const float top = w.cloudTop(type);
        CHECK(top > previous && top <= s.cloudTop, "type %.2f: top %.0f", type, top);
        previous = top;
    }
    CHECK(std::abs(w.cloudTop(0.0f) - (s.cloudBase + 500.0f)) < 1e-2f, "stratus top %.1f", w.cloudTop(0.0f));
    CHECK(std::abs(w.cloudTop(1.0f) - s.cloudTop) < 1e-2f, "cumulonimbus top %.1f", w.cloudTop(1.0f));
}

QUENCH_TEST(weather_fill_matches_sample) {
    Settings s;
    s.coverage = 0.45f;
    s.cirrus = 0.5f;
    Weather w(s);
    w.addCell({.position = {1000.0f, 2000.0f}, .radius = 1500.0f, .precipitation = 0.8f});
    std::vector<Sample> grid(32 * 32);
    w.fill({-8000.0f, -8000.0f}, 16000.0f, 32, grid);
    int differing = 0;
    for (std::uint32_t z = 0; z < 32; ++z)
        for (std::uint32_t x = 0; x < 32; ++x) {
            const glm::vec2 p{-8000.0f + (float(x) + 0.5f) * 500.0f, -8000.0f + (float(z) + 0.5f) * 500.0f};
            if (!sameSample(grid[z * 32 + x], w.sample(p), 0.0f)) ++differing;
        }
    CHECK(differing == 0, "%d of 1024 texels differ from sample()", differing);
}

// Const queries from several threads at once give what one thread gets (ThreadSanitizer checks the races).
QUENCH_TEST(weather_queries_in_parallel) {
    Settings s;
    s.coverage = 0.6f;
    s.precipitation = 0.7f;
    Weather w(s);
    w.addCell({.position = {0.0f, 0.0f}, .precipitation = 1.0f});
    w.setTime(77.0);
    constexpr std::uint32_t kCount = 4096;
    std::vector<float> serial(kCount);
    std::vector<float> parallel(kCount);
    const auto at = [](std::uint32_t i) { return glm::vec2(float(i % 64) * 512.0f, float(i / 64) * 512.0f); };
    for (std::uint32_t i = 0; i < kCount; ++i) serial[i] = w.sample(at(i)).precipitation + w.cover(at(i), 800.0f);
    JobSystem jobs;
    std::atomic<int> ranges{0};
    jobs.wait(jobs.parallelFor(kCount, 64, [&](std::uint32_t begin, std::uint32_t end) {
        for (std::uint32_t i = begin; i < end; ++i)
            parallel[i] = w.sample(at(i)).precipitation + w.cover(at(i), 800.0f);
        ranges.fetch_add(1, std::memory_order_relaxed);
    }));
    CHECK(ranges.load() > 0, "no ranges ran");
    CHECK(serial == parallel, "parallel queries differ");
}

// What a query costs (printed; the bound only catches a pathological slowdown).
QUENCH_TEST(weather_sample_cost) {
    Settings s;
    s.coverage = 0.5f;
    s.cirrus = 0.3f;
    Weather w(s);
    for (std::size_t i = 0; i < Weather::kMaxCells; ++i)
        w.addCell({.position = {float(i) * 3000.0f, 0.0f}, .precipitation = 0.5f});
    const auto start = std::chrono::steady_clock::now();
    float sum = 0.0f;
    constexpr int kCount = 20000;
    for (int i = 0; i < kCount; ++i) sum += w.sample({float(i) * 13.0f, float(i) * 7.0f}).coverage;
    const double ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kCount;
    std::printf("  sample with %zu cells: %.0f ns (sum %.1f)\n", Weather::kMaxCells, ns, double(sum));
    CHECK(ns < 200000.0, "a sample takes %.0f ns", ns);
}
