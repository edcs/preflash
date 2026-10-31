#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "preflash/fail.h"
#include "preflash/panel.h"

// The handles carry nothing: a board has one panel, so firmware's handle only
// has to exist.
struct esp_lcd_panel_t {};
struct esp_lcd_panel_io_t {};

namespace preflash::panel {

esp_lcd_panel_handle_t handle() {
  static esp_lcd_panel_t panel;
  return &panel;
}

esp_lcd_panel_io_handle_t io_handle() {
  static esp_lcd_panel_io_t io;
  return &io;
}

}  // namespace preflash::panel

extern "C" {

esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t panel) {
  return panel ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t panel) {
  return panel ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t panel) {
  return panel ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t panel, bool on_off) {
  if (!panel) return ESP_ERR_INVALID_ARG;
  // Staying lit would show firmware's "off" screen as if it had worked.
  if (!on_off) preflash_unsupported("esp_lcd_panel_disp_on_off", "the panel can't turn off yet");
  return ESP_OK;
}

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start, int y_start,
                                    int x_end, int y_end, const void* color_data) {
  if (!panel || !color_data || x_start >= x_end || y_start >= y_end) return ESP_ERR_INVALID_ARG;
  preflash::panel::draw(x_start, y_start, x_end, y_end, color_data);
  return ESP_OK;
}

esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io) {
  return io ? ESP_OK : ESP_ERR_INVALID_ARG;
}

}  // extern "C"
