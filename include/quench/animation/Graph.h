#pragma once

// Animation graphs: data-driven state machines in JSON (the format is in docs/animation.md). A graph has parameters
// (floats, bools, triggers) that the game sets, and layers; each layer is a state machine whose states play a clip, a
// 1D blend tree or a 2D blend space, with transitions on conditions, triggers and exit times that cross-fade over a
// duration. Layers after the first override the pose on a joint mask, or add to it (additive clips).

#include <quench/core/Result.h>

#include <glm/vec2.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quench::animation {

inline constexpr int kMaxBlendPoints = 16; // clips of one blend tree or blend space

enum class ParameterType : std::uint8_t { Float, Bool, Trigger };

struct ParameterDef {
    std::string name;
    ParameterType type = ParameterType::Float;
    float initial = 0.0f; // bools and triggers: 0 or 1
};

enum class CompareOp : std::uint8_t { Greater, GreaterEqual, Less, LessEqual, Equal, NotEqual };

struct ConditionDef {
    int parameter = 0; // index into GraphDef::parameters
    CompareOp op = CompareOp::Greater;
    float value = 0.0f; // bools: 0 or 1
};

// A clip of a 1D blend tree at a parameter value. `phase` shifts the clip against the tree's normalized time (a clip
// whose left foot lands half a cycle later than in the others: 0.5).
struct BlendPoint {
    std::string clip;
    float at = 0.0f;
    float phase = 0.0f;
};

// A clip of a 2D blend space at a point of its two parameters (a velocity: x to the left, z forward).
struct BlendPoint2D {
    std::string clip;
    glm::vec2 at{0.0f};
    float phase = 0.0f;
};

struct StateDef {
    std::string name;
    // One clip, a blend tree over `blendParameter` (points sorted by `at`), or a blend space over
    // `blend2dParameters` (gradient band weights, blendSpaceWeights); none: an empty state (the rest pose in the first
    // layer, no contribution in the others). The clips of a tree or a space play at one normalized time (their phases
    // stay in step, so feet do not cross over), its duration the weighted mean of theirs.
    std::string clip;
    std::vector<BlendPoint> blend;
    int blendParameter = -1;
    std::vector<BlendPoint2D> blend2d;
    int blend2dParameters[2] = {-1, -1};
    float speed = 1.0f;       // playback rate
    int speedParameter = -1;  // >= 0: the rate is also multiplied by this parameter
    std::optional<bool> loop; // overrides the clip's own flag
};

struct TransitionDef {
    int from = -1; // index into the layer's states; -1: from any state (not to itself)
    int to = 0;
    std::vector<ConditionDef> conditions; // all must hold
    int trigger = -1;                     // >= 0: also needs the trigger, which the transition consumes
    float exitTime = -1.0f;               // >= 0: the source's normalized time must reach it (a loop: each cycle)
    float duration = 0.2f;                // cross-fade in seconds
};

enum class LayerBlend : std::uint8_t { Override, Additive };

struct LayerDef {
    std::string name;
    LayerBlend blend = LayerBlend::Override;
    std::vector<std::string> mask; // joints whose subtrees the layer affects; empty: every joint
    float weight = 1.0f;
    int weightParameter = -1; // >= 0: the weight is also multiplied by this parameter
    std::vector<StateDef> states;
    std::vector<TransitionDef> transitions; // checked in order, the first that holds starts
    int start = 0;
};

struct GraphDef {
    std::vector<ParameterDef> parameters;
    std::vector<LayerDef> layers; // at least one

    int findParameter(std::string_view name) const; // -1 when absent
    // The clip names the graph uses (for checks against clip sets).
    std::vector<std::string> clipNames() const;
};

// Checks the structure, names and references; the error names the layer, state or transition.
Result<GraphDef> parseGraph(std::string_view json);

// The weights of a 2D blend space's points at `p`, by gradient band interpolation (freeform Cartesian; R. S.
// Johansen, "Automated Semi-Procedural Animation for Character Locomotion", 2009, section 6.3): per point i the
// smallest over the other points j of 1 - dot(p - p_i, p_j - p_i) / |p_j - p_i|^2, clamped to 0..1, then normalized.
// They sum to 1, are 1 at a point's own position and 0 at the others', and change continuously with `p`; outside
// the points the nearest ones keep the weight. Points must be distinct; `weights` has one entry per point.
void blendSpaceWeights(std::span<const glm::vec2> points, glm::vec2 p, std::span<float> weights);

} // namespace quench::animation
