#include <quench/audio/VoiceStream.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace quench::audio {

VoiceStream::VoiceStream(const VoiceStreamDesc& desc) : Generator(Format{1, desc.sampleRate}), m_desc(desc) {
    const double rate = double(std::max<std::uint32_t>(desc.sampleRate, 1));
    m_capacity = std::max<std::uint32_t>(1024, std::uint32_t(std::max(1.0, desc.capacitySeconds) * rate));
    m_prebuffer = std::uint32_t(std::max(0.0, desc.prebufferSeconds) * rate);
    m_fade = std::max<std::uint32_t>(1, std::uint32_t(std::max(0.0, desc.fadeSeconds) * rate));
    m_ring.resize(m_capacity);
}

VoiceStream::~VoiceStream() = default;

std::uint32_t VoiceStream::push(std::span<const float> samples) {
    const std::uint64_t write = m_write.load(std::memory_order_relaxed);
    const std::uint64_t read = m_read.load(std::memory_order_acquire);
    const auto room = std::uint32_t(m_capacity - (write - read));
    const auto n = std::uint32_t(std::min<std::size_t>(samples.size(), room));
    for (std::uint32_t done = 0; done < n;) {
        const auto offset = std::uint32_t((write + done) % m_capacity);
        const std::uint32_t piece = std::min(n - done, m_capacity - offset);
        std::memcpy(m_ring.data() + offset, samples.data() + done, piece * sizeof(float));
        done += piece;
    }
    m_write.store(write + n, std::memory_order_release);
    m_droppedPush += samples.size() - n;
    return n;
}

void VoiceStream::flush() {
    m_flushTo.store(m_write.load(std::memory_order_relaxed), std::memory_order_release);
    ++m_flushes;
}

double VoiceStream::queuedSeconds() const {
    const std::uint64_t write = m_write.load(std::memory_order_acquire);
    const std::uint64_t from =
        std::max(m_read.load(std::memory_order_acquire), m_flushTo.load(std::memory_order_acquire));
    return write > from ? double(write - from) / double(m_desc.sampleRate) : 0.0;
}

bool VoiceStream::active() const { return queuedSeconds() > 0.0 || m_playing.load(std::memory_order_relaxed); }

float VoiceStream::level() const { return std::bit_cast<float>(m_levelBits.load(std::memory_order_relaxed)); }

bool VoiceStream::audible() const { return level() > m_desc.audibleLevel; }

double VoiceStream::playedSeconds() const { return double(playedFrames()) / double(m_desc.sampleRate); }

// Copies up to `frames` queued samples (at most up to `limit`, a ring position) to `out`; returns how many.
std::uint32_t VoiceStream::take(float* out, std::uint32_t frames, std::uint64_t limit) {
    const std::uint64_t read = m_read.load(std::memory_order_relaxed);
    const auto n = std::uint32_t(std::min<std::uint64_t>(frames, limit > read ? limit - read : 0));
    for (std::uint32_t done = 0; done < n;) {
        const auto offset = std::uint32_t((read + done) % m_capacity);
        const std::uint32_t piece = std::min(n - done, m_capacity - offset);
        std::memcpy(out + done, m_ring.data() + offset, piece * sizeof(float));
        done += piece;
    }
    m_read.store(read + n, std::memory_order_release);
    return n;
}

std::uint32_t VoiceStream::generate(float* out, std::uint32_t frames) {
    std::uint32_t done = 0;
    const std::uint64_t write = m_write.load(std::memory_order_acquire);
    const std::uint64_t flushTo = m_flushTo.load(std::memory_order_acquire);

    if (flushTo > m_read.load(std::memory_order_relaxed)) {
        // A flush: what plays fades out over the next fade's samples (those queued before the flush), the rest of
        // what was queued before it is dropped.
        if (m_mode == Mode::Playing) {
            const std::uint32_t n = take(out, std::min(frames, m_fade), flushTo);
            for (std::uint32_t i = 0; i < n; ++i) out[i] *= 1.0f - float(i + 1) / float(n);
            if (n == 0 && m_last != 0.0f) {
                // Nothing left to fade with: the last sample falls to zero instead.
                const std::uint32_t m = std::min(frames, m_fade);
                for (std::uint32_t i = 0; i < m; ++i) out[i] = m_last * (1.0f - float(i + 1) / float(m));
                done = m;
            } else {
                done = n;
            }
        }
        m_read.store(std::max(m_read.load(std::memory_order_relaxed), flushTo), std::memory_order_release);
        m_mode = Mode::Waiting;
        m_waited = 0;
        m_last = 0.0f;
        m_playing.store(false, std::memory_order_relaxed);
    }

    while (done < frames) {
        const std::uint64_t read = m_read.load(std::memory_order_relaxed);
        const std::uint64_t queued = write > read ? write - read : 0;
        if (m_mode == Mode::Waiting) {
            if (queued == 0) {
                std::memset(out + done, 0, (frames - done) * sizeof(float));
                m_waited = 0;
                done = frames;
                break;
            }
            if (queued >= m_prebuffer || m_waited >= m_prebuffer) {
                m_mode = Mode::Playing;
                m_rampIn = m_fade;
                m_playing.store(true, std::memory_order_relaxed);
                continue;
            }
            const std::uint32_t n = std::min(frames - done, m_prebuffer - m_waited);
            std::memset(out + done, 0, n * sizeof(float));
            m_waited += n;
            done += n;
            continue;
        }
        if (queued == 0) {
            // An underrun: the last sample falls to zero instead of stepping there.
            const std::uint32_t n = std::min(frames - done, m_fade);
            for (std::uint32_t i = 0; i < n; ++i) out[done + i] = m_last * (1.0f - float(i + 1) / float(n));
            done += n;
            m_underruns.fetch_add(1, std::memory_order_relaxed);
            m_mode = Mode::Waiting;
            m_waited = 0;
            m_last = 0.0f;
            m_playing.store(false, std::memory_order_relaxed);
            continue;
        }
        const std::uint32_t n = take(out + done, frames - done, write);
        for (std::uint32_t i = 0; i < n && m_rampIn > 0; ++i, --m_rampIn)
            out[done + i] *= float(m_fade - m_rampIn + 1) / float(m_fade + 1);
        m_last = out[done + n - 1];
        m_played.fetch_add(n, std::memory_order_relaxed);
        done += n;
    }

    float peak = 0.0f;
    for (std::uint32_t i = 0; i < frames; ++i) peak = std::max(peak, std::abs(out[i]));
    const float decay = float(std::pow(0.5, double(frames) / double(m_desc.sampleRate) / 0.1));
    m_level = std::min(1.0f, std::max(peak, m_level * decay));
    m_levelBits.store(std::bit_cast<std::uint32_t>(m_level), std::memory_order_relaxed);
    return frames;
}

} // namespace quench::audio
