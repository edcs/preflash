#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "bsp/audio.h"
#include "preflash/audio.h"

namespace preflash {
namespace {

constexpr int kMaxQueuedMs = 100;
// A device that stops draining would hold the firmware's writer for ever, so it
// is given up on, and writes are counted like a headless run's.
constexpr auto kStallLimit = std::chrono::seconds(2);
constexpr auto kPoll = std::chrono::milliseconds(5);

struct Speaker {
  std::mutex lock;
  bool open = false;
  int channels = 0;
  int rate = 0;
  int volume = 100;
  bool sink_gone = false;
  // Closers waiting for the lock. A write in the middle of pacing sees it and lets go.
  std::atomic<int> closers{0};
};

Speaker& speaker() {
  static Speaker s;
  return s;
}

audio::Sink* g_sink = nullptr;  // under the speaker's lock
std::mutex g_request_mutex;
std::function<void()> g_request;
bool g_requested = false;
std::atomic<size_t> g_frames{0};

// Waits until the sink has room. False if it never drains, or if the codec is
// being closed, which has no use for the sound.
bool wait_for_room(audio::Sink* sink, const std::atomic<int>& closers) {
  auto progress = std::chrono::steady_clock::now();
  int last = sink->queued_ms();
  while (last > kMaxQueuedMs) {
    if (closers > 0) return false;
    std::this_thread::sleep_for(kPoll);
    const int now = sink->queued_ms();
    if (now < last) progress = std::chrono::steady_clock::now();
    last = now;
    if (std::chrono::steady_clock::now() - progress > kStallLimit) return false;
  }
  return true;
}

}  // namespace

namespace audio {
void set_sink(Sink* sink) {
  Speaker& s = speaker();
  std::lock_guard<std::mutex> hold(s.lock);
  g_sink = sink;
  // A new sink gets a fresh chance; only the one that failed is given up on.
  s.sink_gone = false;
  // Firmware may have opened the codec before the device was ready.
  if (sink && s.open) sink->start(s.rate, s.channels);
}

void set_device_request(std::function<void()> request) {
  std::lock_guard<std::mutex> hold(g_request_mutex);
  g_request = std::move(request);
  g_requested = false;
}
}  // namespace audio

size_t audio_samples_written() { return g_frames; }

}  // namespace preflash

extern "C" {

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void) {
  esp_io_expander_handle_t expander = bsp_io_expander_init();
  esp_io_expander_set_dir(expander, BSP_POWER_AMP_IO, IO_EXPANDER_OUTPUT);
  esp_io_expander_set_level(expander, BSP_POWER_AMP_IO, true);
  return &preflash::speaker();
}

int esp_codec_dev_open(esp_codec_dev_handle_t codec, esp_codec_dev_sample_info_t* fs) {
  if (codec != &preflash::speaker() || !fs) return ESP_CODEC_DEV_INVALID_ARG;
  if (fs->bits_per_sample != 16 || fs->channel < 1 || fs->channel > 8 || fs->sample_rate == 0) {
    std::fprintf(stderr,
                 "preflash: audio format rejected: %d bits, %d channels, %u Hz (need 16 bits, "
                 "1 to 8 channels, a nonzero rate)\n",
                 static_cast<int>(fs->bits_per_sample), static_cast<int>(fs->channel),
                 static_cast<unsigned>(fs->sample_rate));
    return ESP_CODEC_DEV_INVALID_ARG;
  }
  preflash::Speaker& s = preflash::speaker();
  {
    std::lock_guard<std::mutex> hold(s.lock);
    s.open = true;
    s.sink_gone = false;  // each open gets a fresh chance at the device
    s.channels = fs->channel;
    s.rate = static_cast<int>(fs->sample_rate);
    if (preflash::g_sink) preflash::g_sink->start(s.rate, s.channels);
  }
  // Outside the lock: the device is only wished for here, and opened elsewhere.
  std::function<void()> ask;
  {
    std::lock_guard<std::mutex> hold(preflash::g_request_mutex);
    if (preflash::g_request && !preflash::g_requested) {
      preflash::g_requested = true;
      ask = preflash::g_request;
    }
  }
  if (ask) ask();
  return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_close(esp_codec_dev_handle_t codec) {
  if (codec != &preflash::speaker()) return ESP_CODEC_DEV_INVALID_ARG;
  preflash::Speaker& s = preflash::speaker();
  // A write that is pacing holds the lock for up to a chunk of sound. Saying so first
  // lets it go, so that closing doesn't wait on the sound.
  ++s.closers;
  std::lock_guard<std::mutex> hold(s.lock);
  --s.closers;
  s.open = false;
  return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t codec, int volume) {
  if (codec != &preflash::speaker() || volume < 0 || volume > 100) return ESP_CODEC_DEV_INVALID_ARG;
  preflash::Speaker& s = preflash::speaker();
  std::lock_guard<std::mutex> hold(s.lock);
  s.volume = volume;
  return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_write(esp_codec_dev_handle_t codec, void* data, int len) {
  if (codec != &preflash::speaker() || (!data && len > 0) || len < 0)
    return ESP_CODEC_DEV_INVALID_ARG;
  preflash::Speaker& s = preflash::speaker();
  std::unique_lock<std::mutex> hold(s.lock);
  if (!s.open) return ESP_CODEC_DEV_WRONG_STATE;
  const size_t frame_bytes = sizeof(int16_t) * static_cast<size_t>(s.channels);
  if (static_cast<size_t>(len) % frame_bytes != 0) return ESP_CODEC_DEV_INVALID_ARG;
  if (len == 0) return ESP_CODEC_DEV_OK;

  const size_t frames = static_cast<size_t>(len) / frame_bytes;
  size_t dropped = frames;
  if (preflash::g_sink && !s.sink_gone) {
    std::vector<int16_t> scaled(static_cast<size_t>(len) / sizeof(int16_t));
    std::memcpy(scaled.data(), data, static_cast<size_t>(len));
    if (s.volume != 100)
      for (int16_t& v : scaled) v = static_cast<int16_t>(v * s.volume / 100);
    // The room check only sees what is already queued, so one long write is fed in
    // pieces of about 100 ms; otherwise it would all go in at once and never pace.
    const size_t chunk_frames =
        std::max<size_t>(1, static_cast<size_t>(s.rate) * preflash::kMaxQueuedMs / 1000);
    const size_t chunk_bytes = chunk_frames * frame_bytes;
    const auto* bytes = reinterpret_cast<const uint8_t*>(scaled.data());
    for (size_t at = 0; at < static_cast<size_t>(len); at += chunk_bytes) {
      const size_t n = std::min(chunk_bytes, static_cast<size_t>(len) - at);
      if (s.closers > 0) {
        dropped = 0;  // closing: nothing is left to pace
        break;
      }
      if (!preflash::wait_for_room(preflash::g_sink, s.closers)) {
        if (s.closers > 0) {
          dropped = 0;  // closing: nothing is left to pace
          break;
        }
        std::fprintf(stderr, "preflash: the audio device stopped playing, so sound is dropped\n");
        s.sink_gone = true;
        break;
      }
      if (!preflash::g_sink->queue(bytes + at, n)) {
        std::fprintf(stderr, "preflash: the audio device refused sound, so it is dropped\n");
        s.sink_gone = true;
        break;
      }
      dropped -= n / frame_bytes;
    }
  }
  preflash::g_frames += frames;
  if (dropped > 0) {
    // I2S takes sound at its sample rate. Firmware can time itself by the writes, so
    // sound with nowhere to go still takes as long as it would to play.
    const auto playing = std::chrono::microseconds(dropped * 1000000 / static_cast<size_t>(s.rate));
    hold.unlock();
    std::this_thread::sleep_for(playing);
  }
  return ESP_CODEC_DEV_OK;
}

}  // extern "C"
