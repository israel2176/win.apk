// audio_capture.h -- the microphone on Windows: WASAPI capture of the default
// input device, converted to the app's rate and channel count as float
// (native_audio_input.cpp). AudioRecord (AudioInputBridge) and OpenSL ES's
// recorders (native_opensles.cpp) both read it.

#pragma once

#include <cstddef>
#include <memory>

namespace winapk::audio {

class Capture;

// Windows' privacy switch for the microphone (Settings > Privacy >
// Microphone): false when it is off for desktop programs.
bool microphone_allowed();

// Whether the app holds android.permission.RECORD_AUDIO, asked as
// AudioRecord asks it (AudioInputBridge.open: SpecialAccessStore.holds); true
// when the store has no answer or no JVM runs. Any thread: one that is not a
// Java thread is attached as a daemon.
bool record_audio_permitted();

// A capture of `channels` channels at `rate` Hz, buffering at least
// `buffer_frames` (and a second) of what was captured; nothing is opened yet.
std::shared_ptr<Capture> capture_create(int rate, int channels, int buffer_frames);
// Opens the device and starts capturing; false if there is no capture device
// or access is denied. Waits (bounded) for the device to open.
bool capture_start(Capture* c);
void capture_stop(Capture* c);
// Takes up to `n` samples (channels interleaved, float in [-1, 1], whole
// frames). Blocking waits, bounded, for the whole request.
size_t capture_read(Capture* c, float* out, size_t n, bool blocking);

}  // namespace winapk::audio
