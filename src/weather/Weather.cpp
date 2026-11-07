#include <quench/weather/Weather.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace quench::weather {

namespace {

glm::vec2 unitOr(glm::vec2 v, glm::vec2 fallback) {
    const float len = glm::length(v);
    return len > 1e-6f ? v / len : fallback;
}

Sample toSample(glm::vec4 v) { return {v.x, v.y, v.z, v.w}; }

// A per-cell phase of the evolution, so two cells never have the same lobes.
float cellPhase(CellId id) { return float(id % 64u) * 7.31f; }

} // namespace

float typeValue(CloudType type) { return float(type) / 3.0f; }

CloudType nearestType(float value) {
    return CloudType(std::clamp(int(std::lround(std::clamp(value, 0.0f, 1.0f) * 3.0f)), 0, 3));
}

Weather::Weather(const Settings& settings) : m_settings(settings) { refresh(); }

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
}

void Weather::setTime(double time) {
    m_time = time;
    const glm::vec2 dir = unitOr(m_settings.wind.direction, {1.0f, 0.0f});
    m_airOffset = dir * float(double(m_settings.wind.speed) * time);
    m_evolution = time * double(std::max(m_settings.evolution, 0.0f)) / kEvolutionPeriod;
    refresh();
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
