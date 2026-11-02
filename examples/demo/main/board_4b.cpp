// The ESP32-S3-Touch-LCD-4B, through Waveshare's board package: the package owns the bus, the
// panel, touch and LVGL's task, so little is left to do here.
#include "board.h"

#include "bsp/esp-bsp.h"
#include "esp_check.h"

namespace board {
namespace {

lv_display_t* g_display = nullptr;

}  // namespace

void start() {
  ESP_ERROR_CHECK(bsp_i2c_init());

  bsp_display_cfg_t config = {};
  config.lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  config.lvgl_port_cfg.task_stack = 8192;
  config.buffer_size = BSP_LCD_H_RES * 40;
  config.flags.buff_spiram = true;
  g_display = bsp_display_start_with_config(&config);
  ESP_ERROR_CHECK(g_display ? ESP_OK : ESP_FAIL);
}

const char* name() { return "ESP32-S3-Touch-LCD-4B"; }

i2c_master_bus_handle_t sensor_bus() { return bsp_i2c_get_handle(); }

void set_brightness(int percent) { ESP_ERROR_CHECK(bsp_display_brightness_set(percent)); }

bool can_turn_to(int) { return true; }

void turn_to(int degrees) {
  bsp_display_rotate(g_display, static_cast<lv_display_rotation_t>(degrees / 90));
}

bool has_power_chip() { return true; }

esp_codec_dev_handle_t speaker() {
  static esp_codec_dev_handle_t codec = bsp_audio_codec_speaker_init();
  return codec;
}

Lock::Lock() { bsp_display_lock(0); }

Lock::~Lock() { bsp_display_unlock(); }

}  // namespace board
