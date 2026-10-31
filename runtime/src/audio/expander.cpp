#include <mutex>

#include "bsp/audio.h"
#include "esp_io_expander.h"

// The pins live in memory. The TCA9554 on the bus only has to answer, because
// nothing on the Mac listens to its outputs.
struct esp_io_expander_s {
  std::mutex lock;
  uint32_t outputs = 0;  // pins that are outputs
  uint32_t levels = 0;   // what they drive
};

extern "C" {

esp_io_expander_handle_t bsp_io_expander_init(void) {
  static esp_io_expander_t expander;
  return &expander;
}

esp_err_t esp_io_expander_set_dir(esp_io_expander_handle_t handle, uint32_t pin_num_mask,
                                  esp_io_expander_dir_t direction) {
  if (!handle) return ESP_ERR_INVALID_ARG;
  // Checked before any state changes, so a bad value can't quietly make pins inputs.
  if (direction != IO_EXPANDER_INPUT && direction != IO_EXPANDER_OUTPUT)
    return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> hold(handle->lock);
  if (direction == IO_EXPANDER_OUTPUT)
    handle->outputs |= pin_num_mask;
  else
    handle->outputs &= ~pin_num_mask;
  return ESP_OK;
}

esp_err_t esp_io_expander_set_level(esp_io_expander_handle_t handle, uint32_t pin_num_mask,
                                    uint8_t level) {
  if (!handle) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> hold(handle->lock);
  if ((pin_num_mask & ~handle->outputs) != 0) return ESP_ERR_INVALID_STATE;
  if (level)
    handle->levels |= pin_num_mask;
  else
    handle->levels &= ~pin_num_mask;
  return ESP_OK;
}

esp_err_t esp_io_expander_get_level(esp_io_expander_handle_t handle, uint32_t pin_num_mask,
                                    uint32_t* level_mask) {
  if (!handle || !level_mask) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> hold(handle->lock);
  // Nothing drives an input here, so it reads high, as the chip's pull-ups would.
  const uint32_t seen = (handle->levels & handle->outputs) | ~handle->outputs;
  *level_mask = seen & pin_num_mask;
  return ESP_OK;
}

esp_err_t bsp_audio_poweramp_enable(bool enable) {
  return esp_io_expander_set_level(bsp_io_expander_init(), BSP_POWER_AMP_IO, enable);
}

}  // extern "C"
