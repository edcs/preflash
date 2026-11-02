#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int reset_gpio_num;
  lcd_rgb_element_order_t rgb_ele_order;
  lcd_rgb_data_endian_t data_endian;
  uint32_t bits_per_pixel;
  struct {
    uint32_t reset_active_high : 1;
  } flags;
  void* vendor_config;
} esp_lcd_panel_dev_config_t;

#ifdef __cplusplus
}
#endif
