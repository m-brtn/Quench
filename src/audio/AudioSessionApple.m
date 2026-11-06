// iOS backend of the audio session (AudioSession.h). Apple, "Configuring your app's audio session" and AVAudioSession
// documentation: the category decides how the app's audio mixes with other apps' and whether the Ring/Silent switch
// silences it; activation may fail while another app holds a non-mixable session (a call), and is retried after an
// interruption ends.

#if !__has_feature(objc_arc)
#error "compile with ARC (-fobjc-arc, set by quench_configure_target)"
#endif

#include "core/Platform.h"

#if QUENCH_PLATFORM_IOS

#include "AudioSession.h"

#import <AVFoundation/AVFoundation.h>

#include <string.h>

static void copyText(NSString* text, char* out, int size) {
    if (!out || size <= 0) return;
    strlcpy(out, text ? text.UTF8String : "", (size_t)size);
}

bool quench_audio_session_configure(int category, char* error, int errorSize) {
    @autoreleasepool {
        AVAudioSessionCategory name = category == 2   ? AVAudioSessionCategoryPlayback
                                      : category == 1 ? AVAudioSessionCategorySoloAmbient
                                                      : AVAudioSessionCategoryAmbient;
        NSError* failure = nil;
        if (![[AVAudioSession sharedInstance] setCategory:name
                                                     mode:AVAudioSessionModeDefault
                                                  options:0
                                                    error:&failure]) {
            copyText(failure.localizedDescription, error, errorSize);
            return false;
        }
        return true;
    }
}

bool quench_audio_session_activate(bool active, char* error, int errorSize) {
    @autoreleasepool {
        NSError* failure = nil;
        const AVAudioSessionSetActiveOptions options =
            active ? 0 : AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation;
        if (![[AVAudioSession sharedInstance] setActive:active withOptions:options error:&failure]) {
            copyText(failure.localizedDescription, error, errorSize);
            return false;
        }
        return true;
    }
}

void quench_audio_session_route(char* out, int size) {
    @autoreleasepool {
        NSMutableArray<NSString*>* outputs = [NSMutableArray array];
        for (AVAudioSessionPortDescription* port in [AVAudioSession sharedInstance].currentRoute.outputs)
            [outputs addObject:[NSString stringWithFormat:@"%@ (%@)", port.portName, port.portType]];
        copyText(outputs.count ? [outputs componentsJoinedByString:@", "] : @"no output", out, size);
    }
}

void quench_audio_session_category(char* out, int size) {
    @autoreleasepool {
        copyText([AVAudioSession sharedInstance].category, out, size);
    }
}

#endif
