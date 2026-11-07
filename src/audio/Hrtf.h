#pragma once

// Binaural rendering of mono voices: head-related impulse responses from the SADIE database (HrtfTable.inc, written by
// tools/make_hrtf_table.py), interpolated between the table's directions. Each ear has its own delay (fractional,
// ramped across a block) and its own filter, convolved directly (128 taps at 48 kHz; no FFT).

#include <quench/audio/Spatial.h>

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace quench::audio::detail {

// The table at the mix rate (resampled linearly from 48 kHz).
class Hrtf {
public:
    explicit Hrtf(std::uint32_t sampleRate);

    std::uint32_t taps() const { return m_taps; }
    float maxDelay() const { return m_maxDelay; } // samples
    // The impulse responses of both ears for a direction in the listener's frame (x right, y up, z forward; unit
    // length), each reversed (its last tap first), and the ears' delays in samples.
    void lookup(const glm::vec3& direction, float* left, float* right, float delays[2]) const;

private:
    std::uint32_t m_taps = 0;
    float m_maxDelay = 0.0f;
    std::vector<float> m_irs;    // per direction and ear, reversed
    std::vector<float> m_delays; // per direction and ear
};

// The direction from the listener to an emitter in the listener's frame (x right, y up, z forward), and the share of
// the HRTF in its sound: 1 outside the emitter's minDistance, falling to 0 at the listener, where panning has
// narrowed to the center (spatialize()).
struct HrtfDirection {
    glm::vec3 direction{0.0f, 0.0f, 1.0f};
    float weight = 0.0f;
};
HrtfDirection hrtfDirection(const Listener& listener, const Emitter& emitter);

// How a block of a voice is split between panning and the HRTF; each value ramps linearly across the block from its
// first entry to its second.
struct HrtfShare {
    float left[2] = {}, right[2] = {}; // the panned gains
    float mix[2] = {};                 // the HRTF's share, 0..1
    float gain[2] = {};                // the volume of the HRTF's input
};

// One voice's state: its input history, each ear's delayed input and the current filters. Everything is allocated by
// the constructor.
class HrtfFilter {
public:
    HrtfFilter(const Hrtf& hrtf, std::uint32_t maxFrames);

    // For a new voice: clears the history; the next process() starts with the filters of its direction.
    void reset();
    // Filters `frames` (at most maxFrames) mono samples heard from `direction` (unit length, the listener's frame)
    // and adds them to `out` (interleaved stereo). A direction that moved by more than half a degree crossfades from
    // the old filters to the new ones over the block.
    void process(const float* input, std::uint32_t frames, const glm::vec3& direction, float* out);
    // A mono voice's block as the mixer panned it (interleaved stereo, `share.left` and `share.right` applied): adds
    // its panned share (1 - mix) to `out` and filters the rest.
    void mix(const float* panned, std::uint32_t frames, const HrtfShare& share, const glm::vec3& direction, float* out);

private:
    const Hrtf* m_hrtf;
    std::uint32_t m_taps = 0;
    std::uint32_t m_keep = 0;     // input history kept for the delays
    std::vector<float> m_history; // m_keep + maxFrames samples and a zero
    std::vector<float> m_delayed; // per ear: taps - 1 samples of the past, then the block
    std::vector<float> m_filters; // [current, next] x [left, right] x taps, reversed
    std::vector<float> m_scratch; // the outputs of the current and next filters for a block
    std::vector<float> m_input;   // mix()'s input for process()
    std::uint32_t m_maxFrames = 0;
    float m_delay[2] = {};
    glm::vec3 m_direction{0.0f, 0.0f, 1.0f};
    bool m_primed = false;
};

} // namespace quench::audio::detail
