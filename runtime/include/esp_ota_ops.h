#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_ERR_OTA_BASE 0x1500
#define ESP_ERR_OTA_PARTITION_CONFLICT (ESP_ERR_OTA_BASE + 0x01)
#define ESP_ERR_OTA_SELECT_INFO_INVALID (ESP_ERR_OTA_BASE + 0x02)
#define ESP_ERR_OTA_VALIDATE_FAILED (ESP_ERR_OTA_BASE + 0x03)
#define ESP_ERR_OTA_SMALL_SEC_VER (ESP_ERR_OTA_BASE + 0x04)
#define ESP_ERR_OTA_ROLLBACK_FAILED (ESP_ERR_OTA_BASE + 0x05)
#define ESP_ERR_OTA_ROLLBACK_INVALID_STATE (ESP_ERR_OTA_BASE + 0x06)

typedef enum {
  ESP_OTA_IMG_NEW = 0x0U,
  ESP_OTA_IMG_PENDING_VERIFY = 0x1U,
  ESP_OTA_IMG_VALID = 0x2U,
  ESP_OTA_IMG_INVALID = 0x3U,
  ESP_OTA_IMG_ABORTED = 0x4U,
  ESP_OTA_IMG_UNDEFINED = 0xFFFFFFFFU,
} esp_ota_img_states_t;

// Updates are not simulated: the running app is always valid and the spare slot is
// always empty. The calls that would change a slot say so once, in the log, and fail.
const esp_partition_t* esp_ota_get_running_partition(void);
const esp_partition_t* esp_ota_get_boot_partition(void);
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* start_from);
const esp_partition_t* esp_ota_get_last_invalid_partition(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t* partition,
                                      esp_ota_img_states_t* ota_state);
esp_err_t esp_ota_get_partition_description(const esp_partition_t* partition,
                                            esp_app_desc_t* app_desc);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* partition);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);

#ifdef __cplusplus
}
#endif
