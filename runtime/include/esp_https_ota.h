#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* esp_https_ota_handle_t;
typedef esp_err_t (*http_client_init_cb_t)(esp_http_client_handle_t);

#define ESP_ERR_HTTPS_OTA_BASE (0x9000)
#define ESP_ERR_HTTPS_OTA_IN_PROGRESS (ESP_ERR_HTTPS_OTA_BASE + 1)

typedef struct {
  const esp_http_client_config_t* http_config;
  http_client_init_cb_t http_client_init_cb;
  bool bulk_flash_erase;
  bool partial_http_download;
  int max_http_request_size;
  uint32_t buffer_caps;
} esp_https_ota_config_t;

// Updates are not simulated. Beginning one logs that once and fails.
esp_err_t esp_https_ota(const esp_https_ota_config_t* ota_config);
esp_err_t esp_https_ota_begin(const esp_https_ota_config_t* ota_config,
                              esp_https_ota_handle_t* handle);
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t handle);
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t handle);
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t handle);
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t handle);
esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t handle, esp_app_desc_t* new_app_info);
int esp_https_ota_get_image_len_read(esp_https_ota_handle_t handle);

#ifdef __cplusplus
}
#endif
