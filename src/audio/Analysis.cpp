#include <quench/audio/Analysis.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace quench::audio {
namespace {

// In-place iterative radix-2 FFT; data.size() is a power of two.
void fft(std::vector<std::complex<double>>& data) {
    const std::size_t n = data.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const double angle = -2.0 * std::numbers::pi / double(length);
        const std::complex<double> unit(std::cos(angle), std::sin(angle));
        for (std::size_t start = 0; start < n; start += length) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < length / 2; ++k) {
                const std::complex<double> a = data[start + k];
                const std::complex<double> b = data[start + k + length / 2] * w;
                data[start + k] = a + b;
                data[start + k + length / 2] = a - b;
                w *= unit;
            }
        }
    }
}

} // namespace

float Levels::rmsMono() const { return std::sqrt(0.5f * (rms[0] * rms[0] + rms[1] * rms[1])); }

Levels measureLevels(std::span<const float> samples, std::uint32_t channels) {
    Levels levels;
    if (channels == 0) return levels;
    levels.frames = samples.size() / channels;
    if (levels.frames == 0) return levels;
    double sum[2] = {0.0, 0.0};
    for (std::uint64_t f = 0; f < levels.frames; ++f) {
        for (std::uint32_t c = 0; c < std::min(channels, 2u); ++c) {
            const float s = samples[f * channels + c];
            sum[c] += double(s) * s;
            levels.peak[c] = std::max(levels.peak[c], std::abs(s));
        }
    }
    levels.rms[0] = float(std::sqrt(sum[0] / double(levels.frames)));
    levels.rms[1] = channels > 1 ? float(std::sqrt(sum[1] / double(levels.frames))) : levels.rms[0];
    if (channels == 1) levels.peak[1] = levels.peak[0];
    return levels;
}

Peak dominantFrequency(std::span<const float> samples, std::uint32_t channels, std::uint32_t sampleRate, float minHz,
                       float maxHz) {
    Peak peak;
    if (channels == 0 || sampleRate == 0) return peak;
    const std::size_t frames = samples.size() / channels;
    if (frames < 256) return peak;
    const std::size_t n = std::min<std::size_t>(std::bit_floor(frames), 65536);
    const std::size_t first = frames - n;

    std::vector<std::complex<double>> data(n);
    for (std::size_t i = 0; i < n; ++i) {
        double sum = 0.0;
        for (std::uint32_t c = 0; c < channels; ++c) sum += samples[(first + i) * channels + c];
        const double window = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(n));
        data[i] = sum / channels * window;
    }
    fft(data);

    const double binHz = double(sampleRate) / double(n);
    const auto lo = std::max<std::size_t>(1, std::size_t(std::ceil(minHz / binHz)));
    const auto hi = std::min<std::size_t>(n / 2 - 2, std::size_t(std::floor(maxHz / binHz)));
    if (lo > hi) return peak;
    std::size_t best = lo;
    for (std::size_t k = lo; k <= hi; ++k)
        if (std::abs(data[k]) > std::abs(data[best])) best = k;
    const double magnitude = std::abs(data[best]);
    if (magnitude < 1e-9 * double(n)) return peak;

    // Parabola through the log magnitudes around the peak (close to exact for the Hann window's main lobe).
    double offset = 0.0;
    if (best > 0) {
        const double a = std::log(std::abs(data[best - 1]) + 1e-30), b = std::log(magnitude),
                     c = std::log(std::abs(data[best + 1]) + 1e-30);
        const double denominator = a - 2.0 * b + c;
        if (std::abs(denominator) > 1e-12) offset = std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5);
    }
    peak.frequency = float((double(best) + offset) * binHz);
    // The Hann window's response `offset` bins off its center: sinc(offset) / (1 - offset²).
    const double x = std::numbers::pi * offset;
    const double response = std::abs(offset) < 1e-6 ? 1.0 : std::sin(x) / x / (1.0 - offset * offset);
    peak.level = float(magnitude * 4.0 / double(n) / response);
    return peak;
}

} // namespace quench::audio
