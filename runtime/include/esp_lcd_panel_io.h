#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char reserved;  // empty on a device; C++ would size an empty struct differently from C
} esp_lcd_panel_io_event_data_t;

typedef bool (*esp_lcd_panel_io_color_trans_done_cb_t)(esp_lcd_panel_io_handle_t panel_io,
                                                       esp_lcd_panel_io_event_data_t* edata,
                                                       void* user_ctx);

typedef struct {
  esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
} esp_lcd_panel_io_callbacks_t;

typedef int esp_lcd_spi_bus_handle_t;

typedef struct {
  int cs_gpio_num;
  int dc_gpio_num;
  int spi_mode;
  unsigned int pclk_hz;
  size_t trans_queue_depth;
  esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
  void* user_ctx;
  int lcd_cmd_bits;
  int lcd_param_bits;
  uint8_t cs_ena_pretrans;
  uint8_t cs_ena_posttrans;
  struct {
    unsigned int dc_high_on_cmd : 1;
    unsigned int dc_low_on_data : 1;
    unsigned int dc_low_on_param : 1;
    unsigned int octal_mode : 1;
    unsigned int quad_mode : 1;
    unsigned int sio_mode : 1;
    unsigned int lsb_first : 1;
    unsigned int cs_high_active : 1;
  } flags;
} esp_lcd_panel_io_spi_config_t;

// The bus must have been started. The wiring in the config means nothing to the virtual panel.
esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                   const esp_lcd_panel_io_spi_config_t* io_config,
                                   esp_lcd_panel_io_handle_t* ret_io);
// The callback runs on the caller's thread, as esp_lcd_panel_draw_bitmap finishes.
esp_err_t esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io,
                                                    const esp_lcd_panel_io_callbacks_t* cbs,
                                                    void* user_ctx);
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io);

#ifdef __cplusplus
}
#endif
