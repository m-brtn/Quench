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
    const glm::vec3 rain = w.rainVelocity({0.0f, 2.0f, 0.0f});
    CHECK(rain.y == -Weather::kRainFallSpeed && rain.x == low.x && rain.z == low.z, "rain (%.2f %.2f %.2f) at 2 m",
          rain.x, rain.y, rain.z);

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

// Ground under rain wets by Settings::wettingTime and puddleFillTime and dries by dryingTime and puddleDryTime once
// the rain stops; ground the rain never reached stays dry.
QUENCH_TEST(weather_wetness_builds_and_dries) {
    Settings s = clearSky();
    s.wind.speed = 0.0f;
    Weather w(s);
    CHECK(w.wetness({0.0f, 0.0f}).ground == 0.0f && w.wetness({0.0f, 0.0f}).water == 0.0f, "wet under a clear sky");
    const CellId shower = w.addCell({.position = {0.0f, 0.0f},
                                     .radius = 3000.0f,
                                     .type = CloudType::Cumulonimbus,
                                     .precipitation = 1.0f,
                                     .drifts = false});
    const auto run = [&](double seconds) {
        for (int i = 0; i < int(std::lround(seconds * 60.0)); ++i) w.update(1.0 / 60.0);
    };
    run(60.0);
    Wetness at = w.wetness({10.0f, -20.0f});
    std::printf("  after 60 s of rain: ground %.3f, water %.3f\n", at.ground, at.water);
    CHECK(std::abs(at.ground - 0.5f) < 0.01f, "ground %.3f after half the wetting time", at.ground);
    CHECK(std::abs(at.water - 60.0f / 900.0f) < 0.005f, "water %.3f", at.water);
    CHECK(w.wetnessGrid()[std::size_t(8 * Weather::kWetnessGrid + 8)].precipitation == 1.0f, "no rain in the grid");
    run(90.0);
    at = w.wetness({10.0f, -20.0f});
    CHECK(at.ground == 1.0f, "ground %.3f after the wetting time", at.ground);
    CHECK(w.wetness({0.0f, 9000.0f}).ground == 0.0f, "wet where it never rained");
    CHECK(w.removeCell(shower), "removeCell");
    run(450.0);
    at = w.wetness({10.0f, -20.0f});
    std::printf("  450 s after the rain: ground %.3f, water %.3f\n", at.ground, at.water);
    CHECK(std::abs(at.ground - 0.5f) < 0.01f, "ground %.3f after half the drying time", at.ground);
    CHECK(at.water == 0.0f, "water %.3f after 450 s of drying", at.water);
    run(500.0);
    CHECK(w.wetness({10.0f, -20.0f}).ground == 0.0f, "still wet after the drying time");
}

// A shower drifting over the focus: dry before it comes, soaked under it, drying after it; setTime() estimates the
// same wetness from the weather's past, and the same calls give the same grid.
QUENCH_TEST(weather_wetness_follows_a_passing_shower) {
    Settings s = clearSky();
    s.wind = {{1.0f, 0.0f}, 10.0f};
    s.evolution = 0.0f;
    const Cell cell{.position = {-6000.0f, 0.0f}, .radius = 3000.0f, .precipitation = 1.0f};
    Weather a(s);
    a.addCell(cell);
    Weather b = a;
    float before = -1.0f;
    for (int i = 1; i <= 1200 * 30; ++i) {
        a.update(1.0 / 30.0);
        b.update(1.0 / 30.0);
        if (i == 400 * 30) before = a.wetness({0.0f, 0.0f}).ground;
    }
    const Wetness integrated = a.wetness({0.0f, 0.0f});
    Weather c(s);
    c.addCell(cell);
    c.setTime(1200.0);
    const Wetness estimated = c.wetness({0.0f, 0.0f});
    std::printf("  at 400 s: ground %.3f; at 1200 s: integrated ground %.3f water %.3f, estimated %.3f %.3f\n", before,
                integrated.ground, integrated.water, estimated.ground, estimated.water);
    CHECK(before == 0.0f, "wet at 400 s, before the shower: %.3f", before);
    CHECK(integrated.ground > 0.3f && integrated.ground < 0.7f, "ground %.3f, 460 s after the shower",
          integrated.ground);
    CHECK(std::abs(integrated.ground - estimated.ground) < 0.12f, "estimate %.3f against %.3f", estimated.ground,
          integrated.ground);
    CHECK(std::abs(integrated.water - estimated.water) < 0.05f, "water estimate %.3f against %.3f", estimated.water,
          integrated.water);
    // The versions differ (unique to each Weather); the grids do not.
    bool same = a.wetnessVersion() != b.wetnessVersion();
    for (std::size_t i = 0; i < a.wetnessGrid().size(); ++i)
        same = same && a.wetnessGrid()[i].wetness.ground == b.wetnessGrid()[i].wetness.ground &&
               a.wetnessGrid()[i].wetness.water == b.wetnessGrid()[i].wetness.water;
    CHECK(same, "the same calls gave another grid");
}

// The grid follows its focus in whole cells: wetness it holds stays where it fell, cells it brings in are estimated.
QUENCH_TEST(weather_wetness_focus_moves) {
    Settings s = clearSky();
    s.wind.speed = 0.0f;
    Weather w(s);
    const CellId shower =
        w.addCell({.position = {0.0f, 0.0f}, .radius = 3000.0f, .precipitation = 1.0f, .drifts = false});
    for (int i = 0; i < 60 * 30; ++i) w.update(1.0 / 30.0);
    w.removeCell(shower);
    const float here = w.wetness({100.0f, 50.0f}).ground;
    const glm::vec2 origin = w.wetnessOrigin();
    w.setWetnessFocus({200.0f, -130.0f});
    CHECK(w.wetnessOrigin() != origin, "the grid did not move");
    CHECK(w.wetnessFocus() == glm::vec2(200.0f, -130.0f), "focus");
    const float moved = w.wetness({100.0f, 50.0f}).ground;
    CHECK(std::abs(moved - here) < 1e-6f, "wetness %.4f became %.4f when the grid moved", here, moved);
    // Far away the grid starts from the estimate: dry, since no rain fell there (and the shower is gone).
    w.setWetnessFocus({20000.0f, 0.0f});
    CHECK(w.wetness({20000.0f, 0.0f}).ground == 0.0f, "wet far from the rain");
    CHECK(w.wetness({100.0f, 50.0f}).ground == 0.0f, "outside the grid the estimate kept the removed shower");
}

// Rain or snow by the temperature, colder with the height, and a cell's own share whatever the temperature.
QUENCH_TEST(weather_snow_by_temperature) {
    Settings s = clearSky();
    s.wind.speed = 0.0f;
    Weather w(s);
    CHECK(w.snowShare({0.0f, s.cloudBase, 0.0f}) == 0.0f, "snow below the default cloud base: %.3f",
          w.snowShare({0.0f, s.cloudBase, 0.0f}));
    s.temperature = 10.0f;
    w.setSettings(s);
    CHECK(w.snowShare({0.0f, 0.0f, 0.0f}) == 0.0f, "snow at 10 degrees: %.3f", w.snowShare({0.0f, 0.0f, 0.0f}));
    CHECK(std::abs(w.temperature({0.0f, 1000.0f, 0.0f}) - 3.5f) < 1e-4f, "temperature at 1000 m: %.3f",
          w.temperature({0.0f, 1000.0f, 0.0f}));
    const float aloft = w.snowShare({0.0f, 1500.0f, 0.0f});
    CHECK(std::abs(aloft - 0.9f) < 1e-3f, "snow share %.3f at the cloud base (0.25 degrees)", aloft);
    s.temperature = 1.25f;
    w.setSettings(s);
    CHECK(std::abs(w.snowShare({0.0f, 0.0f, 0.0f}) - 0.5f) < 1e-4f, "sleet at 1.25 degrees: %.3f",
          w.snowShare({0.0f, 0.0f, 0.0f}));
    s.temperature = -2.0f;
    w.setSettings(s);
    CHECK(w.snowShare({0.0f, 0.0f, 0.0f}) == 1.0f, "rain at -2 degrees");
    s.temperature = 10.0f;
    w.setSettings(s);
    const CellId flurry =
        w.addCell({.position = {0.0f, 0.0f}, .radius = 2000.0f, .precipitation = 1.0f, .drifts = false, .snow = 1.0f});
    CHECK(w.snowShare({100.0f, 0.0f, 0.0f}) == 1.0f, "the cell's own snow: %.3f", w.snowShare({100.0f, 0.0f, 0.0f}));
    CHECK(w.snowShare({5000.0f, 0.0f, 0.0f}) == 0.0f, "snow outside the cell");
    const float edge = w.snowShare({1400.0f, 0.0f, 0.0f});
    CHECK(edge > 0.0f && edge < 1.0f, "no mix at the cell's edge: %.3f", edge);
    CHECK(w.precipitation({100.0f, 0.0f, 0.0f}) == 1.0f, "precipitation %.3f", w.precipitation({100.0f, 0.0f, 0.0f}));
    w.removeCell(flurry);
    CHECK(w.snowShare({100.0f, 0.0f, 0.0f}) == 0.0f, "snow after the cell went");

    // The wetness grid's precipitation and snow share follow the settings and the cells at the next update, also
    // with the clock stopped.
    const auto gridCell = [&w] {
        const glm::vec2 c = (glm::vec2(0.0f) - w.wetnessOrigin()) / Weather::kWetnessCell;
        return w.wetnessGrid()[std::size_t(int(c.y)) * Weather::kWetnessGrid + std::size_t(int(c.x))];
    };
    const CellId shower =
        w.addCell({.position = {0.0f, 0.0f}, .radius = 2000.0f, .precipitation = 1.0f, .drifts = false});
    w.update(0.0);
    CHECK(gridCell().precipitation > 0.9f && gridCell().snowShare == 0.0f, "grid after a cell: %.3f, snow %.3f",
          gridCell().precipitation, gridCell().snowShare);
    s.temperature = -5.0f;
    w.setSettings(s);
    const std::uint64_t version = w.wetnessVersion();
    w.update(0.0);
    CHECK(gridCell().snowShare == 1.0f && w.wetnessVersion() != version, "grid's snow share %.3f below freezing",
          gridCell().snowShare);
    CHECK(gridCell().wetness.water == 0.0f && gridCell().wetness.snow == 0.0f, "no time passed, nothing fell");
    w.removeCell(shower);
    w.update(0.0);
    CHECK(gridCell().precipitation == 0.0f, "grid after the cell went: %.3f", gridCell().precipitation);
}

// Snow builds a cover without wetting the ground, lies while it is cold, melts as fast as it is warm and wets the
// ground with its meltwater; setTime() estimates the cover of a long snowfall.
QUENCH_TEST(weather_snow_cover_builds_and_melts) {
    Settings s = clearSky();
    s.wind.speed = 0.0f;
    s.temperature = -3.0f;
    Weather w(s);
    const CellId snowfall = w.addCell({.position = {0.0f, 0.0f},
                                       .radius = 3000.0f,
                                       .type = CloudType::Stratus,
                                       .precipitation = 1.0f,
                                       .drifts = false});
    const auto run = [&](double seconds) {
        for (int i = 0; i < int(std::lround(seconds * 60.0)); ++i) w.update(1.0 / 60.0);
    };
    run(300.0);
    Wetness at = w.wetness({10.0f, -20.0f});
    std::printf("  after 300 s of snow: snow %.3f, ground %.3f, water %.3f\n", at.snow, at.ground, at.water);
    CHECK(std::abs(at.snow - 0.5f) < 0.01f, "snow %.3f after half the snow time", at.snow);
    CHECK(at.ground == 0.0f && at.water == 0.0f, "snow wet the ground: %.3f %.3f", at.ground, at.water);
    CHECK(w.wetnessGrid()[std::size_t(8 * Weather::kWetnessGrid + 8)].snowShare == 1.0f, "no snow in the grid");
    w.removeCell(snowfall);
    run(600.0);
    at = w.wetness({10.0f, -20.0f});
    CHECK(std::abs(at.snow - 0.5f) < 0.01f, "snow %.3f melted below freezing", at.snow);
    s.temperature = 5.0f;
    w.setSettings(s);
    run(225.0);
    at = w.wetness({10.0f, -20.0f});
    std::printf("  225 s at 5 degrees: snow %.3f, ground %.3f, water %.3f\n", at.snow, at.ground, at.water);
    CHECK(std::abs(at.snow - 0.25f) < 0.01f, "snow %.3f after a quarter of the melt time", at.snow);
    CHECK(std::abs(at.ground - 0.5f) < 0.02f, "ground %.3f wet by the meltwater", at.ground);
    CHECK(std::abs(at.water - 0.125f) < 0.01f, "water %.3f from the meltwater", at.water);
    run(300.0);
    at = w.wetness({10.0f, -20.0f});
    CHECK(at.snow == 0.0f, "snow %.3f after the melt time", at.snow);
    CHECK(at.ground > 0.8f, "ground %.3f after the melt", at.ground);
    s.temperature = -3.0f;
    Weather e(s);
    e.addCell({.position = {0.0f, 0.0f}, .radius = 3000.0f, .precipitation = 0.5f, .drifts = false});
    e.setTime(4000.0);
    at = e.wetness({10.0f, -20.0f});
    CHECK(at.snow > 0.999f && at.ground == 0.0f, "estimated snow %.3f, ground %.3f after a long snowfall", at.snow,
          at.ground);
}

// Rain and snow side by side: each layer is found on its own, and sleet counts as both by its share.
QUENCH_TEST(weather_rain_or_snow_search) {
    Settings s = clearSky();
    s.wind.speed = 0.0f;
    Weather w(s);
    w.addCell({.position = {3000.0f, 0.0f}, .radius = 1000.0f, .precipitation = 1.0f, .drifts = false, .snow = 1.0f});
    w.addCell({.position = {-6000.0f, 0.0f}, .radius = 1000.0f, .precipitation = 1.0f, .drifts = false});
    const float any = w.distanceToPrecipitation({0.0f, 0.0f});
    const float snow = w.distanceToPrecipitation({0.0f, 0.0f}, 0.1f, 20000.0f, Precipitation::Snow);
    const float rain = w.distanceToPrecipitation({0.0f, 0.0f}, 0.1f, 20000.0f, Precipitation::Rain);
    std::printf("  any %.0f m, snow %.0f m, rain %.0f m\n", any, snow, rain);
    CHECK(any <= snow && any > snow - 200.0f, "the nearest precipitation %.0f m is not the snow %.0f m", any, snow);
    CHECK(snow > 1500.0f && snow < 3000.0f, "snow at %.0f m", snow);
    CHECK(rain > 4500.0f && rain < 6000.0f, "rain at %.0f m", rain);
    CHECK(w.distanceToPrecipitation({3000.0f, 0.0f}, 0.1f, 20000.0f, Precipitation::Snow) == 0.0f, "no snow inside");
    CHECK(w.distanceToPrecipitation({3000.0f, 0.0f}, 0.1f, 20000.0f, Precipitation::Rain) > 4000.0f, "rain in snow");
}

// Gusts and eddies: the speed along the wind at 10 m deviates by about 0.45 of the mean at gusts 1 (in time at a point,
// and from place to place at a time), each direction along and across by about 0.3 and up by about 0.12 at
// turbulence 1; without either the wind is its mean, unchanged.
QUENCH_TEST(weather_wind_gusts) {
    const auto deviations = [](float gusts, float turbulence) {
        Settings s = clearSky();
        s.wind = {.direction = {0.0f, 1.0f}, .speed = 12.0f, .gusts = gusts, .turbulence = turbulence};
        Weather w(s);
        w.setTime(100.0);
        const glm::vec3 at{30.0f, 10.0f, -40.0f};
        const float mean = glm::length(w.meanWindAt(at));
        double sum[3] = {}, squares[3] = {};
        constexpr int kSteps = 6000;
        for (int i = 0; i < kSteps; ++i) {
            w.update(0.1);
            const glm::vec3 v = w.windAt(at);
            const double c[3] = {v.z, v.x, v.y}; // along, across, up
            for (int k = 0; k < 3; ++k) {
                sum[k] += c[k];
                squares[k] += c[k] * c[k];
            }
        }
        glm::vec4 out(0.0f);
        for (int k = 0; k < 3; ++k) {
            const double m = sum[k] / kSteps;
            out[k] = float(std::sqrt(std::max(squares[k] / kSteps - m * m, 0.0)) / mean);
        }
        out.w = float(sum[0] / kSteps / mean);
        // Along a line 2 km down the wind at one time.
        double s1 = 0.0, s2 = 0.0;
        for (int i = 0; i < 2000; ++i) {
            const float along = w.windAt(at + glm::vec3(0.0f, 0.0f, float(i) * 1.0f)).z;
            s1 += along;
            s2 += double(along) * along;
        }
        const double m = s1 / 2000.0;
        std::printf("  gusts %.1f, turbulence %.1f: %.2f m/s mean; deviation along %.3f, across %.3f, up %.3f of it "
                    "(in time), along %.3f (in space), mean %.3f of it\n",
                    gusts, turbulence, mean, out.x, out.y, out.z, std::sqrt(std::max(s2 / 2000.0 - m * m, 0.0)) / mean,
                    out.w);
        return out;
    };
    const glm::vec4 gusty = deviations(1.0f, 0.0f);
    CHECK(gusty.x > 0.25f && gusty.x < 0.55f && gusty.y < 1e-6f && gusty.z < 1e-6f, "gusts: %.3f along, %.3f across",
          gusty.x, gusty.y);
    CHECK(std::abs(gusty.w - 1.0f) < 0.1f, "the gusts' mean %.3f of the mean wind", gusty.w);
    const glm::vec4 rough = deviations(0.0f, 1.0f);
    CHECK(rough.y > 0.2f && rough.y < 0.4f && rough.x > 0.2f && rough.x < 0.4f && rough.z > 0.06f && rough.z < 0.2f,
          "eddies: %.3f along, %.3f across, %.3f up", rough.x, rough.y, rough.z);
    const glm::vec4 calm = deviations(0.0f, 0.0f);
    CHECK(calm.x < 1e-5f && calm.y < 1e-5f && calm.z < 1e-5f, "a steady wind varies");

    Settings s = clearSky();
    s.wind = {.direction = {3.0f, 4.0f}, .speed = 12.0f, .gusts = 0.8f, .turbulence = 0.6f};
    Weather w(s);
    w.setTime(321.0);
    const glm::vec3 p{5.0f, 2.0f, 7.0f};
    CHECK(w.windOffset(p) != glm::vec3(0.0f) && w.windAt(p) != w.meanWindAt(p), "no gusts");
    s.wind.gusts = s.wind.turbulence = 0.0f;
    w.setSettings(s);
    CHECK(w.windOffset(p) == glm::vec3(0.0f) && w.windAt(p) == w.meanWindAt(p), "gusts without gusts");
}

// The offset the gusts give the air changes at the rate of their wind, for a point that travels with them (at 0.6 of
// the wind at the cloud base); the wind is the same after setTime as after updates, and goes on without a jump where
// its period starts again.
QUENCH_TEST(weather_wind_offset_and_period) {
    Settings s = clearSky();
    s.wind = {.direction = {0.6f, 0.8f}, .speed = 15.0f, .gusts = 0.7f, .turbulence = 0.5f};
    Weather w(s);
    // Early, where the air has moved 300 m: the field's coordinates subtract the air's distance (a float), and at 3 km
    // its rounding alone puts a few hundredths of a m/s into a rate over 0.01 s (more without fused multiply-adds, as
    // in WebAssembly).
    w.setTime(20.0);
    const glm::vec2 dir(0.6f, 0.8f);
    constexpr double kStep = 0.01;
    float worst = 0.0f, largest = 0.0f;
    glm::vec3 p{12.0f, 3.0f, -20.0f};
    for (int i = 0; i < 400; ++i) {
        const glm::vec3 before = w.windOffset(p);
        const glm::vec3 gust = w.windAt(p) - w.meanWindAt(p);
        w.update(kStep);
        const glm::vec2 moved = dir * float(0.6 * 15.0 * kStep);
        const glm::vec3 next = p + glm::vec3(moved.x, 0.0f, moved.y);
        const glm::vec3 rate = (w.windOffset(next) - before) / float(kStep);
        const glm::vec3 gustAfter = w.windAt(next) - w.meanWindAt(next);
        worst = std::max(worst, glm::length(rate - 0.5f * (gust + gustAfter)));
        largest = std::max(largest, glm::length(gust));
        p = next;
    }
    std::printf("  the offset's rate against the gust: at most %.4f m/s off (gusts up to %.2f m/s)\n", worst, largest);
    CHECK(largest > 1.0f && worst < 0.02f * largest, "offset rate %.4f off with gusts of %.2f", worst, largest);

    Weather stepped(s);
    for (int i = 0; i < 1000; ++i) stepped.update(0.5);
    Weather jumped(s);
    jumped.setTime(500.0);
    const glm::vec3 a = stepped.windAt({1.0f, 4.0f, 2.0f}), b = jumped.windAt({1.0f, 4.0f, 2.0f});
    CHECK(glm::length(a - b) < 1e-2f, "updated (%.4f %.4f %.4f) against set (%.4f %.4f %.4f)", a.x, a.y, a.z, b.x, b.y,
          b.z);

    // Across the period's start against an ordinary step of the same length.
    const auto change = [&](double t) {
        Weather x(s);
        x.setTime(t - 0.005);
        const glm::vec3 v0 = x.windAt({0.0f, 5.0f, 0.0f});
        x.setTime(t + 0.005);
        return glm::length(x.windAt({0.0f, 5.0f, 0.0f}) - v0);
    };
    const float seam = change(double(QUENCH_WIND_PERIOD)), ordinary = change(1000.0);
    std::printf("  over 0.01 s: %.4f m/s where the period starts again, %.4f elsewhere\n", seam, ordinary);
    CHECK(seam < 0.05f && ordinary < 0.05f, "the wind jumps: %.4f at the period, %.4f elsewhere", seam, ordinary);
}

// Lightning: flashes only where a cumulonimbus rains, within its core, at about the rate asked for; the same flashes
// however a span is cut and from a later time; none at lightning 0 or from a shower that is not a storm.
QUENCH_TEST(weather_lightning) {
    Settings s = clearSky();
    s.wind = {.direction = {1.0f, 0.0f}, .speed = 5.0f};
    Weather w(s);
    w.setTime(1000.0);
    w.addCell({.position = {0.0f, 0.0f}, .radius = 3200.0f, .precipitation = 1.0f});
    std::vector<Strike> all, part, out;
    for (double t = 400.0; t < 1000.0; t += 60.0) {
        w.strikes({0.0f, 0.0f}, 20000.0f, t, t + 60.0, out);
        all.insert(all.end(), out.begin(), out.end());
    }
    for (double t = 400.0; t < 1000.0; t += 7.5) {
        w.strikes({0.0f, 0.0f}, 20000.0f, t, t + 7.5, out);
        part.insert(part.end(), out.begin(), out.end());
    }
    float farthest = 0.0f;
    bool ordered = true;
    for (std::size_t i = 0; i < all.size(); ++i) {
        // The cell drifts with the wind (5 m/s): where it was then.
        const glm::vec2 centre(5.0f * float(all[i].time - 1000.0), 0.0f);
        farthest = std::max(farthest, glm::length(all[i].position - centre));
        if (i > 0 && all[i].time < all[i - 1].time) ordered = false;
    }
    std::printf("  %zu flashes in 10 minutes (%.1f a minute), the farthest %.0f m from the cell's centre\n", all.size(),
                double(all.size()) / 10.0, farthest);
    CHECK(all.size() >= 5 && all.size() <= 200, "%zu flashes in 10 minutes", all.size());
    CHECK(farthest < 2000.0f && ordered, "a flash %.0f m from the core", farthest);
    bool same = all.size() == part.size();
    for (std::size_t i = 0; same && i < all.size(); ++i)
        same = all[i].time == part[i].time && all[i].position == part[i].position;
    CHECK(same, "%zu flashes in minutes, %zu in pieces", all.size(), part.size());
    w.update(25.0);
    w.strikes({0.0f, 0.0f}, 20000.0f, 940.0, 1000.0, out);
    std::size_t expected = 0;
    for (const Strike& k : all) expected += k.time >= 940.0 ? 1 : 0;
    same = out.size() == expected;
    for (std::size_t i = 0; same && i < out.size(); ++i)
        same = std::abs(out[i].position.x - all[all.size() - expected + i].position.x) < 0.05f;
    CHECK(same, "later: %zu flashes in the last minute, %zu before", out.size(), expected);
    w.strikes({30000.0f, 0.0f}, 5000.0f, 940.0, 1000.0, out);
    CHECK(out.empty(), "%zu flashes far from the storm", out.size());
    // The flicker of a flash: dark before it, two to four strokes, dark again after kFlashLength.
    REQUIRE(!all.empty(), "no flashes");
    int fewest = 9, most = 0;
    for (const Strike& k : all) {
        int strokes = 0;
        float previous = Weather::flash(k, k.time - 0.001), rising = 0.0f;
        for (double age = 0.0; age < Weather::kFlashLength; age += 0.001) {
            const float light = Weather::flash(k, k.time + age);
            if (light > previous + 0.05f && rising <= 0.0f) ++strokes;
            rising = light - previous;
            previous = light;
        }
        fewest = std::min(fewest, strokes);
        most = std::max(most, strokes);
        CHECK(Weather::flash(k, k.time - 0.01) == 0.0f &&
                  Weather::flash(k, k.time + Weather::kFlashLength + 1e-6) == 0.0f &&
                  Weather::flash(k, k.time) >= k.brightness,
              "a flash at %.3f s: %.3f before, %.3f at, %.3f after", k.time, double(Weather::flash(k, k.time - 0.01)),
              double(Weather::flash(k, k.time)), double(Weather::flash(k, k.time + Weather::kFlashLength + 1e-6)));
    }
    std::printf("  strokes per flash: %d to %d\n", fewest, most);
    CHECK(fewest >= 2 && most <= 4, "%d to %d strokes", fewest, most);

    // A frame's query: 30 km around, 1/60 s.
    const auto start = std::chrono::steady_clock::now();
    constexpr int kFrames = 200;
    std::size_t seen = 0;
    for (int i = 0; i < kFrames; ++i) {
        const double t = 960.0 + double(i) / 60.0;
        w.strikes({0.0f, 0.0f}, 30000.0f, t, t + 1.0 / 60.0, out);
        seen += out.size();
    }
    std::printf("  a frame's query of 30 km: %.4f ms (%zu flashes)\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / kFrames,
                seen);

    s.lightning = 0.0f;
    w.setSettings(s);
    w.strikes({0.0f, 0.0f}, 20000.0f, 940.0, 1000.0, out);
    CHECK(out.empty(), "%zu flashes at lightning 0", out.size());
    Weather shower(clearSky());
    shower.addCell({.position = {0.0f, 0.0f}, .type = CloudType::Cumulus, .precipitation = 1.0f});
    shower.strikes({0.0f, 0.0f}, 20000.0f, 0.0, 60.0, out);
    CHECK(out.empty(), "%zu flashes from a cumulus", out.size());
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

    s.wind.gusts = 0.6f;
    s.wind.turbulence = 0.4f;
    w.setSettings(s);
    const auto windStart = std::chrono::steady_clock::now();
    glm::vec3 wind(0.0f);
    for (int i = 0; i < kCount; ++i) wind += w.windAt({float(i) * 1.3f, 2.0f, float(i) * 0.7f});
    const double windNs =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - windStart).count() / kCount;
    std::printf("  windAt with gusts and eddies: %.0f ns (sum %.1f)\n", windNs, double(wind.x));
    CHECK(windNs < 100000.0, "a wind query takes %.0f ns", windNs);
}
