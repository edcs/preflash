#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_ERR_IMAGE_BASE 0x2000
#define ESP_ERR_IMAGE_FLASH_FAIL (ESP_ERR_IMAGE_BASE + 1)
#define ESP_ERR_IMAGE_INVALID (ESP_ERR_IMAGE_BASE + 2)

typedef struct {
  uint32_t offset;
  uint32_t size;
} esp_partition_pos_t;

typedef enum {
  ESP_IMAGE_VERIFY,
  ESP_IMAGE_VERIFY_SILENT,
} esp_image_load_mode_t;

typedef struct {
  uint32_t start_addr;
  uint32_t image_len;
} esp_image_metadata_t;

// The spare slot never holds an image, so nothing verifies.
esp_err_t esp_image_verify(esp_image_load_mode_t mode, const esp_partition_pos_t* part,
                           esp_image_metadata_t* data);

#ifdef __cplusplus
}
#endif
