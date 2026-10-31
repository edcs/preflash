#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_io_expander_s esp_io_expander_t;
typedef esp_io_expander_t* esp_io_expander_handle_t;

typedef enum {
    IO_EXPANDER_PIN_NUM_0 = (1ULL << 0),
    IO_EXPANDER_PIN_NUM_1 = (1ULL << 1),
    IO_EXPANDER_PIN_NUM_2 = (1ULL << 2),
    IO_EXPANDER_PIN_NUM_3 = (1ULL << 3),
    IO_EXPANDER_PIN_NUM_4 = (1ULL << 4),
    IO_EXPANDER_PIN_NUM_5 = (1ULL << 5),
    IO_EXPANDER_PIN_NUM_6 = (1ULL << 6),
    IO_EXPANDER_PIN_NUM_7 = (1ULL << 7),
    IO_EXPANDER_PIN_NUM_8 = (1ULL << 8),
    IO_EXPANDER_PIN_NUM_9 = (1ULL << 9),
    IO_EXPANDER_PIN_NUM_10 = (1ULL << 10),
    IO_EXPANDER_PIN_NUM_11 = (1ULL << 11),
    IO_EXPANDER_PIN_NUM_12 = (1ULL << 12),
    IO_EXPANDER_PIN_NUM_13 = (1ULL << 13),
    IO_EXPANDER_PIN_NUM_14 = (1ULL << 14),
    IO_EXPANDER_PIN_NUM_15 = (1ULL << 15),
    IO_EXPANDER_PIN_NUM_16 = (1ULL << 16),
    IO_EXPANDER_PIN_NUM_17 = (1ULL << 17),
    IO_EXPANDER_PIN_NUM_18 = (1ULL << 18),
    IO_EXPANDER_PIN_NUM_19 = (1ULL << 19),
    IO_EXPANDER_PIN_NUM_20 = (1ULL << 20),
    IO_EXPANDER_PIN_NUM_21 = (1ULL << 21),
    IO_EXPANDER_PIN_NUM_22 = (1ULL << 22),
    IO_EXPANDER_PIN_NUM_23 = (1ULL << 23),
    IO_EXPANDER_PIN_NUM_24 = (1ULL << 24),
    IO_EXPANDER_PIN_NUM_25 = (1ULL << 25),
    IO_EXPANDER_PIN_NUM_26 = (1ULL << 26),
    IO_EXPANDER_PIN_NUM_27 = (1ULL << 27),
    IO_EXPANDER_PIN_NUM_28 = (1ULL << 28),
    IO_EXPANDER_PIN_NUM_29 = (1ULL << 29),
    IO_EXPANDER_PIN_NUM_30 = (1ULL << 30),
    IO_EXPANDER_PIN_NUM_31 = (1ULL << 31),
    IO_EXPANDER_PIN_NUM_MAX,
} esp_io_expander_pin_num_t;

typedef enum {
  IO_EXPANDER_INPUT,
  IO_EXPANDER_OUTPUT,
} esp_io_expander_dir_t;

esp_err_t esp_io_expander_set_dir(esp_io_expander_handle_t handle, uint32_t pin_num_mask,
                                  esp_io_expander_dir_t direction);
// As on the real driver, every pin in the mask must be an output first.
esp_err_t esp_io_expander_set_level(esp_io_expander_handle_t handle, uint32_t pin_num_mask,
                                    uint8_t level);
esp_err_t esp_io_expander_get_level(esp_io_expander_handle_t handle, uint32_t pin_num_mask,
                                    uint32_t* level_mask);

#ifdef __cplusplus
}
#endif
