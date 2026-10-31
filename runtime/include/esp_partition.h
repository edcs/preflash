#pragma once

#include <stdbool.h>
#include <stdint.h>

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
} esp_partition_subtype_t;

// There is no flash to describe, so the chip pointer is left out.
typedef struct {
  esp_partition_type_t type;
  esp_partition_subtype_t subtype;
  uint32_t address;
  uint32_t size;
  uint32_t erase_size;
  char label[17];
  bool encrypted;
  bool readonly;
} esp_partition_t;

#ifdef __cplusplus
}
#endif
