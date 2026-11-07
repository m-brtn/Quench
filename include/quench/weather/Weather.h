#pragma once

// The weather of a world (docs/weather.md): a map of clouds and precipitation over the ground that the wind moves and
// that changes shape over time, with cells a game places on it (one raining cumulonimbus in a blue sky, an overcast
// region over the mountains). Headless: the renderer draws its clouds and their shadows (render::Scene::setWeather),
// a game asks it what is above a point (precipitation, how overcast it is, the wind).
//
// The weather is a pure function of its settings, its cells and its clock: the same seed, settings, cells and time
// give the same weather in every run and on every platform. The one thing it remembers is how wet the ground is
// around a focus (wetness), which follows the rain that actually fell there; the same calls in the same order give
// the same wetness. The field itself is shared with the GPU
// (shaders/quench/WeatherField.h), so a query and the clouds on the screen agree. Not thread-safe: one thread changes
// it; const queries may run on several threads at once while nothing changes it.

#include <quench/WeatherField.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace quench::weather {

// The cloud types of the map, on one continuous scale (the map blends between neighbours).
enum class CloudType : std::uint8_t {
    Stratus,      // a flat grey layer, a few hundred metres thick: overcast, drizzle
    Cumulus,      // fair-weather heaps, about 1.5 km tall, flat bases
    Towering,     // towering cumulus, about 5 km tall
    Cumulonimbus, // a storm cloud up to Settings::cloudTop with an anvil, heavy showers
};

// 0, 1/3, 2/3 or 1: the type's value on the map's scale.
float typeValue(CloudType type);
// The type nearest to a value on that scale.
CloudType nearestType(float value);

struct Wind {
    glm::vec2 direction{1.0f, 0.0f}; // world xz, the way the air moves (normalized where it is used)
    float speed = 8.0f;              // m/s at the clouds' height
    bool operator==(const Wind&) const = default;
};

struct Settings {
    std::uint32_t seed = 1; // the field's shape (its low 24 bits)
    // The field: how much of the sky its clouds cover (0 clear, 1 overcast), their type, how much the type varies
    // over the map, how much the thick parts rain or snow (0..1) and the high cirrus (0..1).
    float coverage = 0.35f;
    CloudType type = CloudType::Cumulus;
    float typeVariation = 0.15f;
    float precipitation = 0.0f;
    float cirrus = 0.0f;
    Wind wind{};
    // How fast the clouds change shape: at 1, a field of clouds has re-formed after about 10 minutes; 0 freezes it
    // (the wind still moves it).
    float evolution = 1.0f;
    float featureSize = 6000.0f; // m: the size of the field's groups of clouds and of the clear gaps between them
    float cloudBase = 1500.0f;   // m: the height of the cloud bases above y = 0
    float cloudTop = 9000.0f;    // m: the top of the tallest clouds (cumulonimbus); the other types are lower
    float cirrusHeight = 8000.0f;
    // Wetness (Weather::wetness): seconds of precipitation 1 that soak dry ground (twice as long at 0.5) and that fill
    // the hollows with standing water, and seconds without precipitation that dry soaked ground and empty full
    // puddles.
    float wettingTime = 120.0f;
    float dryingTime = 900.0f;
    float puddleFillTime = 900.0f;
    float puddleDryTime = 2400.0f;
    bool operator==(const Settings&) const = default;
};

// A cloud a game places: round, with lobes that change over time, on top of the field.
struct Cell {
    glm::vec2 position{0.0f}; // world xz of its centre (m) when it is set
    float radius = 2500.0f;   // m: the cloud fades out towards it; precipitation falls within about half of it
    CloudType type = CloudType::Cumulonimbus;
    float coverage = 1.0f;      // at its centre (0..1)
    float precipitation = 0.0f; // under its core (0..1)
    bool drifts = true;         // moves with the wind (false: stays over its position, a cloud on a mountain)
    bool operator==(const Cell&) const = default;
};

using CellId = std::uint32_t;
inline constexpr CellId kNoCell = 0;

// The map at a point: each 0..1. type is on the CloudType scale (0 stratus .. 1 cumulonimbus).
struct Sample {
    float coverage = 0.0f;
    float type = 0.0f;
    float precipitation = 0.0f;
    float cirrus = 0.0f;
};

// How wet open ground is: each 0..1.
struct Wetness {
    float ground = 0.0f; // 0 dry .. 1 soaked (darker, glossier)
    float water = 0.0f;  // standing water in the hollows: 0 none .. 1 full puddles
};

// A cell of the remembered wetness (Weather::wetnessGrid): the wetness at its centre and the precipitation falling
// there at its last update.
struct WetnessCell {
    Wetness wetness{};
    float precipitation = 0.0f;
};

class Weather {
public:
    static constexpr std::size_t kMaxCells = QUENCH_MAX_CLOUD_CELLS;
    // Weather time for one unit of evolution at Settings::evolution 1 (s).
    static constexpr double kEvolutionPeriod = 600.0;
    // The remembered wetness: kWetnessGrid x kWetnessGrid cells of kWetnessCell m around the wetness focus, updated
    // once kWetnessStep s of weather time have passed.
    static constexpr int kWetnessGrid = 16;
    static constexpr float kWetnessCell = 64.0f;
    static constexpr double kWetnessStep = 0.25;
    // Precipitation below this neither wets nor keeps the ground from drying.
    static constexpr float kWettingThreshold = 0.02f;

    explicit Weather(const Settings& settings = {});

    // Takes effect at once; the air's offset and the evolution time stay (a new wind moves the air from where it is).
    // The wetness stays too and follows the new precipitation from now on.
    void setSettings(const Settings& settings);
    const Settings& settings() const { return m_settings; }

    // The weather's clock. update() moves the air by the wind and advances the evolution by the current settings, and
    // wets or dries the remembered ground by the precipitation now (in steps of at least kWetnessStep; one long update
    // is one step); setTime() jumps to a time as if the settings had always been the current ones (what a scripted
    // check or a saved game uses: the weather at time t is then the same however it got there), the wetness included
    // (estimated from the last hours of that weather).
    void update(double dt);
    void setTime(double time);
    double time() const { return m_time; }
    // How far the air has moved since time 0 (world xz, m) and the field's evolution time.
    glm::vec2 airOffset() const { return m_airOffset; }
    double evolutionTime() const { return m_evolution; }

    // Cells: kNoCell when kMaxCells are placed. `position` is where the cell is now; a drifting cell moves on with
    // the air from there.
    CellId addCell(const Cell& cell);
    bool setCell(CellId id, const Cell& cell);
    bool removeCell(CellId id);
    void clearCells();
    // The cell as set, with its current position.
    std::optional<Cell> cell(CellId id) const;
    std::size_t cellCount() const { return m_cells.size(); }

    // The map at a world point (xz, m), now.
    Sample sample(glm::vec2 xz) const;
    // The precipitation falling at a point: the map's below the cloud base, 0 above it. No cover (roofs, trees) is
    // known here: the renderer's precipitation pass handles that (docs/weather.md, part 2).
    float precipitation(glm::vec3 point) const;
    // The distance (m) from a point to the nearest place where the precipitation is at least `threshold` (0 inside
    // one), or maxDistance when there is none within it. A search on at most 48 rings of 32 directions (steps of
    // maxDistance / 48, at least 50 m): 0.03 ms on the M4 Pro with rain 3.7 km away, about 0.3 ms when there is none
    // (1536 samples of about 0.2 us); call it a few times a second, not per frame per object.
    float distanceToPrecipitation(glm::vec2 xz, float threshold = 0.1f, float maxDistance = 20000.0f) const;
    // The mean coverage within `radius` of a point (19 samples): how overcast it is there.
    float cover(glm::vec2 xz, float radius) const;
    // The wind at a point: the settings' wind at the clouds' height, less near the ground (a logarithmic profile,
    // about 0.3 of it at 2 m under a base at 1.5 km). Gusts and local wind come with part 4 (docs/weather.md).
    glm::vec3 windAt(glm::vec3 point) const;
    // The velocity of the rain falling at a point (m/s): kRainFallSpeed down, carried by windAt. The renderer's rain
    // occlusion map looks along it; a game's own test for cover (a raycast up against it from a listener) can too.
    static constexpr float kRainFallSpeed = 7.5f;
    glm::vec3 rainVelocity(glm::vec3 point) const;
    // The height of the cloud tops of a type value (m).
    float cloudTop(float type) const;
    // An upper bound of Sample::type anywhere on the map, the field's and the cells' (what the renderer marches
    // through: cloudTop(highestType()) is the highest top there can be).
    float highestType() const;

    // The map on a grid: resolution x resolution samples at the texel centres of [origin, origin + size] (world xz),
    // row by row in +z. `out` holds resolution * resolution samples.
    void fill(glm::vec2 origin, float size, std::uint32_t resolution, std::span<Sample> out) const;

    // How wet open ground is at a point (no roofs or trees are known here; the renderer's rain map adds them). Within
    // the remembered grid it is the rain that fell there since the grid came there; elsewhere the estimate setTime()
    // makes (about 0.01 ms: 48 samples of the weather's last hours).
    Wetness wetness(glm::vec2 xz) const;
    // Where the grid of remembered wetness is centred: a game's camera or player, set every frame (the grid moves in
    // whole cells; the cells it brings in start from the estimate). At the origin until set.
    void setWetnessFocus(glm::vec2 xz);
    glm::vec2 wetnessFocus() const { return m_wetFocus; }
    // The grid: kWetnessGrid * kWetnessGrid cells, row by row in +z, the first with its corner at wetnessOrigin().
    std::span<const WetnessCell> wetnessGrid() const { return m_wetCells; }
    glm::vec2 wetnessOrigin() const { return m_wetOrigin; }
    // Changes with every update and move of the grid, to a number no other Weather has had (what the renderer
    // uploads again when it changes); a copy keeps it.
    std::uint64_t wetnessVersion() const { return m_wetVersion; }

    // What the renderer's weather pass reads (shaders/quench/WeatherField.h).
    const shader::WeatherUniforms& uniforms() const { return m_uniforms; }

private:
    struct CellEntry {
        CellId id = kNoCell;
        Cell cell{};
        glm::vec2 anchor{0.0f}; // in the air's coordinates when it drifts, else in the world's
    };

    void refresh();
    glm::vec2 cellPosition(const CellEntry& entry) const;
    // The field as it was `ago` seconds before now, had the settings always been the current ones.
    shader::WeatherUniforms pastUniforms(double ago) const;
    // The wetness the last hours of this weather leave at each point (and the precipitation there now).
    void estimateWetness(std::span<const glm::vec2> points, std::span<WetnessCell> out) const;
    void estimateGrid();
    Wetness wetted(Wetness w, float precipitation, double dt) const;

    Settings m_settings{};
    double m_time = 0.0;
    double m_evolution = 0.0;
    glm::vec2 m_airOffset{0.0f};
    std::vector<CellEntry> m_cells; // in id order
    CellId m_nextId = 1;
    shader::WeatherUniforms m_uniforms{};
    glm::vec2 m_wetFocus{0.0f};
    glm::vec2 m_wetOrigin{0.0f};
    std::vector<WetnessCell> m_wetCells;
    double m_wetPending = 0.0; // weather time since the grid's last update
    std::uint64_t m_wetVersion = 0;
};

} // namespace quench::weather
