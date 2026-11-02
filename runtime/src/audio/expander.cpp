#include <mutex>

#include "bsp/audio.h"
#include "esp_io_expander.h"
#include "preflash/board.h"
#include "preflash/bus.h"
#include "preflash/fail.h"

namespace preflash {
namespace {

constexpr uint32_t kAllPins = 0xffffffff;

std::mutex g_mutex;
ExpanderPins g_pins = {0, kAllPins};

}  // namespace

ExpanderPins expander_pins() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_pins;
}

void expander_set_outputs(uint32_t mask, uint32_t outputs) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_pins.outputs = (g_pins.outputs & ~mask) | (outputs & mask);
}

void expander_set_levels(uint32_t mask, uint32_t levels) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_pins.levels = (g_pins.levels & ~mask) | (levels & mask);
}

void expander_power_on() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_pins = {0, kAllPins};
}

bool expander_level(int bit) {
  const ExpanderPins pins = expander_pins();
  return !(pins.outputs >> bit & 1) || (pins.levels >> bit & 1);
}

}  // namespace preflash

// The handle only has to exist: a board has one expander.
struct esp_io_expander_s {};

extern "C" {

esp_io_expander_handle_t bsp_io_expander_init(void) {
  static esp_io_expander_t expander;
  return &expander;
}

uint32_t preflash_power_amp_io(void) {
  const std::optional<int> bit = preflash::board().power_amp_bit;
  return bit ? 1u << *bit : 0;
}

esp_err_t esp_io_expander_set_dir(esp_io_expander_handle_t handle, uint32_t pin_num_mask,
                                  esp_io_expander_dir_t direction) {
  if (!handle) return ESP_ERR_INVALID_ARG;
  // Checked before any state changes, so a bad value can't quietly make pins inputs.
  if (direction != IO_EXPANDER_INPUT && direction != IO_EXPANDER_OUTPUT)
    return ESP_ERR_INVALID_ARG;
  preflash::expander_set_outputs(pin_num_mask, direction == IO_EXPANDER_OUTPUT ? pin_num_mask : 0);
  return ESP_OK;
}

esp_err_t esp_io_expander_set_level(esp_io_expander_handle_t handle, uint32_t pin_num_mask,
                                    uint8_t level) {
  if (!handle) return ESP_ERR_INVALID_ARG;
  if ((pin_num_mask & ~preflash::expander_pins().outputs) != 0) return ESP_ERR_INVALID_STATE;
  preflash::expander_set_levels(pin_num_mask, level ? pin_num_mask : 0);
  return ESP_OK;
}

esp_err_t esp_io_expander_get_level(esp_io_expander_handle_t handle, uint32_t pin_num_mask,
                                    uint32_t* level_mask) {
  if (!handle || !level_mask) return ESP_ERR_INVALID_ARG;
  const preflash::ExpanderPins pins = preflash::expander_pins();
  // Nothing drives an input here, so it reads high, as the chip's pull-ups would.
  const uint32_t seen = (pins.levels & pins.outputs) | ~pins.outputs;
  *level_mask = seen & pin_num_mask;
  return ESP_OK;
}

esp_err_t bsp_audio_poweramp_enable(bool enable) {
  if (preflash_power_amp_io() == 0) {
    preflash_unsupported("bsp_audio_poweramp_enable", "the board file names no power_amp");
  }
  return esp_io_expander_set_level(bsp_io_expander_init(), BSP_POWER_AMP_IO, enable);
}

}  // extern "C"
