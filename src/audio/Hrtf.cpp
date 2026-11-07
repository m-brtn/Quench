#include "Hrtf.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace quench::audio::detail {
namespace {

#include "HrtfTable.inc"

constexpr int kSouthPole = kHrtfRings * kHrtfAzimuths;
constexpr int kNorthPole = kSouthPole + 1;
constexpr float kLastElevation = float(kHrtfFirstElevation + (kHrtfRings - 1) * kHrtfElevationStep);
const float kTurnCos = std::cos(0.5f * float(std::numbers::pi / 180.0)); // half a degree

float degrees(float radians) { return radians * float(180.0 / std::numbers::pi); }

// y[f] += sum over j of filter[j] * x[f + j], for f < frames: a direct convolution with a reversed filter, the
// outer loop over the taps so that the inner one has no dependency between iterations.
void convolve(const float* filter, std::uint32_t taps, const float* x, std::uint32_t frames, float* y) {
    for (std::uint32_t j = 0; j < taps; ++j) {
        const float c = filter[j];
        const float* in = x + j;
        for (std::uint32_t f = 0; f < frames; ++f) y[f] += c * in[f];
    }
}

} // namespace

Hrtf::Hrtf(std::uint32_t sampleRate) {
    const double ratio = double(kHrtfRate) / double(sampleRate); // table samples per output sample
    m_taps = std::uint32_t(std::ceil(double(kHrtfTaps) / ratio));
    m_irs.resize(std::size_t(kHrtfDirections) * 2 * m_taps);
    m_delays.resize(std::size_t(kHrtfDirections) * 2);
    for (std::size_t i = 0; i < std::size_t(kHrtfDirections) * 2; ++i) {
        const std::int16_t* source = kHrtfIr + i * kHrtfTaps;
        float* reversed = m_irs.data() + i * m_taps;
        for (std::uint32_t m = 0; m < m_taps; ++m) {
            // The response at this rate, with the amplitude scaled so that its gain stays the same.
            const double at = double(m) * ratio;
            const auto k = std::size_t(at);
            const double t = at - double(k);
            const double a = k < kHrtfTaps ? source[k] : 0.0;
            const double b = k + 1 < kHrtfTaps ? source[k + 1] : 0.0;
            reversed[m_taps - 1 - m] = float((a + (b - a) * t) * kHrtfScale * ratio);
        }
        m_delays[i] = float(double(kHrtfDelay[i]) / ratio);
        m_maxDelay = std::max(m_maxDelay, m_delays[i]);
    }
}

void Hrtf::lookup(const glm::vec3& direction, float* left, float* right, float delays[2]) const {
    const float elevation = degrees(std::asin(std::clamp(direction.y, -1.0f, 1.0f)));
    float azimuth = degrees(std::atan2(-direction.x, direction.z)); // 90 to the left
    if (azimuth < 0.0f) azimuth += 360.0f;

    // Up to four table directions: two on each of the rings around the elevation, or one ring and a pole.
    int index[4] = {};
    float weight[4] = {};
    int count = 0;
    const auto ring = [&](int r, float w) {
        const float a = azimuth / float(360 / kHrtfAzimuths);
        const auto a0 = int(a) % kHrtfAzimuths;
        const float t = a - std::floor(a);
        index[count] = r * kHrtfAzimuths + a0;
        weight[count++] = w * (1.0f - t);
        index[count] = r * kHrtfAzimuths + (a0 + 1) % kHrtfAzimuths;
        weight[count++] = w * t;
    };
    if (elevation <= float(kHrtfFirstElevation)) {
        const float t = (float(kHrtfFirstElevation) - elevation) / (float(kHrtfFirstElevation) + 90.0f);
        ring(0, 1.0f - t);
        index[count] = kSouthPole;
        weight[count++] = t;
    } else if (elevation >= kLastElevation) {
        const float t = (elevation - kLastElevation) / (90.0f - kLastElevation);
        ring(kHrtfRings - 1, 1.0f - t);
        index[count] = kNorthPole;
        weight[count++] = t;
    } else {
        const float e = (elevation - float(kHrtfFirstElevation)) / float(kHrtfElevationStep);
        const int r0 = std::min(int(e), kHrtfRings - 2);
        const float t = e - float(r0);
        ring(r0, 1.0f - t);
        ring(r0 + 1, t);
    }

    std::fill(left, left + m_taps, 0.0f);
    std::fill(right, right + m_taps, 0.0f);
    delays[0] = delays[1] = 0.0f;
    for (int i = 0; i < count; ++i) {
        if (weight[i] <= 0.0f) continue;
        const float w = weight[i];
        const float* irs = m_irs.data() + std::size_t(index[i]) * 2 * m_taps;
        for (std::uint32_t k = 0; k < m_taps; ++k) {
            left[k] += w * irs[k];
            right[k] += w * irs[m_taps + k];
        }
        delays[0] += w * m_delays[std::size_t(index[i]) * 2];
        delays[1] += w * m_delays[std::size_t(index[i]) * 2 + 1];
    }
}

HrtfDirection hrtfDirection(const Listener& listener, const Emitter& emitter) {
    HrtfDirection result;
    const glm::vec3 offset = emitter.position - listener.position;
    const float distance = glm::length(offset);
    glm::vec3 right = glm::cross(listener.forward, listener.up);
    const float rightLength = glm::length(right);
    const float forwardLength = glm::length(listener.forward);
    if (distance < 1e-4f || rightLength < 1e-6f || forwardLength < 1e-6f) return result;
    right /= rightLength;
    const glm::vec3 forward = listener.forward / forwardLength;
    const glm::vec3 up = glm::cross(right, forward);
    const glm::vec3 d = offset / distance;
    result.direction = glm::normalize(glm::vec3(glm::dot(d, right), glm::dot(d, up), glm::dot(d, forward)));
    result.weight = std::clamp(distance / std::max(emitter.minDistance, 1e-3f), 0.0f, 1.0f);
    return result;
}

HrtfFilter::HrtfFilter(const Hrtf& hrtf, std::uint32_t maxFrames)
    : m_hrtf(&hrtf), m_taps(hrtf.taps()), m_keep(std::uint32_t(std::ceil(hrtf.maxDelay())) + 2),
      m_maxFrames(maxFrames) {
    m_history.resize(std::size_t(m_keep) + maxFrames + 1);
    m_delayed.resize(2 * (std::size_t(m_taps) - 1 + maxFrames));
    m_filters.resize(4 * std::size_t(m_taps));
    m_scratch.resize(2 * std::size_t(maxFrames));
    m_input.resize(maxFrames);
}

void HrtfFilter::reset() {
    std::fill(m_history.begin(), m_history.end(), 0.0f);
    std::fill(m_delayed.begin(), m_delayed.end(), 0.0f);
    m_primed = false;
}

void HrtfFilter::process(const float* input, std::uint32_t frames, const glm::vec3& direction, float* out) {
    if (frames == 0) return;
    frames = std::min(frames, m_maxFrames);
    std::memcpy(m_history.data() + m_keep, input, frames * sizeof(float));

    float* current = m_filters.data();
    float* next = current + 2 * std::size_t(m_taps);
    float delay[2] = {m_delay[0], m_delay[1]};
    bool crossfade = false;
    if (!m_primed) {
        m_hrtf->lookup(direction, current, current + m_taps, m_delay);
        delay[0] = m_delay[0];
        delay[1] = m_delay[1];
        m_direction = direction;
        m_primed = true;
    } else if (glm::dot(direction, m_direction) < kTurnCos) {
        m_hrtf->lookup(direction, next, next + m_taps, delay);
        m_direction = direction;
        crossfade = true;
    }

    const float inv = 1.0f / float(frames);
    const std::size_t lane = std::size_t(m_taps) - 1 + m_maxFrames;
    for (int ear = 0; ear < 2; ++ear) {
        // The input delayed by the ear's delay, read between samples and ramped from the old delay to the new one.
        float* delayed = m_delayed.data() + ear * lane;
        float* block = delayed + (m_taps - 1);
        const float d0 = m_delay[ear], d1 = delay[ear];
        for (std::uint32_t f = 0; f < frames; ++f) {
            const float at = float(m_keep + f) - (d0 + (d1 - d0) * float(f + 1) * inv);
            const auto i = std::uint32_t(at);
            const float t = at - float(i);
            block[f] = m_history[i] + (m_history[i + 1] - m_history[i]) * t;
        }

        float* y = m_scratch.data();
        std::fill(y, y + frames, 0.0f);
        convolve(current + ear * m_taps, m_taps, delayed, frames, y);
        if (crossfade) {
            float* z = y + m_maxFrames;
            std::fill(z, z + frames, 0.0f);
            convolve(next + ear * m_taps, m_taps, delayed, frames, z);
            for (std::uint32_t f = 0; f < frames; ++f) y[f] += (z[f] - y[f]) * (float(f + 1) * inv);
        }
        for (std::uint32_t f = 0; f < frames; ++f) out[2 * f + ear] += y[f];
        std::memmove(delayed, delayed + frames, (m_taps - 1) * sizeof(float));
    }

    if (crossfade) std::copy(next, next + 2 * std::size_t(m_taps), current);
    m_delay[0] = delay[0];
    m_delay[1] = delay[1];
    std::memmove(m_history.data(), m_history.data() + frames, m_keep * sizeof(float));
}

void HrtfFilter::mix(const float* panned, std::uint32_t frames, const HrtfShare& share, const glm::vec3& direction,
                     float* out) {
    frames = std::min(frames, m_maxFrames);
    const float inv = 1.0f / float(frames);
    for (std::uint32_t f = 0; f < frames; ++f) {
        const float w = float(f + 1) * inv;
        const auto ramp = [w](const float (&v)[2]) { return v[0] + (v[1] - v[0]) * w; };
        // The voice's sample before panning: both channels are that sample times their gain.
        const float gains = ramp(share.left) + ramp(share.right);
        const float sample = gains > 1e-20f ? (panned[2 * f] + panned[2 * f + 1]) / gains : 0.0f;
        const float mix = ramp(share.mix);
        m_input[f] = sample * ramp(share.gain) * mix;
        out[2 * f] += panned[2 * f] * (1.0f - mix);
        out[2 * f + 1] += panned[2 * f + 1] * (1.0f - mix);
    }
    process(m_input.data(), frames, direction, out);
}

} // namespace quench::audio::detail
