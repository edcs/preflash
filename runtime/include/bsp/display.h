#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"

// Calls, because the size comes from the board file rather than the compile.
#define BSP_LCD_H_RES (preflash_lcd_width())
#define BSP_LCD_V_RES (preflash_lcd_height())

#ifdef __cplusplus
extern "C" {
#endif

int preflash_lcd_width(void);
int preflash_lcd_height(void);

typedef struct {
  int max_transfer_sz;
} bsp_display_config_t;

esp_err_t bsp_display_new(const bsp_display_config_t* config, esp_lcd_panel_handle_t* ret_panel,
                          esp_lcd_panel_io_handle_t* ret_io);
esp_err_t bsp_display_brightness_init(void);
esp_err_t bsp_display_brightness_set(int brightness_percent);
int bsp_display_brightness_get(void);
esp_err_t bsp_display_backlight_on(void);
esp_err_t bsp_display_backlight_off(void);

#ifdef __cplusplus
}
#endif
