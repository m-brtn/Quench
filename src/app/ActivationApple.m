// macOS backend of the offscreen mode (App.cpp): the process becomes an app that never appears in the Dock and is
// never activated, so a run nobody watches neither shows an icon nor takes the focus. It runs before SDL_Init: SDL's
// video init makes the shared application, which registers with Launch Services, and a bundle registers with the
// policy of its Info.plist (a regular app, a Dock icon) until the policy changes. Made here first, with SDL's class,
// it registers as a background-only app: LSBackgroundOnly goes into the process's copy of the Info.plist first (not
// a documented way; without it a bundle was a regular app for up to 20 ms in 9 of 10 runs, with it in none of 10,
// 2026-11-06). SDL then leaves the launch to whoever made the application and dispatches the events in its own pump.

#if !__has_feature(objc_arc)
#error "compile with ARC (-fobjc-arc, set by quench_configure_target)"
#endif

#include "core/Platform.h"

#if QUENCH_PLATFORM_MACOS
#import <AppKit/AppKit.h>

void quench_app_prohibit_activation(void) {
    @autoreleasepool {
        @try {
            [(NSMutableDictionary*)[NSBundle mainBundle].infoDictionary setObject:@YES forKey:@"LSBackgroundOnly"];
        } @catch (NSException* e) {}
        Class application = NSClassFromString(@"SDL3Application");
        [(application ? application : [NSApplication class]) sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        [NSApp finishLaunching];
    }
}
#endif
