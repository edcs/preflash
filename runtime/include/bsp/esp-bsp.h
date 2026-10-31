#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bsp/display.h"
#include "esp_err.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#define BSP_CAPS_DISPLAY 1
#define BSP_CAPS_TOUCH 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  lvgl_port_cfg_t lvgl_port_cfg;
  uint32_t buffer_size;
  uint32_t trans_size;
  bool double_buffer;
  struct {
    unsigned int buff_dma : 1;
    unsigned int buff_spiram : 1;
  } flags;
} bsp_display_cfg_t;

lv_display_t* bsp_display_start(void);
lv_display_t* bsp_display_start_with_config(const bsp_display_cfg_t* cfg);
lv_indev_t* bsp_display_get_input_dev(void);
bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);
void bsp_display_rotate(lv_display_t* disp, lv_display_rotation_t rotation);

#ifdef __cplusplus
}
#endif
