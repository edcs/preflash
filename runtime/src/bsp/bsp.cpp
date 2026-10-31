#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "preflash/board.h"
#include "preflash/fail.h"
#include "preflash/panel.h"

extern "C" {

lv_display_t* bsp_display_start(void) {
  const bsp_display_cfg_t cfg = {.lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG()};
  return bsp_display_start_with_config(&cfg);
}

lv_display_t* bsp_display_start_with_config(const bsp_display_cfg_t* cfg) {
  if (!cfg || lvgl_port_init(&cfg->lvgl_port_cfg) != ESP_OK) return nullptr;

  const preflash::Board& board = preflash::board();
  lvgl_port_display_cfg_t display = {};
  display.io_handle = preflash::panel::io_handle();
  display.panel_handle = preflash::panel::handle();
  display.hres = static_cast<uint32_t>(board.width);
  display.vres = static_cast<uint32_t>(board.height);
  display.color_format = LV_COLOR_FORMAT_RGB565;
  return lvgl_port_add_disp(&display);
}

lv_indev_t* bsp_display_get_input_dev(void) {
  // New devices join at the head of LVGL's list, so the port's touch input stays last.
  lv_indev_t* last = nullptr;
  for (lv_indev_t* i = lv_indev_get_next(nullptr); i; i = lv_indev_get_next(i)) last = i;
  return last;
}

bool bsp_display_lock(uint32_t timeout_ms) { return lvgl_port_lock(timeout_ms); }

void bsp_display_unlock(void) { lvgl_port_unlock(); }

void bsp_display_rotate(lv_display_t* disp, lv_display_rotation_t rotation) {
  lv_display_set_rotation(disp, rotation);
}

esp_err_t bsp_display_new(const bsp_display_config_t*, esp_lcd_panel_handle_t* ret_panel,
                          esp_lcd_panel_io_handle_t* ret_io) {
  if (!ret_panel) return ESP_ERR_INVALID_ARG;
  *ret_panel = preflash::panel::handle();
  if (ret_io) *ret_io = preflash::panel::io_handle();
  return ESP_OK;
}

esp_err_t bsp_display_brightness_init(void) { return ESP_OK; }

esp_err_t bsp_display_brightness_set(int brightness_percent) {
  preflash::panel::set_brightness(brightness_percent);
  return ESP_OK;
}

int bsp_display_brightness_get(void) { return preflash::panel::brightness(); }

esp_err_t bsp_display_backlight_on(void) { return bsp_display_brightness_set(100); }

esp_err_t bsp_display_backlight_off(void) { return bsp_display_brightness_set(0); }

int preflash_backlight_pin(void) { return preflash::board().backlight.pin; }

esp_err_t bsp_i2c_init(void) {
  bsp_i2c_get_handle();
  return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void) {
  static const i2c_master_bus_handle_t bus = [] {
    const i2c_master_bus_config_t config = {};
    i2c_master_bus_handle_t handle = nullptr;
    ESP_ERROR_CHECK(i2c_new_master_bus(&config, &handle));
    return handle;
  }();
  return bus;
}

esp_err_t bsp_touch_new(const bsp_touch_config_t*, esp_lcd_touch_handle_t*) {
  preflash_unsupported("bsp_touch_new", "touches go through lvgl_port_add_disp");
}

}  // extern "C"
