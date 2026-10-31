#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_lcd_touch_s* esp_lcd_touch_handle_t;

typedef struct {
  void* dummy;
} bsp_touch_config_t;

// Touches reach LVGL through lvgl_port_add_disp, not a touch driver, so this
// stops the run rather than hand back a handle that does nothing.
esp_err_t bsp_touch_new(const bsp_touch_config_t* config, esp_lcd_touch_handle_t* ret_touch);

#ifdef __cplusplus
}
#endif
