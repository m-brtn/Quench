#include "DisplayHeadroom.h"

#include "core/Platform.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

#if QUENCH_PLATFORM_APPLE
extern "C" bool quench_app_screen_headroom(const void* nativeWindow, float* current, float* potential);
#endif

namespace quench::app::detail {

std::optional<DisplayHeadroom> queryDisplayHeadroom(SDL_Window* window) {
    if (!window) return std::nullopt;
#if QUENCH_PLATFORM_APPLE
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    const char* nativeWindow =
        QUENCH_PLATFORM_IOS ? SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER : SDL_PROP_WINDOW_COCOA_WINDOW_POINTER;
    DisplayHeadroom headroom;
    if (quench_app_screen_headroom(SDL_GetPointerProperty(props, nativeWindow, nullptr), &headroom.current,
                                   &headroom.potential))
        return headroom;
#endif
    return std::nullopt;
}

DisplayHeadroom resolveDisplayHeadroom(const std::optional<DisplayHeadroom>& platform, float sdl) {
    DisplayHeadroom h = platform.value_or(DisplayHeadroom{.current = sdl, .potential = sdl});
    h.current = std::isfinite(h.current) ? std::max(1.0f, h.current) : 1.0f;
    h.potential = std::isfinite(h.potential) ? std::max(h.current, h.potential) : h.current;
    return h;
}

bool HeadroomLog::due(float headroom, double now) {
    if (std::abs(headroom - m_value) <= 0.01f * std::max(1.0f, m_value) || now - m_time < kInterval) return false;
    logged(headroom, now);
    return true;
}

void HeadroomLog::logged(float headroom, double now) {
    m_value = headroom;
    m_time = now;
}

} // namespace quench::app::detail
