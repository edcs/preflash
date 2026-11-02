#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_lcd_panel_t* esp_lcd_panel_handle_t;
typedef struct esp_lcd_panel_io_t* esp_lcd_panel_io_handle_t;

typedef enum {
  LCD_RGB_ELEMENT_ORDER_RGB = 0,
  LCD_RGB_ELEMENT_ORDER_BGR = 1,
} lcd_rgb_element_order_t;

typedef enum {
  LCD_RGB_DATA_ENDIAN_BIG = 0,
  LCD_RGB_DATA_ENDIAN_LITTLE,
} lcd_rgb_data_endian_t;

#ifdef __cplusplus
}
#endif
