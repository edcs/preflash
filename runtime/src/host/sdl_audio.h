#pragma once

// The sound card: the only SDL the app itself uses, all of it on the main thread. The window
// is a process of its own (runtime/window).
namespace preflash::sdl_audio {

// From now on, firmware's first open of the codec asks for an output device. Not opened
// before that: a Mac with no output device can hold SDL for many seconds, and firmware that
// never plays a sound shouldn't wait on it.
void listen_for_codec();

// On the main thread: opens the device if firmware has asked for one. A Mac with no output
// device still runs the firmware, with sound counted and dropped, and the next open asks again.
void open_if_asked();

}  // namespace preflash::sdl_audio
