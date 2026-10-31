#pragma once

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_ERR_WIFI_NOT_INIT (ESP_ERR_WIFI_BASE + 1)
#define ESP_ERR_WIFI_NOT_STARTED (ESP_ERR_WIFI_BASE + 2)
#define ESP_ERR_WIFI_NOT_STOPPED (ESP_ERR_WIFI_BASE + 3)
#define ESP_ERR_WIFI_IF (ESP_ERR_WIFI_BASE + 4)
#define ESP_ERR_WIFI_MODE (ESP_ERR_WIFI_BASE + 5)
#define ESP_ERR_WIFI_STATE (ESP_ERR_WIFI_BASE + 6)
#define ESP_ERR_WIFI_CONN (ESP_ERR_WIFI_BASE + 7)
#define ESP_ERR_WIFI_NVS (ESP_ERR_WIFI_BASE + 8)
#define ESP_ERR_WIFI_MAC (ESP_ERR_WIFI_BASE + 9)
#define ESP_ERR_WIFI_SSID (ESP_ERR_WIFI_BASE + 10)
#define ESP_ERR_WIFI_PASSWORD (ESP_ERR_WIFI_BASE + 11)
#define ESP_ERR_WIFI_TIMEOUT (ESP_ERR_WIFI_BASE + 12)
#define ESP_ERR_WIFI_NOT_CONNECT (ESP_ERR_WIFI_BASE + 15)

#define WIFI_INIT_CONFIG_MAGIC 0x1F2F3F4F

// The fields are the ones the stack keeps in its default, in the same order, so
// designated initialisers in firmware still compile.
typedef struct {
  int static_rx_buf_num;
  int dynamic_rx_buf_num;
  int tx_buf_type;
  int static_tx_buf_num;
  int dynamic_tx_buf_num;
  int cache_tx_buf_num;
  int csi_enable;
  int ampdu_rx_enable;
  int ampdu_tx_enable;
  int amsdu_tx_enable;
  int nvs_enable;
  int nano_enable;
  int rx_ba_win;
  int wifi_task_core_id;
  int beacon_max_len;
  int mgmt_sbuf_num;
  uint64_t feature_caps;
  bool sta_disconnected_pm;
  int espnow_max_encrypt_num;
  int magic;
} wifi_init_config_t;

#define WIFI_INIT_CONFIG_DEFAULT()                                                       \
  {                                                                                      \
    .static_rx_buf_num = 10, .dynamic_rx_buf_num = 32, .tx_buf_type = 1,                 \
    .static_tx_buf_num = 0, .dynamic_tx_buf_num = 32, .cache_tx_buf_num = 0,             \
    .csi_enable = 0, .ampdu_rx_enable = 1, .ampdu_tx_enable = 1, .amsdu_tx_enable = 0,   \
    .nvs_enable = 1, .nano_enable = 0, .rx_ba_win = 6, .wifi_task_core_id = 0,           \
    .beacon_max_len = 752, .mgmt_sbuf_num = 32, .feature_caps = 0,                       \
    .sta_disconnected_pm = false, .espnow_max_encrypt_num = 7,                           \
    .magic = WIFI_INIT_CONFIG_MAGIC                                                      \
  }

esp_err_t esp_wifi_init(const wifi_init_config_t* config);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(wifi_storage_t storage);
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_get_mode(wifi_mode_t* mode);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_set_config(wifi_interface_t interface, wifi_config_t* conf);
esp_err_t esp_wifi_get_config(wifi_interface_t interface, wifi_config_t* conf);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t type);

esp_err_t esp_wifi_scan_start(const wifi_scan_config_t* config, bool block);
esp_err_t esp_wifi_scan_stop(void);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t* number);
// Hands the results over and forgets them, as the device does.
esp_err_t esp_wifi_scan_get_ap_records(uint16_t* number, wifi_ap_record_t* ap_records);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t* ap_info);

#ifdef __cplusplus
}
#endif
