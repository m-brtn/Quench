// The engine owns main: SDL's main callbacks drive the app (no blocking loop, as browsers and mobile lifecycles
// require). Linked into game executables through the quench_main object library (quench_add_game()).

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "AppState.h"

using quench::app::detail::AppState;

SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv) {
    auto* state = new AppState();
    *appstate = state;
    return quench::app::detail::init(*state, argc, argv);
}

SDL_AppResult SDL_AppIterate(void* appstate) { return quench::app::detail::iterate(*static_cast<AppState*>(appstate)); }

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    return quench::app::detail::event(*static_cast<AppState*>(appstate), *event);
}

void SDL_AppQuit(void* appstate, SDL_AppResult result) {
    auto* state = static_cast<AppState*>(appstate);
    if (!state) return;
    quench::app::detail::quit(*state, result);
    delete state;
}
