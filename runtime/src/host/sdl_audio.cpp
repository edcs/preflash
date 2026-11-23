#include "sdl_audio.h"

#include <SDL2/SDL.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "preflash/audio.h"

namespace preflash::sdl_audio {
namespace {

// Set by firmware's thread when it first opens the codec, taken by the main thread.
std::atomic<bool> g_audio_wanted{false};

// The device takes one fixed format and SDL converts what firmware writes to it,
// since the codec's rate isn't known until firmware opens it, long after the app starts.
constexpr int kDeviceRate = 48000;
constexpr Uint8 kDeviceChannels = 2;
constexpr int kBytesPerSample = 2;

class SdlSink : public audio::Sink {
 public:
  explicit SdlSink(SDL_AudioDeviceID device) : device_(device) {}

  void start(int rate, int channels) override {
    if (stream_) SDL_FreeAudioStream(stream_);
    stream_ = SDL_NewAudioStream(AUDIO_S16SYS, static_cast<Uint8>(channels), rate, AUDIO_S16SYS,
                                 kDeviceChannels, kDeviceRate);
    if (!stream_) std::fprintf(stderr, "preflash: no audio conversion: %s\n", SDL_GetError());
  }

  bool queue(const void* samples, size_t bytes) override {
    if (!stream_ || SDL_AudioStreamPut(stream_, samples, static_cast<int>(bytes)) != 0)
      return false;
    const int ready = SDL_AudioStreamAvailable(stream_);
    if (ready < 0) return false;
    if (ready == 0) return true;
    converted_.resize(static_cast<size_t>(ready));
    const int got = SDL_AudioStreamGet(stream_, converted_.data(), ready);
    if (got < 0) return false;
    if (got == 0) return true;
    return SDL_QueueAudio(device_, converted_.data(), static_cast<Uint32>(got)) == 0;
  }

  int queued_ms() override {
    const Uint64 bytes_per_second = kDeviceRate * kDeviceChannels * kBytesPerSample;
    return static_cast<int>(SDL_GetQueuedAudioSize(device_) * 1000ull / bytes_per_second);
  }

 private:
  SDL_AudioDeviceID device_;
  SDL_AudioStream* stream_ = nullptr;
  std::vector<uint8_t> converted_;
};

// A Mac with no output device still runs the firmware: sound is counted and dropped.
// False if there's no device yet, so that the next codec open can try again.
bool open_audio() {
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    std::fprintf(stderr, "preflash: no audio, so sound is dropped: %s\n", SDL_GetError());
    return false;
  }
  SDL_AudioSpec want = {};
  want.freq = kDeviceRate;
  want.format = AUDIO_S16SYS;
  want.channels = kDeviceChannels;
  want.samples = 1024;
  SDL_AudioSpec have;
  const SDL_AudioDeviceID device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (device == 0) {
    std::fprintf(stderr, "preflash: no audio device, so sound is dropped: %s\n", SDL_GetError());
    return false;
  }
  SDL_PauseAudioDevice(device, 0);
  audio::set_sink(new SdlSink(device));
  return true;
}


}  // namespace

void listen_for_codec() {
  audio::set_device_request([] { g_audio_wanted = true; });
}

void open_if_asked() {
  // SDL audio starts on this thread. The firmware keeps running while the device opens, and
  // its sound is dropped until then.
  if (g_audio_wanted.exchange(false) && !open_audio()) listen_for_codec();
}

}  // namespace preflash::sdl_audio
