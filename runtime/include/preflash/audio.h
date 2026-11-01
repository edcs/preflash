#pragma once

#include <cstddef>
#include <functional>

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

// A sink may arrive after firmware has opened the codec; it is started with the
// format that is open. Until it does, sound is dropped, taking as long as it would
// to play.
void set_sink(Sink* sink);

// Asks the window for an output device, the first time firmware opens the codec.
// The request runs on the firmware's thread and must only note the wish: the device
// itself is opened on the main thread, which then calls set_sink if it has one.
// Installing a request lets the next open ask again.
void set_device_request(std::function<void()> request);

}  // namespace preflash::audio

namespace preflash {
// Sample frames (one per channel-set) that firmware has written to the codec since start.
size_t audio_samples_written();
}  // namespace preflash
