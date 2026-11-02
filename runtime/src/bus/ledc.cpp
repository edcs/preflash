#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>

#include "driver/ledc.h"
#include "preflash/board.h"
#include "preflash/panel.h"

namespace {

struct Timer {
  bool configured = false;
  int resolution_bits = 0;
};

struct Channel {
  bool configured = false;
  int pin = -1;
  ledc_timer_t timer = LEDC_TIMER_0;
  uint32_t duty = 0;
  bool inverted = false;
};

std::mutex g_mutex;
std::array<Timer, LEDC_TIMER_MAX> g_timers;
std::array<Channel, LEDC_CHANNEL_MAX> g_channels;

bool real(ledc_mode_t mode, ledc_channel_t channel) {
  return mode == LEDC_LOW_SPEED_MODE && channel >= LEDC_CHANNEL_0 && channel < LEDC_CHANNEL_MAX;
}

// With the mutex held. Full scale counts as always on, which the hardware reaches a step
// past this, so that 0 and 100 percent both land exactly.
void show(const Channel& channel) {
  const preflash::Backlight& backlight = preflash::board().backlight;
  if (channel.pin != backlight.pin) return;
  const uint32_t top = (1u << g_timers[channel.timer].resolution_bits) - 1;
  uint32_t on = std::min(channel.duty, top);
  if (backlight.active_low != channel.inverted) on = top - on;
  preflash::panel::set_brightness(static_cast<int>((uint64_t{on} * 100 + top / 2) / top));
}

}  // namespace

extern "C" {

esp_err_t ledc_timer_config(const ledc_timer_config_t* timer_conf) {
  if (!timer_conf || timer_conf->speed_mode != LEDC_LOW_SPEED_MODE ||
      timer_conf->timer_num < LEDC_TIMER_0 || timer_conf->timer_num >= LEDC_TIMER_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  Timer& timer = g_timers[timer_conf->timer_num];
  if (timer_conf->deconfigure) {
    timer = {};
    return ESP_OK;
  }
  if (timer_conf->duty_resolution < LEDC_TIMER_1_BIT ||
      timer_conf->duty_resolution >= LEDC_TIMER_BIT_MAX || timer_conf->freq_hz == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  timer.configured = true;
  timer.resolution_bits = timer_conf->duty_resolution;
  return ESP_OK;
}

esp_err_t ledc_channel_config(const ledc_channel_config_t* ledc_conf) {
  if (!ledc_conf || !real(ledc_conf->speed_mode, ledc_conf->channel) ||
      ledc_conf->timer_sel < LEDC_TIMER_0 || ledc_conf->timer_sel >= LEDC_TIMER_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  Channel& channel = g_channels[ledc_conf->channel];
  channel = {true, ledc_conf->gpio_num, ledc_conf->timer_sel, ledc_conf->duty,
             ledc_conf->flags.output_invert != 0};
  if (g_timers[channel.timer].configured) show(channel);
  return ESP_OK;
}

esp_err_t ledc_set_duty(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t duty) {
  if (!real(speed_mode, channel)) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_channels[channel].configured) return ESP_ERR_INVALID_STATE;
  g_channels[channel].duty = duty;
  return ESP_OK;
}

esp_err_t ledc_update_duty(ledc_mode_t speed_mode, ledc_channel_t channel) {
  if (!real(speed_mode, channel)) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_mutex);
  const Channel& state = g_channels[channel];
  if (!state.configured || !g_timers[state.timer].configured) return ESP_ERR_INVALID_STATE;
  show(state);
  return ESP_OK;
}

}  // extern "C"
