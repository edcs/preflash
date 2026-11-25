#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace preflash::audio {

// Where windowed audio goes. The SDL sound card installs one once it has opened the
// device on the main thread. The codec model calls it from the writer's thread, which
// queues samples to SDL from there, as SDL's audio queue allows. Without one, which is
// a headless run or a Mac with no audio device, writes are counted and dropped, taking
// as long as the sound would to play.
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

struct TapFormat {
  int rate;
  int channels;
};

// A listener on everything firmware writes to the codec, with the volume applied. Taps
// work with or without a sink and never change how writes are paced. They are called
// on the firmware's thread, under the speaker's lock: a tap must return at once, so it
// never blocks or allocates without bound, and it must not call back into the codec.
class Tap {
 public:
  virtual ~Tap() = default;
  // The format that is open, 16-bit interleaved. Called when the codec opens, and at
  // once from add_tap if it is already open.
  virtual void format(TapFormat format) = 0;
  // One whole write, in the format given.
  virtual void samples(const int16_t* data, size_t frames) = 0;
  // The codec closed.
  virtual void stop() = 0;
};

// Returns an id for remove_tap.
int add_tap(Tap* tap);
// Never returns while that tap is being called; the tap is not called afterwards.
void remove_tap(int id);
// How many taps are in, for tests.
size_t tap_count();

}  // namespace preflash::audio

namespace preflash {
// Sample frames (one per channel-set) that firmware has written to the codec since start.
size_t audio_samples_written();
}  // namespace preflash
