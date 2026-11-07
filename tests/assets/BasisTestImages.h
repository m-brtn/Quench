#pragma once

// Procedural images for the Basis Universal cases: the cook's cases encode them (and write the files in assets/data),
// the assets cases compare what those files transcode to with them.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace quench::test {

struct BasisTestImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

inline std::uint8_t toByte(double v) { return std::uint8_t(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

// Color: smooth gradients, soft spots and thin lines, opaque.
inline BasisTestImage basisColorImage(std::uint32_t width, std::uint32_t height) {
    BasisTestImage image{width, height, {}};
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            const double u = (x + 0.5) / width, v = (y + 0.5) / height;
            const double spot = 0.5 + 0.5 * std::sin(u * 19.0) * std::cos(v * 13.0);
            const bool line = (x % 16 == 3) || (y % 23 == 7);
            image.rgba.insert(image.rgba.end(), {toByte(line ? 0.9 : 0.15 + 0.6 * u * spot),
                                                 toByte(line ? 0.85 : 0.2 + 0.5 * v), toByte(0.25 + 0.5 * spot), 255});
        }
    return image;
}

// Color with alpha: the color image with an alpha ramp and a hard-edged hole.
inline BasisTestImage basisAlphaImage(std::uint32_t width, std::uint32_t height) {
    BasisTestImage image = basisColorImage(width, height);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            const double dx = x + 0.5 - width * 0.5, dy = y + 0.5 - height * 0.5;
            const bool hole = dx * dx + dy * dy < width * height * 0.04;
            image.rgba[(std::size_t(y) * width + x) * 4 + 3] = hole ? 0 : toByte((x + 0.5) / width);
        }
    return image;
}

// One color, opaque (for the GPU cases, which check pixels by color).
inline BasisTestImage basisSolidImage(std::uint32_t width, std::uint32_t height, std::uint8_t r, std::uint8_t g,
                                      std::uint8_t b) {
    BasisTestImage image{width, height, {}};
    for (std::uint32_t i = 0; i < width * height; ++i) image.rgba.insert(image.rgba.end(), {r, g, b, 255});
    return image;
}

// A tangent-space normal map (+Z out of the surface) of round bumps, linear.
inline BasisTestImage basisNormalImage(std::uint32_t width, std::uint32_t height) {
    BasisTestImage image{width, height, {}};
    constexpr double kPi = 3.14159265358979323846;
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            // The slopes of h = 0.15 sin(4 pi u) sin(4 pi v) + 0.05 sin(22 u + 9 v).
            const double u = (x + 0.5) / width, v = (y + 0.5) / height;
            const double du =
                0.15 * 4 * kPi * std::cos(4 * kPi * u) * std::sin(4 * kPi * v) + 0.05 * 22 * std::cos(22 * u + 9 * v);
            const double dv =
                0.15 * 4 * kPi * std::sin(4 * kPi * u) * std::cos(4 * kPi * v) + 0.05 * 9 * std::cos(22 * u + 9 * v);
            const double length = std::sqrt(du * du + dv * dv + 1.0);
            image.rgba.insert(image.rgba.end(), {toByte(0.5 - 0.5 * du / length), toByte(0.5 - 0.5 * dv / length),
                                                 toByte(0.5 + 0.5 / length), 255});
        }
    return image;
}

// The Basis Universal files in assets/data (embedded in the assets cases): their images and settings. The cook's case
// cook_basis_test_data checks them against a fresh encode and writes them again with QUENCH_UPDATE_TEST_DATA=1.
struct BasisTestFile {
    const char* name;
    BasisTestImage image;
    bool srgb, alpha, normals, uastc;
    // The least quality of its RGBA8 transcode, about 2 dB (or degrees) under the measured one: PSNR over RGB in dB,
    // or with `normals` the largest mean normal error in degrees.
    double floor;
};
inline std::vector<BasisTestFile> basisTestFiles() {
    return {
        {"color_etc1s.ktx2", basisColorImage(64, 64), true, false, false, false, 19.0},
        {"alpha_etc1s.ktx2", basisAlphaImage(64, 64), true, true, false, false, 19.0},
        {"color_uastc.ktx2", basisColorImage(64, 64), true, false, false, true, 40.0},
        {"alpha_uastc.ktx2", basisAlphaImage(64, 64), true, true, false, true, 37.0},
        {"normals_uastc.ktx2", basisNormalImage(64, 64), false, false, true, true, 5.0},
        {"odd_uastc.ktx2", basisColorImage(6, 5), true, false, false, true, 24.0},
        {"green_etc1s.ktx2", basisSolidImage(16, 16, 20, 200, 20), true, false, false, false, 30.0},
        {"blue_uastc.ktx2", basisSolidImage(16, 16, 20, 20, 200), true, false, false, true, 30.0},
    };
}

// Peak signal to noise ratio of two RGBA8 images over their first `channels` channels, in dB (99 when the same).
inline double basisPsnr(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b, int channels = 3) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (int(i % 4) >= channels) continue;
        const double d = double(a[i]) - double(b[i]);
        sum += d * d;
        ++count;
    }
    return sum == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / (sum / double(count)));
}

// The mean and the largest angle between the normals of two normal maps (RGB), in degrees.
inline std::pair<double, double> basisNormalError(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
    if (a.size() != b.size() || a.empty()) return {180.0, 180.0};
    double sum = 0.0, largest = 0.0;
    for (std::size_t i = 0; i + 3 < a.size(); i += 4) {
        double na[3], nb[3], la = 0.0, lb = 0.0, dot = 0.0;
        for (int c = 0; c < 3; ++c) {
            na[c] = a[i + c] / 127.5 - 1.0;
            nb[c] = b[i + c] / 127.5 - 1.0;
            la += na[c] * na[c];
            lb += nb[c] * nb[c];
        }
        for (int c = 0; c < 3; ++c) dot += na[c] * nb[c];
        const double angle = std::acos(std::clamp(dot / std::sqrt(std::max(la * lb, 1e-12)), -1.0, 1.0)) * 180.0 /
                             3.14159265358979323846;
        sum += angle;
        largest = std::max(largest, angle);
    }
    return {sum / double(a.size() / 4), largest};
}

} // namespace quench::test
