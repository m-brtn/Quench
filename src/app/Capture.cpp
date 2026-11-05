#include "AppState.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <vector>

namespace quench::app::detail {

namespace {

float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = std::uint32_t(h & 0x8000u) << 16;
    const std::uint32_t exponent = (h >> 10) & 0x1fu;
    const std::uint32_t mantissa = h & 0x3ffu;
    if (exponent == 0) {
        const float value = std::ldexp(float(mantissa), -24);
        return sign ? -value : value;
    }
    if (exponent == 31) return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13));
    return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

} // namespace

Result<double> writePng(std::string_view path, std::span<const std::byte> pixels, std::uint32_t width,
                        std::uint32_t height, gpu::TextureFormat format, float paperWhite) {
    const bool bgra = format == gpu::TextureFormat::Bgra8Unorm || format == gpu::TextureFormat::Bgra8UnormSrgb;
    const bool rgba = format == gpu::TextureFormat::Rgba8Unorm || format == gpu::TextureFormat::Rgba8UnormSrgb;
    const bool half = format == gpu::TextureFormat::Rgba16Float;
    if (!bgra && !rgba && !half)
        return Error{std::format("captures of {} are not supported", gpu::formatInfo(format).name)};
    const std::size_t count = std::size_t(width) * height;
    if (pixels.size() < count * (half ? 8 : 4)) return Error{"capture: too few pixels"};

    // RGBA, opaque: captures compare equal whatever alpha the frame left in the target.
    std::vector<std::uint8_t> rgbaPixels(count * 4);
    double peak = 0.0;
    if (half) {
        // Linear values with paper white at `paperWhite`: clipped at paper white and encoded with gamma 2.2, as the
        // renderer's SDR path writes display values.
        std::vector<std::uint8_t> encode(4097);
        for (std::size_t i = 0; i < encode.size(); ++i)
            encode[i] = std::uint8_t(std::lround(std::pow(double(i) / 4096.0, 1.0 / 2.2) * 255.0));
        const float white = std::max(paperWhite, 1e-3f);
        for (std::size_t i = 0; i < count; ++i) {
            for (int c = 0; c < 3; ++c) {
                std::uint16_t bits;
                std::memcpy(&bits, pixels.data() + i * 8 + std::size_t(c) * 2, 2);
                float v = halfToFloat(bits) / white;
                if (!(v >= 0.0f)) v = 0.0f; // also NaN
                peak = std::max(peak, double(v));
                rgbaPixels[i * 4 + std::size_t(c)] = encode[std::size_t(std::lround(std::min(v, 1.0f) * 4096.0f))];
            }
            rgbaPixels[i * 4 + 3] = 255;
        }
    } else {
        const auto* in = reinterpret_cast<const std::uint8_t*>(pixels.data());
        std::uint8_t brightest = 0;
        for (std::size_t i = 0; i < count; ++i) {
            rgbaPixels[i * 4 + 0] = in[i * 4 + (bgra ? 2 : 0)];
            rgbaPixels[i * 4 + 1] = in[i * 4 + 1];
            rgbaPixels[i * 4 + 2] = in[i * 4 + (bgra ? 0 : 2)];
            rgbaPixels[i * 4 + 3] = 255;
            brightest = std::max({brightest, in[i * 4 + 0], in[i * 4 + 1], in[i * 4 + 2]});
        }
        peak = std::pow(double(brightest) / 255.0, 2.2);
    }
    SDL_Surface* surface =
        SDL_CreateSurfaceFrom(int(width), int(height), SDL_PIXELFORMAT_RGBA32, rgbaPixels.data(), int(width * 4));
    if (!surface) return Error{std::format("SDL_CreateSurfaceFrom: {}", SDL_GetError())};
    SDL_IOStream* memory = SDL_IOFromDynamicMem();
    const bool encoded = memory && SDL_SavePNG_IO(surface, memory, false);
    SDL_DestroySurface(surface);
    if (!encoded) {
        if (memory) SDL_CloseIO(memory);
        return Error{std::format("PNG encoding: {}", SDL_GetError())};
    }
    const Sint64 size = SDL_GetIOSize(memory);
    const auto* data = static_cast<const std::uint8_t*>(
        SDL_GetPointerProperty(SDL_GetIOProperties(memory), SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, nullptr));
    std::vector<std::uint8_t> png(data, data + (data && size > 0 ? size : 0));
    SDL_CloseIO(memory);

    if (!std::filesystem::path(path).is_absolute()) {
        if (Result<void> written = fs::writeUser(path, png); !written) return Error{written.error()};
        return peak;
    }
    SDL_IOStream* file = SDL_IOFromFile(std::string(path).c_str(), "wb");
    if (!file) return Error{std::format("cannot open: {}", SDL_GetError())};
    const bool written = SDL_WriteIO(file, png.data(), png.size()) == png.size();
    const bool closed = SDL_CloseIO(file);
    if (!written || !closed) return Error{std::format("write: {}", SDL_GetError())};
    return peak;
}

} // namespace quench::app::detail
