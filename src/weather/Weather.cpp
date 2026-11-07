#include <quench/weather/Weather.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace quench::weather {

namespace {

// Versions of the remembered wetness, unique across every Weather (a renderer that is given another one uploads its
// grid even when the two have changed as often).
std::atomic<std::uint64_t> g_wetVersions{0};

std::uint64_t nextWetVersion() { return g_wetVersions.fetch_add(1, std::memory_order_relaxed) + 1; }

glm::vec2 unitOr(glm::vec2 v, glm::vec2 fallback) {
    const float len = glm::length(v);
    return len > 1e-6f ? v / len : fallback;
}

Sample toSample(glm::vec4 v) { return {v.x, v.y, v.z, v.w}; }

// A per-cell phase of the evolution, so two cells never have the same lobes.
float cellPhase(CellId id) { return float(id % 64u) * 7.31f; }

// The wetness estimate: steps over the weather's past, as far back as soaked ground and full puddles take to dry
// after wetting (at most two hours).
constexpr int kEstimateSteps = 48;

glm::vec2 gridOrigin(glm::vec2 focus) {
    const float cell = Weather::kWetnessCell;
    return glm::floor(focus / cell) * cell - float(Weather::kWetnessGrid / 2) * cell;
}

} // namespace

float typeValue(CloudType type) { return float(type) / 3.0f; }

CloudType nearestType(float value) {
    return CloudType(std::clamp(int(std::lround(std::clamp(value, 0.0f, 1.0f) * 3.0f)), 0, 3));
}

Weather::Weather(const Settings& settings) : m_settings(settings) {
    refresh();
    m_wetOrigin = gridOrigin(m_wetFocus);
    m_wetCells.resize(std::size_t(kWetnessGrid) * kWetnessGrid);
    estimateGrid();
}

void Weather::setSettings(const Settings& settings) {
    m_settings = settings;
    refresh();
}

void Weather::update(double dt) {
    if (!(dt > 0.0)) return;
    m_time += dt;
    const glm::vec2 dir = unitOr(m_settings.wind.direction, {1.0f, 0.0f});
    m_airOffset += dir * float(double(m_settings.wind.speed) * dt);
    m_evolution += dt * double(std::max(m_settings.evolution, 0.0f)) / kEvolutionPeriod;
    refresh();
    m_wetPending += dt;
    if (m_wetPending < kWetnessStep) return;
    const float cell = kWetnessCell;
    for (int z = 0; z < kWetnessGrid; ++z)
        for (int x = 0; x < kWetnessGrid; ++x) {
            WetnessCell& c = m_wetCells[std::size_t(z * kWetnessGrid + x)];
            c.precipitation =
                sample(m_wetOrigin + glm::vec2((float(x) + 0.5f) * cell, (float(z) + 0.5f) * cell)).precipitation;
            c.wetness = wetted(c.wetness, c.precipitation, m_wetPending);
        }
    m_wetPending = 0.0;
    m_wetVersion = nextWetVersion();
}

void Weather::setTime(double time) {
    m_time = time;
    const glm::vec2 dir = unitOr(m_settings.wind.direction, {1.0f, 0.0f});
    m_airOffset = dir * float(double(m_settings.wind.speed) * time);
    m_evolution = time * double(std::max(m_settings.evolution, 0.0f)) / kEvolutionPeriod;
    refresh();
    estimateGrid();
}

Wetness Weather::wetted(Wetness w, float precipitation, double dt) const {
    const Settings& s = m_settings;
    const float t = float(dt);
    if (precipitation > kWettingThreshold) {
        w.ground = std::min(1.0f, w.ground + t * precipitation / std::max(s.wettingTime, 1.0f));
        w.water = std::min(1.0f, w.water + t * precipitation / std::max(s.puddleFillTime, 1.0f));
    } else {
        w.ground = std::max(0.0f, w.ground - t / std::max(s.dryingTime, 1.0f));
        w.water = std::max(0.0f, w.water - t / std::max(s.puddleDryTime, 1.0f));
    }
    return w;
}

shader::WeatherUniforms Weather::pastUniforms(double ago) const {
    shader::WeatherUniforms u = m_uniforms;
    const glm::vec2 moved = glm::vec2(u.air.z, u.air.w) * float(double(m_settings.wind.speed) * ago);
    u.air.x -= moved.x;
    u.air.y -= moved.y;
    u.shape.y = float(m_evolution - ago * double(std::max(m_settings.evolution, 0.0f)) / kEvolutionPeriod);
    for (std::size_t i = 0; i < m_cells.size(); ++i) {
        if (!m_cells[i].cell.drifts) continue;
        u.cells[i * 2].x -= moved.x;
        u.cells[i * 2].y -= moved.y;
    }
    return u;
}

void Weather::estimateWetness(std::span<const glm::vec2> points, std::span<WetnessCell> out) const {
    const Settings& s = m_settings;
    const double span =
        std::min(7200.0, double(std::max(std::max(s.wettingTime, 1.0f) + std::max(s.dryingTime, 1.0f),
                                         std::max(s.puddleFillTime, 1.0f) + std::max(s.puddleDryTime, 1.0f))));
    const double step = span / double(kEstimateSteps);
    for (std::size_t i = 0; i < points.size(); ++i) out[i] = {};
    for (int k = 0; k < kEstimateSteps; ++k) {
        const shader::WeatherUniforms u = pastUniforms(span - (double(k) + 0.5) * step);
        for (std::size_t i = 0; i < points.size(); ++i) {
            const float p = shader::quenchWeatherSample(u, points[i]).z;
            out[i].wetness = wetted(out[i].wetness, p, step);
        }
    }
    for (std::size_t i = 0; i < points.size(); ++i) out[i].precipitation = sample(points[i]).precipitation;
}

void Weather::estimateGrid() {
    std::vector<glm::vec2> points(m_wetCells.size());
    for (int z = 0; z < kWetnessGrid; ++z)
        for (int x = 0; x < kWetnessGrid; ++x)
            points[std::size_t(z * kWetnessGrid + x)] =
                m_wetOrigin + glm::vec2((float(x) + 0.5f) * kWetnessCell, (float(z) + 0.5f) * kWetnessCell);
    estimateWetness(points, m_wetCells);
    m_wetPending = 0.0;
    m_wetVersion = nextWetVersion();
}

void Weather::setWetnessFocus(glm::vec2 xz) {
    m_wetFocus = xz;
    const glm::vec2 origin = gridOrigin(xz);
    if (origin == m_wetOrigin) return;
    const glm::ivec2 shift = glm::ivec2(glm::round((origin - m_wetOrigin) / kWetnessCell));
    std::vector<WetnessCell> cells(m_wetCells.size());
    std::vector<glm::vec2> fresh;
    std::vector<std::size_t> freshIndex;
    for (int z = 0; z < kWetnessGrid; ++z)
        for (int x = 0; x < kWetnessGrid; ++x) {
            const int ox = x + shift.x;
            const int oz = z + shift.y;
            const std::size_t i = std::size_t(z * kWetnessGrid + x);
            if (ox >= 0 && ox < kWetnessGrid && oz >= 0 && oz < kWetnessGrid) {
                cells[i] = m_wetCells[std::size_t(oz * kWetnessGrid + ox)];
            } else {
                fresh.push_back(origin + glm::vec2((float(x) + 0.5f) * kWetnessCell, (float(z) + 0.5f) * kWetnessCell));
                freshIndex.push_back(i);
            }
        }
    std::vector<WetnessCell> estimated(fresh.size());
    estimateWetness(fresh, estimated);
    for (std::size_t k = 0; k < fresh.size(); ++k) cells[freshIndex[k]] = estimated[k];
    m_wetCells = std::move(cells);
    m_wetOrigin = origin;
    m_wetVersion = nextWetVersion();
}

Wetness Weather::wetness(glm::vec2 xz) const {
    // Bilinear between the cell centres inside the grid; the estimate outside it.
    const glm::vec2 g = (xz - m_wetOrigin) / kWetnessCell - 0.5f;
    const float last = float(kWetnessGrid - 1);
    if (g.x >= -0.5f && g.y >= -0.5f && g.x <= last + 0.5f && g.y <= last + 0.5f) {
        const glm::vec2 c = glm::clamp(g, glm::vec2(0.0f), glm::vec2(last));
        const glm::ivec2 i0 = glm::min(glm::ivec2(glm::floor(c)), glm::ivec2(kWetnessGrid - 2));
        const glm::vec2 f = c - glm::vec2(i0);
        const auto at = [&](int x, int z) { return m_wetCells[std::size_t(z * kWetnessGrid + x)].wetness; };
        const Wetness a = at(i0.x, i0.y), b = at(i0.x + 1, i0.y), d = at(i0.x, i0.y + 1), e = at(i0.x + 1, i0.y + 1);
        const auto mix = [&](float va, float vb, float vd, float ve) {
            return (va * (1.0f - f.x) + vb * f.x) * (1.0f - f.y) + (vd * (1.0f - f.x) + ve * f.x) * f.y;
        };
        return {mix(a.ground, b.ground, d.ground, e.ground), mix(a.water, b.water, d.water, e.water)};
    }
    WetnessCell out{};
    estimateWetness({&xz, 1}, {&out, 1});
    return out.wetness;
}

CellId Weather::addCell(const Cell& cell) {
    if (m_cells.size() >= kMaxCells) return kNoCell;
    CellEntry entry{m_nextId++, cell, cell.drifts ? cell.position - m_airOffset : cell.position};
    m_cells.push_back(entry);
    refresh();
    return entry.id;
}

bool Weather::setCell(CellId id, const Cell& cell) {
    for (CellEntry& entry : m_cells)
        if (entry.id == id) {
            entry.cell = cell;
            entry.anchor = cell.drifts ? cell.position - m_airOffset : cell.position;
            refresh();
            return true;
        }
    return false;
}

bool Weather::removeCell(CellId id) {
    const auto it = std::find_if(m_cells.begin(), m_cells.end(), [id](const CellEntry& e) { return e.id == id; });
    if (it == m_cells.end()) return false;
    m_cells.erase(it);
    refresh();
    return true;
}

void Weather::clearCells() {
    m_cells.clear();
    refresh();
}

std::optional<Cell> Weather::cell(CellId id) const {
    for (const CellEntry& entry : m_cells)
        if (entry.id == id) {
            Cell c = entry.cell;
            c.position = cellPosition(entry);
            return c;
        }
    return std::nullopt;
}

glm::vec2 Weather::cellPosition(const CellEntry& entry) const {
    return entry.cell.drifts ? entry.anchor + m_airOffset : entry.anchor;
}

void Weather::refresh() {
    const Settings& s = m_settings;
    shader::WeatherUniforms& u = m_uniforms;
    u = {};
    u.field = {std::clamp(s.coverage, 0.0f, 1.0f), typeValue(s.type), std::clamp(s.typeVariation, 0.0f, 1.0f),
               std::clamp(s.precipitation, 0.0f, 1.0f)};
    u.shape = {1.0f / std::max(s.featureSize, 1.0f), float(m_evolution), std::clamp(s.cirrus, 0.0f, 1.0f),
               float(s.seed & 0xffffffu)};
    const glm::vec2 dir = unitOr(s.wind.direction, {1.0f, 0.0f});
    u.air = {m_airOffset.x, m_airOffset.y, dir.x, dir.y};
    const float base = std::max(s.cloudBase, 0.0f);
    u.layer = {base, std::max(s.cloudTop, base + 100.0f), s.cirrusHeight, float(m_cells.size())};
    for (std::size_t i = 0; i < m_cells.size(); ++i) {
        const CellEntry& e = m_cells[i];
        const glm::vec2 p = cellPosition(e);
        u.cells[i * 2] = {p.x, p.y, std::max(e.cell.radius, 1.0f), std::clamp(e.cell.coverage, 0.0f, 1.0f)};
        u.cells[i * 2 + 1] = {typeValue(e.cell.type), std::clamp(e.cell.precipitation, 0.0f, 1.0f), cellPhase(e.id),
                              0.0f};
    }
}

Sample Weather::sample(glm::vec2 xz) const { return toSample(shader::quenchWeatherSample(m_uniforms, xz)); }

float Weather::precipitation(glm::vec3 point) const {
    if (point.y >= m_uniforms.layer.x) return 0.0f;
    return sample({point.x, point.z}).precipitation;
}

float Weather::distanceToPrecipitation(glm::vec2 xz, float threshold, float maxDistance) const {
    const auto raining = [&](glm::vec2 p) { return sample(p).precipitation >= threshold; };
    if (raining(xz)) return 0.0f;
    if (!(maxDistance > 0.0f)) return 0.0f;
    // Without precipitation in the field or in a cell there is nothing to find.
    bool any = m_uniforms.field.w > 0.0f;
    for (const CellEntry& e : m_cells) any = any || e.cell.precipitation >= threshold;
    if (!any) return maxDistance;

    constexpr int kDirections = 32;
    const float step = std::max(maxDistance / 48.0f, 50.0f);
    float previous = 0.0f;
    for (float r = step; previous < maxDistance; r += step) {
        const float ring = std::min(r, maxDistance);
        float best = maxDistance;
        for (int k = 0; k < kDirections; ++k) {
            const float a = 6.2831853f * (float(k) + 0.5f * float(int(ring / step) & 1)) / float(kDirections);
            const glm::vec2 dir{std::cos(a), std::sin(a)};
            if (!raining(xz + dir * ring)) continue;
            // Bisection between the previous ring and this one along the direction.
            float lo = previous;
            float hi = ring;
            for (int i = 0; i < 10; ++i) {
                const float mid = 0.5f * (lo + hi);
                if (raining(xz + dir * mid))
                    hi = mid;
                else
                    lo = mid;
            }
            best = std::min(best, hi);
        }
        if (best < maxDistance) return best;
        previous = ring;
    }
    return maxDistance;
}

float Weather::cover(glm::vec2 xz, float radius) const {
    float sum = sample(xz).coverage;
    int count = 1;
    for (int ring = 1; ring <= 2; ++ring) {
        const int n = ring * 6;
        const float r = radius * float(ring) / 2.0f;
        for (int k = 0; k < n; ++k) {
            const float a = 6.2831853f * (float(k) + 0.5f * float(ring - 1)) / float(n);
            sum += sample(xz + glm::vec2(std::cos(a), std::sin(a)) * r).coverage;
            ++count;
        }
    }
    return sum / float(count);
}

glm::vec3 Weather::windAt(glm::vec3 point) const {
    const glm::vec2 dir = unitOr(m_settings.wind.direction, {1.0f, 0.0f});
    // Logarithmic profile with a roughness length of 0.1 m, scaled so that it is 1 at the cloud base.
    constexpr float kRoughness = 0.1f;
    const float height = std::max(point.y, kRoughness * 1.01f);
    const float top = std::max(m_uniforms.layer.x, 10.0f);
    const float scale = std::clamp(std::log(height / kRoughness) / std::log(top / kRoughness), 0.0f, 1.0f);
    const float speed = m_settings.wind.speed * scale;
    return {dir.x * speed, 0.0f, dir.y * speed};
}

glm::vec3 Weather::rainVelocity(glm::vec3 point) const { return windAt(point) - glm::vec3(0.0f, kRainFallSpeed, 0.0f); }

float Weather::cloudTop(float type) const {
    const float base = m_uniforms.layer.x;
    const float top = m_uniforms.layer.y;
    // Thickness by type: stratus 500 m, cumulus 1.5 km, towering cumulus 5 km, cumulonimbus up to the top.
    const float t = std::clamp(type, 0.0f, 1.0f) * 3.0f;
    const float thick[4] = {500.0f, 1500.0f, 5000.0f, std::max(top - base, 5000.0f)};
    const int i = std::min(int(t), 2);
    return base + std::min(thick[i] + (thick[i + 1] - thick[i]) * (t - float(i)), top - base);
}

float Weather::highestType() const {
    // The field's type is the setting moved by the variation times at most 1.25 (its noise) plus 0.2 (its coverage):
    // quenchWeatherSample in quench/WeatherField.h.
    float highest =
        m_uniforms.field.x > 0.0f ? std::clamp(m_uniforms.field.y + m_uniforms.field.z * 1.45f, 0.0f, 1.0f) : 0.0f;
    for (const CellEntry& e : m_cells)
        if (e.cell.coverage > 0.0f) highest = std::max(highest, typeValue(e.cell.type));
    return highest;
}

void Weather::fill(glm::vec2 origin, float size, std::uint32_t resolution, std::span<Sample> out) const {
    if (resolution == 0 || out.size() < std::size_t(resolution) * resolution) return;
    const float texel = size / float(resolution);
    for (std::uint32_t z = 0; z < resolution; ++z)
        for (std::uint32_t x = 0; x < resolution; ++x)
            out[std::size_t(z) * resolution + x] =
                sample(origin + glm::vec2((float(x) + 0.5f) * texel, (float(z) + 0.5f) * texel));
}

} // namespace quench::weather
