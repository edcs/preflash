#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_flash_t {
  uint32_t size;
} esp_flash_t;

extern esp_flash_t* esp_flash_default_chip;

// A null chip means the default one.
esp_err_t esp_flash_get_size(esp_flash_t* chip, uint32_t* out_size);

#ifdef __cplusplus
}
#endif
