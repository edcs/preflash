#include <cstdint>
#include <vector>

#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "preflash/board.h"
#include "preflash/bus.h"
#include "preflash/fail.h"
#include "preflash/panel.h"

// A board has one panel, so firmware's handles only have to exist, apart from the
// callback firmware asks for at the end of a transfer.
struct esp_lcd_panel_t {};
struct esp_lcd_panel_io_t {
  esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done = nullptr;
  void* user_ctx = nullptr;
};

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

namespace {

constexpr int kBytesPerPixel = 2;

// A panel that takes the high byte first shows what firmware sent it only once the
// bytes are the other way round, because the framebuffer is little-endian.
void draw_for_panel(int x_start, int y_start, int x_end, int y_end, const void* color_data) {
  if (!preflash::board().big_endian) {
    preflash::panel::draw(x_start, y_start, x_end, y_end, color_data);
    return;
  }
  const size_t size = static_cast<size_t>(x_end - x_start) * (y_end - y_start) * kBytesPerPixel;
  const auto* from = static_cast<const uint8_t*>(color_data);
  std::vector<uint8_t> swapped(size);
  for (size_t i = 0; i < size; i += kBytesPerPixel) {
    swapped[i] = from[i + 1];
    swapped[i + 1] = from[i];
  }
  preflash::panel::draw(x_start, y_start, x_end, y_end, swapped.data());
}

}  // namespace

extern "C" {

esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                   const esp_lcd_panel_io_spi_config_t* io_config,
                                   esp_lcd_panel_io_handle_t* ret_io) {
  if (!io_config || !ret_io) return ESP_ERR_INVALID_ARG;
  if (!preflash::spi_bus_started(bus)) return ESP_ERR_INVALID_STATE;
  esp_lcd_panel_io_handle_t io = preflash::panel::io_handle();
  io->on_color_trans_done = io_config->on_color_trans_done;
  io->user_ctx = io_config->user_ctx;
  *ret_io = io;
  return ESP_OK;
}

esp_err_t esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io,
                                                    const esp_lcd_panel_io_callbacks_t* cbs,
                                                    void* user_ctx) {
  if (!io || !cbs) return ESP_ERR_INVALID_ARG;
  io->on_color_trans_done = cbs->on_color_trans_done;
  io->user_ctx = user_ctx;
  return ESP_OK;
}

esp_err_t esp_lcd_new_panel_axs15231b(const esp_lcd_panel_io_handle_t io,
                                      const esp_lcd_panel_dev_config_t* panel_dev_config,
                                      esp_lcd_panel_handle_t* ret_panel) {
  if (!io || !panel_dev_config || !ret_panel) return ESP_ERR_INVALID_ARG;
  const std::string& controller = preflash::board().controller;
  if (!controller.empty() && controller != "axs15231b") {
    preflash_unsupported("esp_lcd_new_panel_axs15231b", "the board file names another controller");
  }
  if (panel_dev_config->bits_per_pixel != 16) {
    preflash_unsupported("esp_lcd_new_panel_axs15231b", "the panel only shows 16-bit colour");
  }
  if (panel_dev_config->rgb_ele_order != LCD_RGB_ELEMENT_ORDER_RGB) {
    preflash_unsupported("esp_lcd_new_panel_axs15231b", "BGR element order has no model");
  }
  *ret_panel = preflash::panel::handle();
  return ESP_OK;
}

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
  draw_for_panel(x_start, y_start, x_end, y_end, color_data);
  esp_lcd_panel_io_handle_t io = preflash::panel::io_handle();
  if (io->on_color_trans_done) {
    esp_lcd_panel_io_event_data_t event = {};
    io->on_color_trans_done(io, &event, io->user_ctx);
  }
  return ESP_OK;
}

esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io) {
  if (!io) return ESP_ERR_INVALID_ARG;
  io->on_color_trans_done = nullptr;
  io->user_ctx = nullptr;
  return ESP_OK;
}

}  // extern "C"
