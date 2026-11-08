#include "Test.h"

#include <quench/particles/Emitter.h>
#include <quench/particles/Particles.h>
#include <quench/particles/Path.h>
#include <quench/particles/Simulation.h>
#include <quench/particles/Trail.h>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace quench;
using namespace quench::particles;

namespace {

glm::vec3 position(const Particle& q) { return {q.a.x, q.a.y, q.a.z}; }
glm::vec3 velocity(const Particle& q) { return {q.b.x, q.b.y, q.b.z}; }

// Runs an emitter (and a simulation of its slots) for `seconds` in steps of `dt`; returns the spawns.
struct Run {
    Emitter emitter;
    Simulation simulation;
    ParticleUniforms uniforms;
    std::vector<Particle> spawns;
    std::vector<Particle> all; // every spawn of every step
    double time = 0.0;

    explicit Run(const ParticleDesc& desc)
        : emitter(desc), simulation(emitter.capacity()), uniforms(uniformsOf(desc)) {}

    void step(float dt) {
        time += double(dt);
        spawns.clear();
        emitter.step(dt, float(std::fmod(time, double(QUENCH_PARTICLE_PERIOD))), uniforms, spawns);
        simulation.step(uniforms, spawns);
        all.insert(all.end(), spawns.begin(), spawns.end());
    }
    void run(float seconds, float dt) {
        const int n = int(std::lround(seconds / dt));
        for (int i = 0; i < n; ++i) step(dt);
    }
};

float stripLength(std::span<const TrailVertex> v) {
    float l = 0.0f;
    for (std::size_t i = 1; i < v.size(); ++i)
        if (v[i].position.w >= 0.0f) l += glm::length(glm::vec3(v[i].position) - glm::vec3(v[i - 1].position));
    return l;
}

} // namespace

QUENCH_TEST(particles_curves_interpolate) {
    const FloatCurve flat(2.5f);
    CHECK(flat(0.0f) == 2.5f && flat(1.0f) == 2.5f, "flat %g %g", flat(0.0f), flat(1.0f));
    const FloatCurve ramp = {{0.2f, 0.0f}, {0.6f, 1.0f}, {1.0f, 0.5f}};
    CHECK(ramp(0.0f) == 0.0f, "before the first key %g", ramp(0.0f));
    CHECK(std::abs(ramp(0.4f) - 0.5f) < 1e-6f, "between keys %g", ramp(0.4f));
    CHECK(std::abs(ramp(0.8f) - 0.75f) < 1e-6f, "between keys %g", ramp(0.8f));
    CHECK(ramp(2.0f) == 0.5f, "after the last key %g", ramp(2.0f));
    const ColorCurve fade = {{0.0f, glm::vec4(1.0f)}, {1.0f, glm::vec4(0.0f, 0.5f, 1.0f, 0.0f)}};
    const glm::vec4 mid = fade(0.5f);
    CHECK(std::abs(mid.x - 0.5f) < 1e-6f && std::abs(mid.y - 0.75f) < 1e-6f && std::abs(mid.w - 0.5f) < 1e-6f,
          "color %g %g %g %g", mid.x, mid.y, mid.z, mid.w);

    // The uniforms sample the curves at 16 points; the shared lookups read them back between the samples.
    ParticleDesc desc;
    desc.sizeOverLife = {{0.0f, 0.0f}, {1.0f, 3.0f}};
    desc.colorOverLife = fade;
    desc.color = glm::vec3(2.0f);
    const ParticleUniforms u = uniformsOf(desc);
    for (const float t : {0.0f, 0.13f, 0.5f, 0.77f, 1.0f}) {
        const float size = shader::quenchParticleSize(u, t);
        CHECK(std::abs(size - 3.0f * t) < 1e-5f, "size at %g: %g", t, size);
        const glm::vec4 c = shader::quenchParticleColor(u, t);
        const glm::vec4 want = fade(t);
        CHECK(glm::length(c - want) < 1e-5f, "color at %g", t);
    }
}

QUENCH_TEST(particles_rate_is_frame_rate_independent) {
    ParticleDesc desc;
    desc.rate = 100.0f;
    desc.maxParticles = 4096;
    desc.lifetime = {10.0f, 10.0f};
    for (const float hz : {30.0f, 60.0f, 144.0f, 240.0f}) {
        Run r(desc);
        r.run(2.0f, 1.0f / hz);
        CHECK(r.emitter.spawned() >= 199 && r.emitter.spawned() <= 200, "%g Hz: %llu spawns in 2 s at 100/s", hz,
              static_cast<unsigned long long>(r.emitter.spawned()));
        CHECK(r.emitter.alive() == r.simulation.alive(), "%g Hz: emitter %u alive, simulation %u", hz,
              r.emitter.alive(), r.simulation.alive());
    }
    // A scale (density) scales the rate.
    Run half(desc);
    half.emitter.setRateScale(0.5f);
    half.run(2.0f, 1.0f / 60.0f);
    CHECK(half.emitter.spawned() >= 99 && half.emitter.spawned() <= 100, "%llu spawns at half the rate",
          static_cast<unsigned long long>(half.emitter.spawned()));
}

QUENCH_TEST(particles_spawns_are_deterministic) {
    ParticleDesc desc;
    desc.shape = Shape::Sphere;
    desc.extent = glm::vec3(1.0f);
    desc.rate = 300.0f;
    desc.noise = 0.5f;
    desc.drag = 1.0f;
    desc.gravity = {0.0f, -2.0f, 0.0f};
    desc.seed = 42;
    Run a(desc), b(desc);
    a.run(1.0f, 1.0f / 60.0f);
    b.run(1.0f, 1.0f / 60.0f);
    REQUIRE(a.all.size() == b.all.size() && !a.all.empty(), "%zu and %zu spawns", a.all.size(), b.all.size());
    CHECK(std::memcmp(a.all.data(), b.all.data(), a.all.size() * sizeof(Particle)) == 0, "the same spawns");
    CHECK(std::memcmp(a.simulation.particles().data(), b.simulation.particles().data(),
                      a.simulation.capacity() * sizeof(Particle)) == 0,
          "the same simulation");

    // Spawn k has the same random numbers at any frame rate: its look, size and lifetime.
    Run c(desc);
    c.run(1.0f, 1.0f / 144.0f);
    const std::size_t n = std::min(a.all.size(), c.all.size());
    REQUIRE(n > 250, "%zu spawns", n);
    bool same = true;
    for (std::size_t k = 0; k < n; ++k) same = same && a.all[k].c == c.all[k].c && a.all[k].b.w == c.all[k].b.w;
    CHECK(same, "spawn k's random numbers depend on the frame rate");

    // Another seed, other particles.
    desc.seed = 43;
    Run d(desc);
    d.run(1.0f, 1.0f / 60.0f);
    CHECK(d.all.size() == a.all.size() && d.all[0].c != a.all[0].c, "another seed");
}

QUENCH_TEST(particles_spawns_fill_their_shape) {
    const glm::mat4 place = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, -5.0f));
    struct Case {
        Shape shape;
        bool surface;
        const char* name;
    };
    for (const Case& k :
         {Case{Shape::Sphere, false, "sphere"}, Case{Shape::Sphere, true, "sphere surface"},
          Case{Shape::Box, false, "box"}, Case{Shape::Disc, false, "disc"}, Case{Shape::Disc, true, "disc rim"}}) {
        ParticleDesc desc;
        desc.shape = k.shape;
        desc.surface = k.surface;
        desc.extent = {2.0f, 1.0f, 0.5f};
        desc.speed = {0.0f, 0.0f};
        desc.maxParticles = 2000;
        desc.rate = 0.0f;
        Run r(desc);
        r.emitter.setTransform(place, true);
        r.emitter.burst(2000);
        r.step(1.0f / 60.0f);
        REQUIRE(r.all.size() == 2000, "%s: %zu spawns", k.name, r.all.size());
        float most = 0.0f, least = 1e9f;
        bool inside = true;
        glm::vec3 mean(0.0f);
        for (const Particle& q : r.all) {
            const glm::vec3 p = position(q) - glm::vec3(10.0f, 0.0f, -5.0f);
            mean += p / 2000.0f;
            switch (k.shape) {
            case Shape::Sphere:
            case Shape::Disc:
                most = std::max(most, glm::length(p));
                least = std::min(least, glm::length(p));
                if (k.shape == Shape::Disc) inside = inside && std::abs(p.y) < 1e-5f;
                break;
            default: inside = inside && std::abs(p.x) <= 2.0f && std::abs(p.y) <= 1.0f && std::abs(p.z) <= 0.5f; break;
            }
        }
        CHECK(inside, "%s: a spawn outside the shape", k.name);
        CHECK(glm::length(mean) < 0.15f, "%s: mean %g %g %g (the shape's centre)", k.name, mean.x, mean.y, mean.z);
        if (k.shape != Shape::Box) {
            CHECK(most <= 2.0f + 1e-4f, "%s: %g from the centre", k.name, most);
            if (k.surface) CHECK(least >= 2.0f - 1e-4f, "%s: %g from the centre", k.name, least);
            if (!k.surface) CHECK(least < 0.5f, "%s: nothing near the centre (%g)", k.name, least);
        }
    }

    // A narrow cone sends every particle along its direction, turned by the transform.
    ParticleDesc cone;
    cone.direction = {0.0f, 0.0f, 1.0f};
    cone.spread = 0.05f;
    cone.speed = {3.0f, 3.0f};
    cone.rate = 0.0f;
    Run r(cone);
    r.emitter.setTransform(glm::rotate(glm::mat4(1.0f), 1.5707963f, glm::vec3(0.0f, 1.0f, 0.0f)), true);
    r.emitter.burst(100);
    r.step(0.0f);
    bool along = true;
    for (const Particle& q : r.all) along = along && glm::dot(velocity(q) / 3.0f, glm::vec3(1.0f, 0.0f, 0.0f)) > 0.99f;
    CHECK(along, "a cone of 0.05 rad around +z turned to +x");
}

QUENCH_TEST(particles_spawn_along_the_emitters_motion) {
    // 10 per metre over a 10 m move in one step: about 100 spawns spread evenly along it.
    ParticleDesc desc;
    desc.rate = 0.0f;
    desc.ratePerMeter = 10.0f;
    desc.speed = {0.0f, 0.0f};
    desc.maxParticles = 1000;
    Run r(desc);
    r.emitter.setTransform(glm::mat4(1.0f), true);
    r.emitter.setTransform(glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)));
    r.step(0.1f);
    REQUIRE(r.all.size() >= 99 && r.all.size() <= 100, "%zu spawns over 10 m at 10/m", r.all.size());
    std::vector<float> xs;
    for (const Particle& q : r.all) xs.push_back(q.a.x);
    std::sort(xs.begin(), xs.end());
    float gap = 0.0f;
    for (std::size_t i = 1; i < xs.size(); ++i) gap = std::max(gap, xs[i] - xs[i - 1]);
    CHECK(xs.front() < 0.2f && xs.back() > 9.8f && gap < 0.11f, "from %g to %g, largest gap %g", xs.front(), xs.back(),
          gap);
    // A teleport spawns nothing for the jump.
    r.all.clear();
    r.emitter.setTransform(glm::translate(glm::mat4(1.0f), glm::vec3(100.0f, 0.0f, 0.0f)), true);
    r.step(0.1f);
    CHECK(r.all.empty(), "%zu spawns for a teleport", r.all.size());

    // Spawns by rate on a moving emitter are placed along its motion too (older ones further back, and moved on).
    ParticleDesc moving;
    moving.rate = 600.0f;
    moving.speed = {0.0f, 0.0f};
    Run m(moving);
    m.emitter.setTransform(glm::mat4(1.0f), true);
    m.emitter.setTransform(glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
    m.step(0.1f);
    REQUIRE(m.all.size() == 60, "%zu spawns", m.all.size());
    CHECK(m.all.front().a.x < 0.05f && m.all.back().a.x > 0.95f, "first at %g, last at %g", m.all.front().a.x,
          m.all.back().a.x);
}

QUENCH_TEST(particles_ring_replaces_the_oldest) {
    ParticleDesc desc;
    desc.maxParticles = 8;
    desc.rate = 0.0f;
    desc.lifetime = {5.0f, 5.0f};
    Run r(desc);
    r.emitter.burst(5);
    r.step(0.1f);
    CHECK(r.uniforms.range.z == 0.0f && r.uniforms.range.w == 5.0f, "first step: slot %g, %g spawns",
          r.uniforms.range.z, r.uniforms.range.w);
    r.emitter.burst(20); // at most 8 at once
    r.step(0.1f);
    CHECK(r.uniforms.range.w == 8.0f, "%g spawns of a burst of 20 into 8 slots", r.uniforms.range.w);
    CHECK(r.emitter.spawned() == 13, "%llu spawned", static_cast<unsigned long long>(r.emitter.spawned()));
    CHECK(r.emitter.alive() == 8 && r.simulation.alive() == 8, "alive %u, %u", r.emitter.alive(), r.simulation.alive());
    // Everything dies after its lifetime; a restart empties the slots at the next step.
    r.run(6.0f, 0.5f);
    CHECK(r.emitter.alive() == 0 && r.simulation.alive() == 0, "alive after their lifetime %u, %u", r.emitter.alive(),
          r.simulation.alive());
    r.emitter.burst(3);
    r.step(0.1f);
    r.emitter.restart();
    r.step(0.1f);
    CHECK(r.uniforms.clock.w == 1.0f && r.simulation.alive() == 0, "restart: clock.w %g, alive %u", r.uniforms.clock.w,
          r.simulation.alive());
}

QUENCH_TEST(particles_motion_is_exact_without_air) {
    // Ballistic and with drag toward rest, stepped at different rates: the closed form, to float rounding.
    ParticleDesc desc;
    desc.gravity = {0.0f, -9.81f, 0.0f};
    for (const float drag : {0.0f, 1.5f}) {
        desc.drag = drag;
        const ParticleUniforms u = uniformsOf(desc);
        for (const float hz : {24.0f, 60.0f, 240.0f}) {
            Particle q{};
            q.a = glm::vec4(0.0f);
            q.b = glm::vec4(3.0f, 10.0f, 0.0f, 100.0f);
            const int n = int(std::lround(2.0f * hz));
            for (int i = 0; i < n; ++i) q = shader::quenchParticleStep(u, q, 1.0f / hz, float(i + 1) / hz);
            const float t = 2.0f;
            glm::vec3 want;
            if (drag == 0.0f) {
                want = glm::vec3(3.0f * t, 10.0f * t - 0.5f * 9.81f * t * t, 0.0f);
            } else {
                const glm::vec3 terminal(0.0f, -9.81f / drag, 0.0f);
                const glm::vec3 dv = glm::vec3(3.0f, 10.0f, 0.0f) - terminal;
                want = terminal * t + dv * ((1.0f - std::exp(-drag * t)) / drag);
            }
            const float error = glm::length(position(q) - want);
            CHECK(error < 2e-3f, "drag %g at %g Hz: %g m off after 2 s", drag, hz, error);
            CHECK(std::abs(q.a.w - 2.0f) < 1e-4f, "age %g", q.a.w);
        }
    }
}

QUENCH_TEST(particles_curl_noise_swirls_without_sinks) {
    // Divergence by central differences against the size of the gradient; the RMS speed per axis about 1.
    double divergence = 0.0, gradient = 0.0, square = 0.0;
    int n = 0;
    const float h = 1e-2f;
    for (int i = 0; i < 2000; ++i) {
        const glm::vec3 p(float(i % 13) * 0.71f - 4.0f, float((i / 13) % 11) * 0.53f - 3.0f, float(i / 143) * 0.37f);
        const float t = float(i % 7) * 0.9f;
        const auto f = [&](const glm::vec3& x) { return shader::quenchParticleCurl(x, t); };
        const float dx = (f(p + glm::vec3(h, 0, 0)).x - f(p - glm::vec3(h, 0, 0)).x) / (2.0f * h);
        const float dy = (f(p + glm::vec3(0, h, 0)).y - f(p - glm::vec3(0, h, 0)).y) / (2.0f * h);
        const float dz = (f(p + glm::vec3(0, 0, h)).z - f(p - glm::vec3(0, 0, h)).z) / (2.0f * h);
        divergence += std::abs(double(dx + dy + dz));
        gradient += std::abs(double(dx)) + std::abs(double(dy)) + std::abs(double(dz));
        const glm::vec3 v = f(p);
        square += double(glm::dot(v, v));
        ++n;
    }
    const double rms = std::sqrt(square / double(n) / 3.0);
    CHECK(divergence < gradient * 1e-3, "divergence %g against %g", divergence, gradient);
    CHECK(rms > 0.8 && rms < 1.25, "RMS per axis %g", rms);
}

QUENCH_TEST(particles_wind_carries_them) {
    // A wind of 10 m/s at the cloud base along +x without gusts: at 10 m up it is about 4.8 m/s (the log profile).
    ParticleDesc desc;
    desc.drag = 3.0f;
    desc.wind = 1.0f;
    ParticleUniforms u = uniformsOf(desc);
    u.wind.air = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    u.wind.speed = glm::vec4(10.0f, 1500.0f, 0.0f, 0.0f);
    Particle q{};
    q.a = glm::vec4(0.0f, 10.0f, 0.0f, 0.0f);
    q.b = glm::vec4(0.0f, 0.0f, 0.0f, 100.0f);
    for (int i = 0; i < 180; ++i) q = shader::quenchParticleStep(u, q, 1.0f / 60.0f, float(i + 1) / 60.0f);
    const float want = 10.0f * std::log(10.0f / 0.1f) / std::log(1500.0f / 0.1f);
    CHECK(std::abs(q.b.x - want) < 0.05f && std::abs(q.b.z) < 1e-4f, "velocity %g %g %g, wind %g", q.b.x, q.b.y, q.b.z,
          want);
}

QUENCH_TEST(particles_twinkle_keeps_the_mean) {
    ParticleDesc desc;
    desc.twinkle = 1.0f;
    desc.twinkleRate = 1.3f;
    for (const float sharpness : {1.0f, 4.0f, 12.0f}) {
        desc.twinkleSharpness = sharpness;
        const ParticleUniforms u = uniformsOf(desc);
        double sum = 0.0;
        float most = 0.0f;
        const int n = 20000;
        for (int i = 0; i < n; ++i) {
            const float b = shader::quenchParticleTwinkle(u, 0.37f, float(i) * 0.0137f);
            sum += double(b);
            most = std::max(most, b);
        }
        const double mean = sum / double(n);
        CHECK(std::abs(mean - 1.0) < 0.05, "sharpness %g: mean %g", sharpness, mean);
        CHECK(most > 1.8f, "sharpness %g: brightest %g", sharpness, most);
    }
}

QUENCH_TEST(particles_look_follows_the_life) {
    ParticleDesc desc;
    desc.size = {0.2f, 0.2f};
    desc.sizeOverLife = {{0.0f, 1.0f}, {1.0f, 0.0f}};
    desc.color = {4.0f, 2.0f, 1.0f};
    desc.colorOverLife = glm::vec4(1.0f);
    desc.lifetime = {1.0f, 1.0f};
    desc.rate = 0.0f;
    Run r(desc);
    r.emitter.burst(1);
    r.step(0.0f);
    r.run(0.5f, 0.05f);
    std::vector<ParticleInstance> look(r.simulation.capacity());
    r.simulation.look(r.uniforms, look);
    const ParticleInstance& i = look[0];
    CHECK(std::abs(i.a.w - 0.1f) < 2e-3f, "half size at half its life %g", i.a.w);
    CHECK(std::abs(i.b.x - 4.0f) < 1e-4f && std::abs(i.b.w - 1.0f) < 1e-4f, "color %g, opacity %g", i.b.x, i.b.w);
    CHECK(look[1].a.w == 0.0f, "an empty slot is drawn with half size %g", look[1].a.w);
    r.run(0.6f, 0.05f);
    r.simulation.look(r.uniforms, look);
    CHECK(look[0].a.w == 0.0f, "a dead particle is drawn with half size %g", look[0].a.w);
}

QUENCH_TEST(particles_path_passes_through_its_points) {
    const std::vector<glm::vec3> points = {{0, 0, 0}, {4, 0, 0}, {4, 0, 3}, {0, 1, 3}};
    for (const bool closed : {false, true}) {
        const Path path(points, closed);
        REQUIRE(path.length() > 10.0f, "length %g", path.length());
        for (const glm::vec3& p : points) {
            float nearest = 1e9f;
            for (int i = 0; i <= 4000; ++i)
                nearest = std::min(nearest, glm::length(path.at(path.length() * float(i) / 4000.0f) - p));
            CHECK(nearest < 0.01f, "closed %d: %g from a point", int(closed), nearest);
        }
        CHECK(glm::length(path.at(0.0f) - points[0]) < 1e-5f, "starts at its first point");
        if (closed) CHECK(glm::length(path.at(path.length()) - points[0]) < 1e-3f, "closes");
        if (!closed) CHECK(glm::length(path.at(1e6f) - points.back()) < 1e-3f, "ends at its last point");
        const glm::vec3 t = path.tangent(path.length() * 0.3f);
        CHECK(std::abs(glm::length(t) - 1.0f) < 1e-4f, "tangent length %g", glm::length(t));
    }
    // Spawns along a path stay within the radius around it.
    ParticleDesc desc;
    desc.shape = Shape::Path;
    desc.path = points;
    desc.extent = glm::vec3(0.1f);
    desc.rate = 0.0f;
    desc.speed = {0.0f, 0.0f};
    desc.maxParticles = 500;
    Run r(desc);
    r.emitter.burst(500);
    r.step(0.0f);
    const Path path(points);
    float worst = 0.0f;
    for (const Particle& q : r.all) {
        float nearest = 1e9f;
        for (int i = 0; i <= 2000; ++i)
            nearest = std::min(nearest, glm::length(path.at(path.length() * float(i) / 2000.0f) - position(q)));
        worst = std::max(worst, nearest);
    }
    CHECK(worst < 0.11f, "a spawn %g from the path", worst);
}

QUENCH_TEST(particles_trail_records_by_distance) {
    TrailDesc desc;
    desc.spacing = 0.1f;
    desc.lifetime = 10.0f;
    for (const float hz : {30.0f, 60.0f, 144.0f}) {
        Trail trail(desc);
        const int n = int(std::lround(hz));
        for (int i = 0; i <= n; ++i) trail.move({float(i) / hz, 0.0f, 0.0f}, double(i) / double(hz));
        CHECK(trail.points() >= 10 && trail.points() <= 12, "%g Hz: %u points over 1 m at 0.1 m", hz, trail.points());
    }
}

QUENCH_TEST(particles_trail_fades_out_behind_the_head) {
    TrailDesc desc;
    desc.spacing = 0.05f;
    desc.lifetime = 0.5f;
    desc.width = 0.2f;
    for (const float hz : {30.0f, 120.0f}) {
        Trail trail(desc);
        // A circle of 2 m radius at 2 m/s for 3 s.
        const auto at = [](double t) { return glm::vec3(2.0f * std::cos(float(t)), 1.0f, 2.0f * std::sin(float(t))); };
        const int n = int(std::lround(3.0f * hz));
        for (int i = 0; i <= n; ++i) trail.move(at(double(i) / double(hz)), double(i) / double(hz));
        std::vector<TrailVertex> v(trail.maxVertices());
        v.resize(trail.build(3.0, v));
        REQUIRE(v.size() >= 10, "%g Hz: %zu vertices", hz, v.size());
        // The head at its width, the tail cut where it is half a second old: about 1 m behind along the circle.
        CHECK(glm::length(glm::vec3(v.front().position) - at(3.0)) < 1e-5f, "%g Hz: the strip starts at the head", hz);
        CHECK(std::abs(v.front().position.w - 0.1f) < 1e-3f, "%g Hz: head half width %g", hz, v.front().position.w);
        CHECK(v.back().position.w < 1e-3f && v.back().color.w < 1e-3f, "%g Hz: tail %g wide, opacity %g", hz,
              v.back().position.w, v.back().color.w);
        const float length = stripLength(v);
        CHECK(std::abs(length - 1.0f) < 0.03f, "%g Hz: %g m long (1 m)", hz, length);
        CHECK(glm::length(glm::vec3(v.back().position) - at(2.5)) < 0.02f, "%g Hz: the tail's end is off by %g", hz,
              glm::length(glm::vec3(v.back().position) - at(2.5)));
        // Smooth: on the circle between the recorded points too.
        float off = 0.0f;
        for (const TrailVertex& x : v)
            off = std::max(off, std::abs(glm::length(glm::vec2(x.position.x, x.position.z)) - 2.0f));
        CHECK(off < 2e-3f, "%g Hz: %g m off the circle", hz, off);
    }
}

QUENCH_TEST(particles_trail_stops_and_breaks) {
    TrailDesc desc;
    desc.spacing = 0.05f;
    desc.lifetime = 0.5f;
    Trail trail(desc);
    for (int i = 0; i <= 30; ++i) trail.move({float(i) * 0.05f, 0.0f, 0.0f}, double(i) / 60.0);
    // A jump: the strip breaks there (a vertex below 0), both parts drawn.
    trail.move({10.0f, 0.0f, 0.0f}, 31.0 / 60.0, true);
    for (int i = 32; i <= 40; ++i) trail.move({10.0f + float(i - 31) * 0.05f, 0.0f, 0.0f}, double(i) / 60.0);
    std::vector<TrailVertex> v(trail.maxVertices());
    v.resize(trail.build(40.0 / 60.0, v));
    const auto breaks = std::count_if(v.begin(), v.end(), [](const TrailVertex& x) { return x.position.w < 0.0f; });
    CHECK(breaks == 1, "%zu breaks after a teleport", std::size_t(breaks));
    CHECK(v.front().position.x > 10.0f && v.back().position.x < 2.0f, "from %g to %g", v.front().position.x,
          v.back().position.x);
    // Stopped: nothing new, and it fades out within its lifetime.
    trail.setEmitting(false);
    trail.move({20.0f, 0.0f, 0.0f}, 41.0 / 60.0);
    v.assign(trail.maxVertices(), {});
    v.resize(trail.build(41.0 / 60.0, v));
    CHECK(!v.empty() && v.front().position.x < 11.0f, "after stopping: %zu vertices from %g", v.size(),
          v.empty() ? 0.0f : v.front().position.x);
    CHECK(!trail.visible(2.0) && trail.build(2.0, v) == 0, "faded out after its lifetime");
    // On again: a new strip from the next move, not joined to the old one.
    trail.setEmitting(true);
    trail.move({30.0f, 0.0f, 0.0f}, 2.0);
    trail.move({30.5f, 0.0f, 0.0f}, 2.1);
    v.assign(trail.maxVertices(), {});
    v.resize(trail.build(2.1, v));
    CHECK(!v.empty() && v.back().position.x >= 30.0f - 1e-4f, "the new strip ends at %g",
          v.empty() ? 0.0f : v.back().position.x);
}
