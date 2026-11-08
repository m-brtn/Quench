#pragma once

// A trail behind a moving point (docs/particles.md, "Trails"): the head records a point wherever it has moved
// `spacing` m, the points age and fade, and the strip through them is a smooth curve (centripetal Catmull-Rom)
// that gets thinner and fainter toward the tail, cut exactly where its oldest part reaches the end of its life.
// Everything follows the times the game gives (move), so a trail looks the same at any frame rate.

#include <quench/particles/Particles.h>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace quench::particles {

struct TrailDesc {
    float lifetime = 1.5f;          // s: a point fades out over this long after the head left it
    float spacing = 0.05f;          // m: the head records a point where it has moved this far
    std::uint32_t maxPoints = 256;  // the most recorded points (the oldest are dropped)
    float maxLength = 0.0f;         // m: the strip ends this far behind the head (0: by age only)
    std::uint32_t subdivisions = 4; // strip vertices per recorded segment at most (fewer on short segments)
    // Width (m, across the strip) and color (linear HDR, times the curve's rgb) over t = max(age / lifetime,
    // length / maxLength): 0 at the head, 1 at the end of the tail.
    float width = 0.1f;
    FloatCurve widthOverAge = {{0.0f, 1.0f}, {1.0f, 0.0f}};
    glm::vec3 color{1.0f};
    float opacity = 1.0f;
    ColorCurve colorOverAge = {{0.0f, glm::vec4(1.0f)}, {1.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f)}};
    // A glow at the head while it emits (half its side, m; 0: none), additive.
    float headSize = 0.0f;
    glm::vec3 headColor{1.0f};
    Blend blend = Blend::Additive;
    float softness = 0.2f;  // m: fades out where it nears the scene behind it
    float minPixels = 1.0f; // drawn at least this wide on the screen, fainter by its width below that
};

// One vertex of a trail's strip: what the renderer uploads (two float4).
struct TrailVertex {
    glm::vec4 position; // xyz (m), w: half the width (m); below 0: the strip breaks before this vertex
    glm::vec4 color;    // rgb (linear HDR), a: opacity
};

class Trail {
public:
    explicit Trail(const TrailDesc& desc = {});

    const TrailDesc& desc() const { return m_desc; }
    void setDesc(const TrailDesc& desc); // a new maxPoints keeps the newest points

    // The head is at `head` at `time` (s on a clock that does not go back: the game's time). teleport: a jump, the
    // strip breaks there.
    void move(const glm::vec3& head, double time, bool teleport = false);
    // Off: the head stops recording and its glow goes out, the strip fades out where it is; on again, a new strip
    // starts at the next move.
    void setEmitting(bool on);
    bool emitting() const { return m_emitting; }
    void clear();

    const glm::vec3& head() const { return m_head; }
    double time() const { return m_time; }
    bool started() const { return m_started; }
    // Recorded points (some may have faded).
    std::uint32_t points() const { return m_count; }
    // An upper bound of build()'s count.
    std::uint32_t maxVertices() const;
    // Whether build() writes anything at `time`.
    bool visible(double time) const;

    // The strip at `time`: from the head (while emitting) to the end of the tail, smoothed; writes at most
    // out.size() vertices and returns how many.
    std::uint32_t build(double time, std::span<TrailVertex> out) const;

private:
    struct Point {
        glm::vec3 position;
        double time;
        std::uint32_t run; // points of one run form one strip
    };
    const Point& point(std::uint32_t newest) const; // 0: the newest
    void push(const Point& p);
    void expire();

    TrailDesc m_desc;
    std::vector<Point> m_ring;
    std::uint32_t m_first = 0; // the oldest point's index in m_ring
    std::uint32_t m_count = 0;
    glm::vec3 m_head{0.0f};
    double m_time = 0.0;
    std::uint32_t m_run = 0;
    bool m_emitting = true;
    bool m_started = false; // the head has been placed in this run
};

} // namespace quench::particles
