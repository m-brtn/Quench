#pragma once

// The iOS audio session (AudioSessionApple.m, AVAudioSession): configured and activated by the engine instead of
// miniaudio, so the category is the game's choice. iOS builds only.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// category: 0 ambient, 1 solo ambient, 2 playback (audio::SessionCategory). false: `error` says why.
bool quench_audio_session_configure(int category, char* error, int errorSize);
bool quench_audio_session_activate(bool active, char* error, int errorSize);
// Recording (the microphone): record = true switches to PlayAndRecord with the playback kept on the speaker
// (DefaultToSpeaker, not the earpiece), Bluetooth output kept in its high-quality profile (AllowBluetoothA2DP: the
// input is then the phone's microphone, not a headset's) and, when `category` is ambient, other apps' audio still
// mixed in (MixWithOthers); record = false sets `category` again (quench_audio_session_configure).
bool quench_audio_session_record(bool record, int category, char* error, int errorSize);
// The current output route, for example "Speaker (Speaker)" or "AirPods (BluetoothA2DPOutput)".
void quench_audio_session_route(char* out, int size);
// The category the session has now (AVAudioSessionCategory name), for the test.
void quench_audio_session_category(char* out, int size);

#ifdef __cplusplus
}
#endif
