#pragma once

#include <cstddef>

namespace preflash::audio {

// Where windowed audio goes. The window installs one on the main thread, which is
// the only code that talks to SDL, and the codec model calls it from the writer's
// thread. Without one, which is a headless run or a Mac with no audio device,
// writes are counted and dropped, taking as long as the sound would to play.
class Sink {
 public:
  virtual ~Sink() = default;
  // 16-bit samples from now on, interleaved.
  virtual void start(int rate, int channels) = 0;
  // False if the sound could not be queued; the codec then gives up on the sink.
  virtual bool queue(const void* samples, size_t bytes) = 0;
  // How much sound is waiting to play.
  virtual int queued_ms() = 0;
};

void set_sink(Sink* sink);

}  // namespace preflash::audio

namespace preflash {
// Sample frames (one per channel-set) that firmware has written to the codec since start.
size_t audio_samples_written();
}  // namespace preflash
