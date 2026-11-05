// quench_image_compare: compares a capture with a golden image (PNG) for the capture checks (tests/RunCapture.cmake).
//
//   quench_image_compare ACTUAL EXPECTED [--diff OUT.png] [--threshold T] [--max-mean M] [--max-over F]
//
// Channel differences are 0..255. Passes when the mean absolute difference is at most M (default 0.5) and the share of
// pixels with a channel difference above T (default 16) is at most F (default 0.001). Exit code 0: pass, 1: the images
// differ, 2: an image could not be read. --diff writes the differences, amplified 4x, as PNG. Host tool: it uses the
// standard output and SDL's PNG reader directly.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

SDL_Surface* load(const char* path) {
    SDL_Surface* loaded = SDL_LoadPNG(path);
    if (!loaded) {
        std::printf("cannot read %s: %s\n", path, SDL_GetError());
        return nullptr;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (!rgba) std::printf("cannot convert %s: %s\n", path, SDL_GetError());
    return rgba;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: quench_image_compare ACTUAL EXPECTED [--diff OUT.png] [--threshold T] [--max-mean M] "
                    "[--max-over F]\n");
        return 2;
    }
    const char* diffPath = nullptr;
    int threshold = 16;
    double maxMean = 0.5;
    double maxOver = 0.001;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--diff") == 0)
            diffPath = argv[i + 1];
        else if (std::strcmp(argv[i], "--threshold") == 0)
            threshold = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--max-mean") == 0)
            maxMean = std::atof(argv[i + 1]);
        else if (std::strcmp(argv[i], "--max-over") == 0)
            maxOver = std::atof(argv[i + 1]);
        else {
            std::printf("unknown option %s\n", argv[i]);
            return 2;
        }
    }

    SDL_Surface* actual = load(argv[1]);
    SDL_Surface* expected = load(argv[2]);
    if (!actual || !expected) return 2;
    if (actual->w != expected->w || actual->h != expected->h) {
        std::printf("size differs: %dx%d, expected %dx%d\n", actual->w, actual->h, expected->w, expected->h);
        return 1;
    }

    const int w = actual->w, h = actual->h;
    std::vector<Uint8> diff(std::size_t(w) * h * 4, 255);
    double sum = 0.0;
    int maxDiff = 0;
    long over = 0;
    for (int y = 0; y < h; ++y) {
        const auto* a = static_cast<const Uint8*>(actual->pixels) + std::size_t(y) * actual->pitch;
        const auto* e = static_cast<const Uint8*>(expected->pixels) + std::size_t(y) * expected->pitch;
        for (int x = 0; x < w; ++x) {
            int pixelMax = 0;
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs(int(a[x * 4 + c]) - int(e[x * 4 + c]));
                sum += d;
                pixelMax = std::max(pixelMax, d);
                diff[(std::size_t(y) * w + x) * 4 + c] = Uint8(std::min(255, d * 4));
            }
            maxDiff = std::max(maxDiff, pixelMax);
            over += pixelMax > threshold ? 1 : 0;
        }
    }
    const double mean = sum / (double(w) * h * 3);
    const double overShare = double(over) / (double(w) * h);
    const bool pass = mean <= maxMean && overShare <= maxOver;
    std::printf("%dx%d: mean difference %.4f (max %.2f), largest %d, pixels over %d: %ld (%.4f%%, max %.4f%%): %s\n", w,
                h, mean, maxMean, maxDiff, threshold, over, overShare * 100.0, maxOver * 100.0, pass ? "pass" : "FAIL");

    if (diffPath) {
        SDL_Surface* surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, diff.data(), w * 4);
        if (!surface || !SDL_SavePNG(surface, diffPath)) std::printf("cannot write %s: %s\n", diffPath, SDL_GetError());
        SDL_DestroySurface(surface);
    }
    SDL_DestroySurface(actual);
    SDL_DestroySurface(expected);
    return pass ? 0 : 1;
}
