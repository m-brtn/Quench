#pragma once

// Lock-free single-producer single-consumer queues between the game thread and the audio thread. Capacity is fixed at
// construction (the only allocation); push and pop never block.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace quench::audio::detail {

template <class T>
class SpscQueue {
public:
    explicit SpscQueue(std::size_t capacity)
        : m_items(std::bit_ceil(std::max<std::size_t>(capacity, 2))), m_mask(m_items.size() - 1) {}

    // Producer: false when full.
    bool push(const T& item) {
        const std::size_t write = m_write.load(std::memory_order_relaxed);
        if (write - m_read.load(std::memory_order_acquire) == m_items.size()) return false;
        m_items[write & m_mask] = item;
        m_write.store(write + 1, std::memory_order_release);
        return true;
    }

    // Consumer: false when empty.
    bool pop(T& item) {
        const std::size_t read = m_read.load(std::memory_order_relaxed);
        if (read == m_write.load(std::memory_order_acquire)) return false;
        item = m_items[read & m_mask];
        m_read.store(read + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const { return m_items.size(); }
    // Either side; exact only on the producer's or consumer's own thread for its own bound.
    std::size_t size() const {
        return m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire);
    }

private:
    std::vector<T> m_items;
    std::size_t m_mask;
    alignas(64) std::atomic<std::size_t> m_write{0};
    alignas(64) std::atomic<std::size_t> m_read{0};
};

// Interleaved float frames. The producer writes into the free space in up to two contiguous pieces (decoders write
// straight into the ring), the consumer copies out.
class SampleRing {
public:
    SampleRing(std::uint32_t channels, std::uint32_t capacityFrames)
        : m_channels(channels), m_capacity(capacityFrames), m_samples(std::size_t(capacityFrames) * channels) {}

    std::uint32_t channels() const { return m_channels; }
    std::uint32_t capacity() const { return m_capacity; }

    // Either side.
    std::uint32_t available() const {
        return std::uint32_t(m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire));
    }

    // Producer: frames that fit now.
    std::uint32_t writable() const { return m_capacity - available(); }
    // Producer: the contiguous free space at the write position (at most `frames`); fill it, then commit().
    float* writeSpan(std::uint32_t frames, std::uint32_t& granted) {
        const std::uint64_t write = m_write.load(std::memory_order_relaxed);
        const std::uint32_t offset = std::uint32_t(write % m_capacity);
        granted = std::min({frames, writable(), m_capacity - offset});
        return m_samples.data() + std::size_t(offset) * m_channels;
    }
    void commit(std::uint32_t frames) {
        m_write.store(m_write.load(std::memory_order_relaxed) + frames, std::memory_order_release);
    }
    // Producer: copies as many frames as fit.
    std::uint32_t write(const float* frames, std::uint32_t count) {
        std::uint32_t done = 0;
        while (done < count) {
            std::uint32_t granted = 0;
            float* dst = writeSpan(count - done, granted);
            if (granted == 0) break;
            std::memcpy(dst, frames + std::size_t(done) * m_channels,
                        std::size_t(granted) * m_channels * sizeof(float));
            commit(granted);
            done += granted;
        }
        return done;
    }

    // Consumer: copies up to `count` frames out.
    std::uint32_t read(float* out, std::uint32_t count) {
        const std::uint64_t read = m_read.load(std::memory_order_relaxed);
        const std::uint32_t n = std::min(count, std::uint32_t(m_write.load(std::memory_order_acquire) - read));
        const std::uint32_t offset = std::uint32_t(read % m_capacity);
        const std::uint32_t first = std::min(n, m_capacity - offset);
        const std::size_t frameBytes = std::size_t(m_channels) * sizeof(float);
        std::memcpy(out, m_samples.data() + std::size_t(offset) * m_channels, first * frameBytes);
        if (n > first) std::memcpy(out + std::size_t(first) * m_channels, m_samples.data(), (n - first) * frameBytes);
        m_read.store(read + n, std::memory_order_release);
        return n;
    }

private:
    std::uint32_t m_channels;
    std::uint32_t m_capacity;
    std::vector<float> m_samples;
    alignas(64) std::atomic<std::uint64_t> m_write{0};
    alignas(64) std::atomic<std::uint64_t> m_read{0};
};

} // namespace quench::audio::detail
