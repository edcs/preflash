#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_intr_alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

// The ESP32-S3's pins, which have no GPIO 22 to 25.
typedef enum {
  GPIO_NUM_NC = -1,
  GPIO_NUM_0 = 0,
  GPIO_NUM_1 = 1,
  GPIO_NUM_2 = 2,
  GPIO_NUM_3 = 3,
  GPIO_NUM_4 = 4,
  GPIO_NUM_5 = 5,
  GPIO_NUM_6 = 6,
  GPIO_NUM_7 = 7,
  GPIO_NUM_8 = 8,
  GPIO_NUM_9 = 9,
  GPIO_NUM_10 = 10,
  GPIO_NUM_11 = 11,
  GPIO_NUM_12 = 12,
  GPIO_NUM_13 = 13,
  GPIO_NUM_14 = 14,
  GPIO_NUM_15 = 15,
  GPIO_NUM_16 = 16,
  GPIO_NUM_17 = 17,
  GPIO_NUM_18 = 18,
  GPIO_NUM_19 = 19,
  GPIO_NUM_20 = 20,
  GPIO_NUM_21 = 21,
  GPIO_NUM_26 = 26,
  GPIO_NUM_27 = 27,
  GPIO_NUM_28 = 28,
  GPIO_NUM_29 = 29,
  GPIO_NUM_30 = 30,
  GPIO_NUM_31 = 31,
  GPIO_NUM_32 = 32,
  GPIO_NUM_33 = 33,
  GPIO_NUM_34 = 34,
  GPIO_NUM_35 = 35,
  GPIO_NUM_36 = 36,
  GPIO_NUM_37 = 37,
  GPIO_NUM_38 = 38,
  GPIO_NUM_39 = 39,
  GPIO_NUM_40 = 40,
  GPIO_NUM_41 = 41,
  GPIO_NUM_42 = 42,
  GPIO_NUM_43 = 43,
  GPIO_NUM_44 = 44,
  GPIO_NUM_45 = 45,
  GPIO_NUM_46 = 46,
  GPIO_NUM_47 = 47,
  GPIO_NUM_48 = 48,
  GPIO_NUM_MAX,
} gpio_num_t;

// Bit 0 is input, bit 1 output and bit 2 open drain, as in ESP-IDF.
typedef enum {
  GPIO_MODE_DISABLE = 0,
  GPIO_MODE_INPUT = 1,
  GPIO_MODE_OUTPUT = 2,
  GPIO_MODE_OUTPUT_OD = 6,
  GPIO_MODE_INPUT_OUTPUT_OD = 7,
  GPIO_MODE_INPUT_OUTPUT = 3,
} gpio_mode_t;

typedef enum {
  GPIO_PULLUP_DISABLE = 0,
  GPIO_PULLUP_ENABLE = 1,
} gpio_pullup_t;

typedef enum {
  GPIO_PULLDOWN_DISABLE = 0,
  GPIO_PULLDOWN_ENABLE = 1,
} gpio_pulldown_t;

typedef enum {
  GPIO_PULLUP_ONLY,
  GPIO_PULLDOWN_ONLY,
  GPIO_PULLUP_PULLDOWN,
  GPIO_FLOATING,
} gpio_pull_mode_t;

typedef enum {
  GPIO_INTR_DISABLE = 0,
  GPIO_INTR_POSEDGE = 1,
  GPIO_INTR_NEGEDGE = 2,
  GPIO_INTR_ANYEDGE = 3,
  GPIO_INTR_LOW_LEVEL = 4,
  GPIO_INTR_HIGH_LEVEL = 5,
  GPIO_INTR_MAX,
} gpio_int_type_t;

typedef struct {
  uint64_t pin_bit_mask;
  gpio_mode_t mode;
  gpio_pullup_t pull_up_en;
  gpio_pulldown_t pull_down_en;
  gpio_int_type_t intr_type;
} gpio_config_t;

esp_err_t gpio_config(const gpio_config_t* config);
esp_err_t gpio_reset_pin(gpio_num_t gpio_num);
esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode);
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);
// 0 for a pin without its input on, as on the device.
int gpio_get_level(gpio_num_t gpio_num);
esp_err_t gpio_set_pull_mode(gpio_num_t gpio_num, gpio_pull_mode_t pull);
esp_err_t gpio_pullup_en(gpio_num_t gpio_num);
esp_err_t gpio_pullup_dis(gpio_num_t gpio_num);
esp_err_t gpio_pulldown_en(gpio_num_t gpio_num);
esp_err_t gpio_pulldown_dis(gpio_num_t gpio_num);
esp_err_t gpio_hold_en(gpio_num_t gpio_num);
esp_err_t gpio_hold_dis(gpio_num_t gpio_num);

// Interrupts. A level change that matches the pin's type calls its handler from a thread
// standing in for the ISR, whoever moved the pin (see gpio.cpp).
typedef void (*gpio_isr_t)(void* arg);
typedef struct intr_handle_data_t* gpio_isr_handle_t;

esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type);
esp_err_t gpio_intr_enable(gpio_num_t gpio_num);
esp_err_t gpio_intr_disable(gpio_num_t gpio_num);
// ESP_ERR_INVALID_STATE if the service is already installed, ESP_ERR_NO_MEM if its thread can't
// start. The flags are ignored.
esp_err_t gpio_install_isr_service(int intr_alloc_flags);
void gpio_uninstall_isr_service(void);
// ESP_ERR_INVALID_STATE before the service is installed. Adding a handler enables the pin's
// interrupt, and removing it disables it, as in ESP-IDF.
esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void* args);
esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num);
// Unsupported: a firmware-wide GPIO ISR has no stand-in. Stops the firmware.
esp_err_t gpio_isr_register(void (*fn)(void*), void* arg, int intr_alloc_flags,
                            gpio_isr_handle_t* handle);

#ifdef __cplusplus
}
#endif
