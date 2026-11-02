// A tone on the speaker, through esp_codec_dev. Playing happens on a task of its own, so the
// screen doesn't wait for it.
#pragma once

namespace audio {

void start();
bool available();
void beep();
void set_volume(int percent);

// Sample frames written to the codec since boot.
unsigned frames_played();

}  // namespace audio
