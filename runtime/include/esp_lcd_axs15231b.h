#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int cmd;
  const void* data;
  size_t data_bytes;
  unsigned int delay_ms;
} axs15231b_lcd_init_cmd_t;

typedef struct {
  const axs15231b_lcd_init_cmd_t* init_cmds;
  uint16_t init_cmds_size;
  bool init_in_command_mode;
  struct {
    unsigned int use_qspi_interface : 1;
    unsigned int use_mipi_interface : 1;
  } flags;
} axs15231b_vendor_config_t;

// Gives firmware the board's one virtual panel. Init commands go nowhere, because the
// panel has no registers to set. The driver's own touch calls aren't here: the chip
// answers on I2C from the board file.
esp_err_t esp_lcd_new_panel_axs15231b(const esp_lcd_panel_io_handle_t io,
                                      const esp_lcd_panel_dev_config_t* panel_dev_config,
                                      esp_lcd_panel_handle_t* ret_panel);

#ifdef __cplusplus
}
#endif

#define AXS15231B_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz) \
  {                                                                         \
    .data0_io_num = d0, .data1_io_num = d1, .sclk_io_num = sclk,            \
    .data2_io_num = d2, .data3_io_num = d3, .max_transfer_sz = max_trans_sz \
  }

#define AXS15231B_PANEL_IO_QSPI_CONFIG(cs, cb, cb_ctx) \
  {                                                    \
    .cs_gpio_num = cs,                                 \
    .dc_gpio_num = -1,                                 \
    .spi_mode = 3,                                     \
    .pclk_hz = 40 * 1000 * 1000,                       \
    .trans_queue_depth = 10,                           \
    .on_color_trans_done = cb,                         \
    .user_ctx = cb_ctx,                                \
    .lcd_cmd_bits = 32,                                \
    .lcd_param_bits = 8,                               \
    .flags = {.quad_mode = true},                      \
  }
