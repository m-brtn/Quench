#pragma once

// Particles and trails without a window or a GPU (docs/particles.md): what a particle system is (ParticleDesc), its
// spawns (Emitter), the CPU simulation (Simulation) that steps particles as the render module's compute pass does
// (one source, quench/ParticleField.h), trails behind moving points (Trail) and the smooth curves both follow (Path).
// The render module keeps systems and trails in its retained scene (render::Scene::addParticles, addTrail) and draws
// them.

#include <quench/ParticleField.h>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace quench::particles {

using Particle = shader::QuenchParticle;
using ParticleInstance = shader::QuenchParticleInstance;
using ParticleUniforms = shader::ParticleUniforms;

// The most slots of one system, and of every system of a scene together.
inline constexpr std::uint32_t kMaxSystemParticles = 65536;
inline constexpr std::uint32_t kMaxSceneParticles = 262144;

// A value drawn evenly from min..max.
struct Range {
    float min = 0.0f;
    float max = 0.0f;
    float at(float t) const { return min + (max - min) * t; }
};

// A value over 0..1 (a particle's life, a trail's age): up to kMaxKeys keys in increasing t, linear between them,
// flat before the first and after the last.
struct FloatCurve {
    struct Key {
        float t = 0.0f;
        float value = 1.0f;
    };
    static constexpr std::size_t kMaxKeys = 8;

    FloatCurve(float value = 1.0f);
    FloatCurve(std::initializer_list<Key> keys); // more than kMaxKeys: the rest are dropped
    float operator()(float t) const;

    std::array<Key, kMaxKeys> keys{};
    std::uint32_t count = 0;
};

// The same for linear HDR color and opacity.
struct ColorCurve {
    struct Key {
        float t = 0.0f;
        glm::vec4 value{1.0f};
    };
    static constexpr std::size_t kMaxKeys = 8;

    ColorCurve(const glm::vec4& value = glm::vec4(1.0f));
    ColorCurve(std::initializer_list<Key> keys);
    glm::vec4 operator()(float t) const;

    std::array<Key, kMaxKeys> keys{};
    std::uint32_t count = 0;
};

// Where an emitter spawns, in its own space (its transform places, turns and scales it).
enum class Shape : std::uint8_t {
    Point,
    Sphere, // extent.x: radius
    Box,    // extent: half sizes
    Disc,   // extent.x: radius, in the emitter's xz plane
    Path,   // along ParticleDesc::path, extent.x: the radius around it
};

enum class Blend : std::uint8_t {
    Additive, // light: adds its color times its opacity (glows, sparks, magic)
    Alpha,    // covers what is behind by its opacity (smoke, dust); unsorted within a system
};

// The procedural shape of a particle's quad.
enum class Sprite : std::uint8_t {
    Glow, // a bright core in a soft halo
    Soft, // a smooth disc
    Star, // a glow with four thin rays (sparkles)
    Puff, // a soft lumpy blob (smoke, dust)
};

enum class Facing : std::uint8_t {
    Camera,   // a quad facing the camera, turned by the particle's rotation
    Velocity, // drawn out along its motion on the screen (sparks, streaks): ParticleDesc::stretch
};

struct ParticleDesc {
    std::uint32_t maxParticles = 1024; // slots (1..kMaxSystemParticles): when all are alive, the oldest is replaced
    std::uint32_t seed = 1;            // the spawns' random numbers (the same seed spawns the same particles)

    // Spawning, in particles per second while emitting and per metre the emitter moves (frame-rate independent:
    // spawned at their time within the frame, along the emitter's motion), scaled by Emitter::setRateScale (the
    // renderer's RenderSettings::particleDensity).
    float rate = 50.0f;
    float ratePerMeter = 0.0f;
    Shape shape = Shape::Point;
    glm::vec3 extent{0.5f};
    bool surface = false;          // Sphere, Disc: on the surface or the rim only
    std::vector<glm::vec3> path{}; // Shape::Path: points in the emitter's space, a centripetal Catmull-Rom curve
    bool pathClosed = false;

    // A particle's start.
    Range lifetime{1.0f, 2.0f};            // s
    Range speed{0.5f, 1.0f};               // m/s
    glm::vec3 direction{0.0f, 1.0f, 0.0f}; // emitter space
    float spread = 0.5f;                   // half angle (rad) of the cone around direction; pi: every direction
    float outward = 0.0f;                  // 0..1: the direction turned away from the shape's centre (or path)
    float inherit = 0.0f;                  // the share of the emitter's velocity it takes
    Range size{0.05f, 0.1f};               // m: half the side of its quad
    Range rotation{0.0f, 0.0f};            // rad
    Range spin{0.0f, 0.0f};                // rad/s

    // Motion (quench/ParticleField.h): gravity, then drag toward the air's velocity, which is the curl noise plus a
    // share of the weather's wind (the scene's weather, render::Scene::setWeather).
    glm::vec3 gravity{0.0f};
    float drag = 0.0f;       // 1/s
    float noise = 0.0f;      // m/s: the curl noise's speed (RMS along each axis)
    float noiseScale = 4.0f; // m: its features' size
    float noiseRate = 0.2f;  // 1/s: how fast it changes
    float wind = 0.0f;       // 0..1: the share of the weather's wind

    // Look: color and opacity times colorOverLife, size times sizeOverLife (over age / lifetime).
    glm::vec3 color{1.0f}; // linear HDR (above 1 glows through bloom)
    float opacity = 1.0f;
    FloatCurve sizeOverLife{};
    ColorCurve colorOverLife = {{0.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f)},
                                {0.1f, glm::vec4(1.0f)},
                                {0.7f, glm::vec4(1.0f)},
                                {1.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f)}};
    float brightnessVariation = 0.0f; // 0..1 between particles
    float twinkle = 0.0f;             // 0..1: how deep each particle's brightness flickers
    float twinkleRate = 1.0f;         // Hz (each particle 0.6..1.4 of it)
    float twinkleSharpness = 4.0f;    // 1: a sine; higher: shorter, brighter flashes
    Blend blend = Blend::Additive;
    Sprite sprite = Sprite::Glow;
    Facing facing = Facing::Camera;
    float stretch = 0.04f;  // Facing::Velocity: drawn along this many seconds of its motion
    float softness = 0.3f;  // m: fades out where it nears the scene behind it (0: a hard cut)
    float nearFade = 0.25f; // m: fades out nearer the camera than this
    float minPixels = 1.0f; // drawn at least this many pixels across, fainter by its area below that
    bool lit = false;       // Blend::Alpha: lit by the sun and the sky's ambient light (color is its albedo)
};

// The parts of a system's uniforms its desc decides (gravity, drag, noise, look, color, curves); range, clock and
// wind are the step's (Emitter::step, the renderer).
ParticleUniforms uniformsOf(const ParticleDesc& desc);

} // namespace quench::particles
