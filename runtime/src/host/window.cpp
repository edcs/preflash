#include "window.h"

#include <SDL2/SDL.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "preflash/audio.h"
#include "preflash/board.h"
#include "preflash/panel.h"
#include "preflash/touch.h"

namespace preflash::window {
namespace {

constexpr int kBytesPerPixel = 2;
// Whatever else the host needs from SDL gets initialised with the window.
constexpr Uint32 kSdlSubsystems = SDL_INIT_VIDEO;

int g_width = 0;
int g_height = 0;
SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_texture = nullptr;
// Set by firmware's thread when it first opens the codec, taken by the main loop.
std::atomic<bool> g_audio_wanted{false};

[[noreturn]] void fail(const char* what) {
  std::fprintf(stderr, "preflash: %s: %s\n", what, SDL_GetError());
  std::exit(1);
}

// A drag can leave the window, and a release out there must still count.
void touch_at(int x, int y, bool down) {
  touch::push({std::clamp(x, 0, g_width - 1), std::clamp(y, 0, g_height - 1), down});
}

void handle_events() {
  static bool held = false;
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_WINDOWEVENT:
        if (e.window.event != SDL_WINDOWEVENT_CLOSE) break;
        [[fallthrough]];
      case SDL_QUIT:
        // Not exit(): firmware threads may be holding locks that destructors would need.
        _exit(0);
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP:
        if (e.button.button == SDL_BUTTON_LEFT) {
          held = e.type == SDL_MOUSEBUTTONDOWN;
          touch_at(e.button.x, e.button.y, held);
        }
        break;
      case SDL_MOUSEMOTION:
        if (held) touch_at(e.motion.x, e.motion.y, true);
        break;
      default:
        break;
    }
  }
}

// Square-root response: 1% is dim but visible, like the panel in a dark room.
Uint8 backlight_level() {
  return static_cast<Uint8>(std::lround(255 * std::sqrt(panel::brightness() / 100.0)));
}

// The device takes one fixed format and SDL converts what firmware writes to it,
// since the codec's rate isn't known until firmware opens it, long after the window.
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
void open_audio() {
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    std::fprintf(stderr, "preflash: no audio, so sound is dropped: %s\n", SDL_GetError());
    return;
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
    return;
  }
  SDL_PauseAudioDevice(device, 0);
  audio::set_sink(new SdlSink(device));
}

}  // namespace

void open() {
  g_width = board().width;
  g_height = board().height;
  if (SDL_Init(kSdlSubsystems) != 0) fail("couldn't start SDL");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
  g_window = SDL_CreateWindow(panel::window_title().c_str(), SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, g_width, g_height, 0);
  if (!g_window) fail("couldn't open the window");
  g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_PRESENTVSYNC);
  if (!g_renderer) fail("couldn't draw to the window");
  SDL_RenderSetLogicalSize(g_renderer, g_width, g_height);
  g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                g_width, g_height);
  if (!g_texture) fail("couldn't make the panel's texture");
  // Not opened yet: a Mac with no output device can hold SDL for many seconds, and
  // firmware that never plays a sound shouldn't wait on it. The first time it opens
  // the codec, the main loop in run() opens the device.
  audio::set_device_request([] { g_audio_wanted = true; });
}

void run() {
  std::vector<uint8_t> frame;
  int shown_brightness = -1;
  for (;;) {
    handle_events();
    // SDL audio starts on this thread. The window doesn't redraw while the device
    // opens, but the firmware keeps running, and its sound is dropped until then.
    if (g_audio_wanted.exchange(false)) open_audio();
    if (panel::take_frame(&frame)) {
      SDL_UpdateTexture(g_texture, nullptr, frame.data(), g_width * kBytesPerPixel);
    }
    if (panel::brightness() != shown_brightness) {
      shown_brightness = panel::brightness();
      SDL_SetWindowTitle(g_window, panel::window_title().c_str());
    }
    const Uint8 level = backlight_level();
    SDL_SetTextureColorMod(g_texture, level, level, level);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    SDL_RenderPresent(g_renderer);
    SDL_Delay(1);
  }
}

}  // namespace preflash::window
