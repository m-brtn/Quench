#pragma once

// The one place that maps compiler platform macros to QUENCH_PLATFORM_* (each 0 or 1). Engine code outside platform
// backend files tests only these.

#if defined(__APPLE__)
#include <TargetConditionals.h>
#define QUENCH_PLATFORM_APPLE 1
#define QUENCH_PLATFORM_IOS (TARGET_OS_IOS)
#define QUENCH_PLATFORM_IOS_SIMULATOR (TARGET_OS_IOS && TARGET_OS_SIMULATOR)
#define QUENCH_PLATFORM_MACOS (TARGET_OS_OSX)
#else
#define QUENCH_PLATFORM_APPLE 0
#define QUENCH_PLATFORM_IOS 0
#define QUENCH_PLATFORM_IOS_SIMULATOR 0
#define QUENCH_PLATFORM_MACOS 0
#endif

#if defined(__ANDROID__)
#define QUENCH_PLATFORM_ANDROID 1
#else
#define QUENCH_PLATFORM_ANDROID 0
#endif

#if defined(__linux__) && !defined(__ANDROID__)
#define QUENCH_PLATFORM_LINUX 1
#else
#define QUENCH_PLATFORM_LINUX 0
#endif

#if defined(_WIN32)
#define QUENCH_PLATFORM_WINDOWS 1
#else
#define QUENCH_PLATFORM_WINDOWS 0
#endif

#if defined(__EMSCRIPTEN__)
#define QUENCH_PLATFORM_WEB 1
#else
#define QUENCH_PLATFORM_WEB 0
#endif
