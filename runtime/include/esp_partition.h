#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ESP_PARTITION_TYPE_APP = 0x00,
  ESP_PARTITION_TYPE_DATA = 0x01,
} esp_partition_type_t;

typedef enum {
  ESP_PARTITION_SUBTYPE_APP_FACTORY = 0x00,
  ESP_PARTITION_SUBTYPE_APP_OTA_MIN = 0x10,
  ESP_PARTITION_SUBTYPE_APP_OTA_0 = 0x10,
  ESP_PARTITION_SUBTYPE_APP_OTA_1 = 0x11,
  ESP_PARTITION_SUBTYPE_DATA_UNDEFINED = 0x06,
} esp_partition_subtype_t;

typedef struct {
  esp_flash_t* flash_chip;
  esp_partition_type_t type;
  esp_partition_subtype_t subtype;
  uint32_t address;
  uint32_t size;
  uint32_t erase_size;
  char label[17];
  bool encrypted;
  bool readonly;
} esp_partition_t;

esp_err_t esp_partition_register_external(esp_flash_t* flash_chip, size_t offset, size_t size,
                                          const char* label, esp_partition_type_t type,
                                          esp_partition_subtype_t subtype,
                                          const esp_partition_t** out_partition);
esp_err_t esp_partition_deregister_external(const esp_partition_t* partition);
esp_err_t esp_partition_read(const esp_partition_t* partition, size_t src_offset, void* dst,
                             size_t size);
esp_err_t esp_partition_write(const esp_partition_t* partition, size_t dst_offset, const void* src,
                              size_t size);
esp_err_t esp_partition_erase_range(const esp_partition_t* partition, size_t offset, size_t size);

#ifdef __cplusplus
}
#endif
