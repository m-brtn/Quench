// Apple backend of queryDisplayHeadroom() (DisplayHeadroom.h): the EDR headroom of the screen a window is on, read
// every frame. Apple, "Performing your own tone mapping" (Metal documentation): the upper bound on displayable values
// "may vary from moment to moment"; read the current one while processing a frame and map all values between 0 and
// it, values above it are clamped. The potential value only says what the screen can reach.

#include "core/Platform.h"

#if QUENCH_PLATFORM_IOS
#import <UIKit/UIKit.h>
#elif QUENCH_PLATFORM_MACOS
#import <AppKit/AppKit.h>
#endif

#include <stdbool.h>

// nativeWindow: the UIWindow (iOS) or NSWindow (macOS) behind an SDL window. false: no screen or no query.
bool quench_app_screen_headroom(const void* nativeWindow, float* current, float* potential) {
    if (!nativeWindow) return false;
    @autoreleasepool {
#if QUENCH_PLATFORM_IOS
        if (@available(iOS 16.0, *)) {
            UIScreen* screen = ((__bridge UIWindow*)nativeWindow).windowScene.screen;
            if (!screen) return false;
            *current = (float)screen.currentEDRHeadroom;
            *potential = (float)screen.potentialEDRHeadroom;
            return true;
        }
        return false;
#elif QUENCH_PLATFORM_MACOS
        NSScreen* screen = ((__bridge NSWindow*)nativeWindow).screen;
        if (!screen) return false;
        *current = (float)screen.maximumExtendedDynamicRangeColorComponentValue;
        *potential = (float)screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
        return true;
#else
        (void)current;
        (void)potential;
        return false;
#endif
    }
}
