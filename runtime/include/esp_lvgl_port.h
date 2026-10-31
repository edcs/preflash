#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int task_priority;
  int task_stack;
  int task_affinity;
  int task_max_sleep_ms;
  unsigned task_stack_caps;
  int timer_period_ms;
} lvgl_port_cfg_t;

#define ESP_LVGL_PORT_INIT_CONFIG()                               \
  {                                                               \
    .task_priority = 4,                                           \
    .task_stack = 7168,                                           \
    .task_affinity = -1,                                          \
    .task_max_sleep_ms = 500,                                     \
    .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,  \
    .timer_period_ms = 5,                                         \
  }

typedef struct {
  bool swap_xy;
  bool mirror_x;
  bool mirror_y;
} lvgl_port_rotation_cfg_t;

typedef void (*lvgl_port_rounder_cb_t)(lv_area_t* area);

typedef struct {
  esp_lcd_panel_io_handle_t io_handle;
  esp_lcd_panel_handle_t panel_handle;
  esp_lcd_panel_handle_t control_handle;
  uint32_t buffer_size;
  bool double_buffer;
  uint32_t trans_size;
  uint32_t hres;
  uint32_t vres;
  bool monochrome;
  lvgl_port_rotation_cfg_t rotation;
  lvgl_port_rounder_cb_t rounder_cb;
  lv_color_format_t color_format;
  struct {
    unsigned int buff_dma : 1;
    unsigned int buff_spiram : 1;
    unsigned int sw_rotate : 1;
    unsigned int swap_bytes : 1;
    unsigned int full_refresh : 1;
    unsigned int direct_mode : 1;
  } flags;
} lvgl_port_display_cfg_t;

// Starts LVGL and the task that runs it.
esp_err_t lvgl_port_init(const lvgl_port_cfg_t* cfg);

// A timeout of 0 waits for as long as it takes. The lock is recursive.
bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);

// Makes an LVGL display drawing on the virtual panel, and the touch input that
// goes with it, since the panel's touches have nowhere else to arrive.
lv_display_t* lvgl_port_add_disp(const lvgl_port_display_cfg_t* disp_cfg);

#ifdef __cplusplus
}
#endif
