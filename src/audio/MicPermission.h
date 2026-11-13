#pragma once

// The system's microphone permission on Apple platforms (MicPermissionApple.m): AVCaptureDevice's authorization for
// audio on macOS, AVAudioApplication's record permission on iOS. Any thread; the request's answer comes on a system
// queue and is kept in an atomic.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// audio::MicPermission as a number: 1 not asked, 2 asking, 3 granted, 4 denied. The first call reads the system's
// status (no prompt); later calls return the kept value, changed by a request's answer.
int quench_mic_permission(void);
// Shows the system's prompt when the status is not determined (once per process; the system remembers the answer).
void quench_mic_request_permission(void);
// false when the process is an app bundle whose Info.plist has no NSMicrophoneUsageDescription (the system would
// refuse or end the app at the first access); a bare executable has none and passes (the prompt names the app that
// started it, a terminal for example). `bundle` gets the bundle's path.
bool quench_mic_bundle_declares_usage(char* bundle, int size);

#ifdef __cplusplus
}
#endif
