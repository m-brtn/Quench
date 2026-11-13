// Apple backend of the microphone permission (MicPermission.h).
//
// macOS: AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio and requestAccessForMediaType:
// (AVFoundation, "Requesting authorization to capture and save media"). The system (TCC) attributes the request to
// the responsible process: an app bundle asks with its own name and its Info.plist's NSMicrophoneUsageDescription; a
// bare executable started from a terminal is attributed to the terminal app, whose name and usage text the prompt
// shows, and whose answer (System Settings, Privacy & Security, Microphone) applies to everything it starts.
// iOS: AVAudioApplication's recordPermission and requestRecordPermissionWithCompletionHandler: (iOS 17 and later; the
// engine's deployment target is iOS 26), with the app's NSMicrophoneUsageDescription in the prompt.

#if !__has_feature(objc_arc)
#error "compile with ARC (-fobjc-arc, set by quench_configure_target)"
#endif

#include "core/Platform.h"

#if QUENCH_PLATFORM_APPLE

#include "MicPermission.h"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <stdatomic.h>
#include <string.h>

enum { kNotRead = -1, kNotAsked = 1, kAsking = 2, kGranted = 3, kDenied = 4 };

static atomic_int g_permission = kNotRead;

static int readStatus(void) {
#if QUENCH_PLATFORM_IOS
    switch ([AVAudioApplication sharedInstance].recordPermission) {
    case AVAudioApplicationRecordPermissionGranted: return kGranted;
    case AVAudioApplicationRecordPermissionDenied: return kDenied;
    case AVAudioApplicationRecordPermissionUndetermined: return kNotAsked;
    }
    return kNotAsked;
#else
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
    case AVAuthorizationStatusAuthorized: return kGranted;
    case AVAuthorizationStatusDenied: return kDenied;
    case AVAuthorizationStatusRestricted: return kDenied; // a policy (parental controls, a profile)
    case AVAuthorizationStatusNotDetermined: return kNotAsked;
    }
    return kNotAsked;
#endif
}

int quench_mic_permission(void) {
    int value = atomic_load(&g_permission);
    if (value != kNotRead) return value;
    @autoreleasepool {
        int expected = kNotRead;
        atomic_compare_exchange_strong(&g_permission, &expected, readStatus());
    }
    return atomic_load(&g_permission);
}

void quench_mic_request_permission(void) {
    if (quench_mic_permission() != kNotAsked) return;
    atomic_store(&g_permission, kAsking);
    @autoreleasepool {
#if QUENCH_PLATFORM_IOS
        [AVAudioApplication requestRecordPermissionWithCompletionHandler:^(BOOL granted) {
          atomic_store(&g_permission, granted ? kGranted : kDenied);
        }];
#else
        [AVCaptureDevice
            requestAccessForMediaType:AVMediaTypeAudio
                    completionHandler:^(BOOL granted) { atomic_store(&g_permission, granted ? kGranted : kDenied); }];
#endif
    }
}

bool quench_mic_bundle_declares_usage(char* bundle, int size) {
    @autoreleasepool {
        NSBundle* main = [NSBundle mainBundle];
        if (bundle && size > 0) strlcpy(bundle, main.bundlePath.UTF8String ?: "", (size_t)size);
        if (![main.bundlePath.pathExtension isEqualToString:@"app"]) return true;
        id text = [main objectForInfoDictionaryKey:@"NSMicrophoneUsageDescription"];
        return [text isKindOfClass:[NSString class]] && [(NSString*)text length] > 0;
    }
}

#endif
