#include "audio.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <numbers>

#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"

namespace audio {
namespace {

constexpr const char* kTag = "audio";
constexpr int kSampleRate = 16000;
constexpr int kBlockFrames = 256;
constexpr int kBeepFrames = kSampleRate / 4;
constexpr double kHz = 880.0;
constexpr double kAmplitude = 12000.0;
constexpr int kFadeFrames = 400;  // stops the tone's ends from clicking

esp_codec_dev_handle_t g_codec = nullptr;
TaskHandle_t g_task = nullptr;
std::atomic<unsigned> g_frames{0};

double fade_at(int frame) {
  const int from_edge = std::min(frame, kBeepFrames - 1 - frame);
  return from_edge < kFadeFrames ? static_cast<double>(from_edge) / kFadeFrames : 1.0;
}

void play_beep() {
  int16_t block[kBlockFrames];
  for (int frame = 0; frame < kBeepFrames; frame += kBlockFrames) {
    for (int i = 0; i < kBlockFrames; ++i) {
      const int at = frame + i;
      const double wave = std::sin(2 * std::numbers::pi * kHz * at / kSampleRate);
      block[i] = static_cast<int16_t>(kAmplitude * fade_at(at) * wave);
    }
    if (esp_codec_dev_write(g_codec, block, sizeof block) != ESP_CODEC_DEV_OK) {
      ESP_LOGW(kTag, "the codec refused a block");
      return;
    }
    g_frames += kBlockFrames;
  }
}

void audio_task(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    play_beep();
  }
}

}  // namespace

void start() {
  g_codec = board::speaker();
  if (!g_codec) return;
  esp_codec_dev_sample_info_t format = {};
  format.bits_per_sample = 16;
  format.channel = 1;
  format.sample_rate = kSampleRate;
  ESP_ERROR_CHECK(esp_codec_dev_open(g_codec, &format));
  set_volume(settings::get().volume);
  ESP_ERROR_CHECK(xTaskCreate(audio_task, "audio", 4096, nullptr, 3, &g_task) == pdPASS ? ESP_OK : ESP_FAIL);
}

bool available() { return g_codec != nullptr; }

void beep() {
  if (g_task) xTaskNotifyGive(g_task);
}

void set_volume(int percent) {
  if (g_codec) esp_codec_dev_set_out_vol(g_codec, percent);
}

unsigned frames_played() { return g_frames; }

}  // namespace audio
