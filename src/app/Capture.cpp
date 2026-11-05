#include "AppState.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <format>
#include <vector>

namespace quench::app::detail {

Result<void> writePng(std::string_view path, std::span<const std::byte> pixels, std::uint32_t width,
                      std::uint32_t height, gpu::TextureFormat format) {
    const bool bgra = format == gpu::TextureFormat::Bgra8Unorm || format == gpu::TextureFormat::Bgra8UnormSrgb;
    const bool rgba = format == gpu::TextureFormat::Rgba8Unorm || format == gpu::TextureFormat::Rgba8UnormSrgb;
    if (!bgra && !rgba) return Error{std::format("captures of {} are not supported", gpu::formatInfo(format).name)};
    if (pixels.size() < std::size_t(width) * height * 4) return Error{"capture: too few pixels"};

    // RGBA, opaque: captures compare equal whatever alpha the frame left in the target.
    std::vector<std::uint8_t> rgbaPixels(std::size_t(width) * height * 4);
    const auto* in = reinterpret_cast<const std::uint8_t*>(pixels.data());
    for (std::size_t i = 0; i < std::size_t(width) * height; ++i) {
        rgbaPixels[i * 4 + 0] = in[i * 4 + (bgra ? 2 : 0)];
        rgbaPixels[i * 4 + 1] = in[i * 4 + 1];
        rgbaPixels[i * 4 + 2] = in[i * 4 + (bgra ? 0 : 2)];
        rgbaPixels[i * 4 + 3] = 255;
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

    if (!std::filesystem::path(path).is_absolute()) return fs::writeUser(path, png);
    SDL_IOStream* file = SDL_IOFromFile(std::string(path).c_str(), "wb");
    if (!file) return Error{std::format("cannot open: {}", SDL_GetError())};
    const bool written = SDL_WriteIO(file, png.data(), png.size()) == png.size();
    const bool closed = SDL_CloseIO(file);
    if (!written || !closed) return Error{std::format("write: {}", SDL_GetError())};
    return {};
}

} // namespace quench::app::detail
