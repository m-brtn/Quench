#pragma once

// Engine log output through SDL_Log: the Xcode console and the device log on Apple platforms, logcat on Android, the
// console elsewhere. Messages use SDL_LOG_CATEGORY_CUSTOM; the engine's own level (log::setLevel) filters them.

namespace quench::app {

// Adds the SDL_Log sink and turns the stderr sink off (SDL_Log already writes there on desktop systems). Returns the
// sink id for log::removeSink().
int addSdlLogSink();

} // namespace quench::app
