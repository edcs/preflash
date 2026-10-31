#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_event_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  WIFI_MODE_NULL = 0,
  WIFI_MODE_STA,
  WIFI_MODE_AP,
  WIFI_MODE_APSTA,
  WIFI_MODE_NAN,
  WIFI_MODE_MAX
} wifi_mode_t;

typedef enum {
  WIFI_IF_STA = 0,
  WIFI_IF_AP,
  WIFI_IF_NAN,
  WIFI_IF_MAX
} wifi_interface_t;

typedef enum { WIFI_STORAGE_FLASH, WIFI_STORAGE_RAM } wifi_storage_t;

typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;

typedef enum {
  WIFI_AUTH_OPEN = 0,
  WIFI_AUTH_WEP,
  WIFI_AUTH_WPA_PSK,
  WIFI_AUTH_WPA2_PSK,
  WIFI_AUTH_WPA_WPA2_PSK,
  WIFI_AUTH_ENTERPRISE,
  WIFI_AUTH_WPA2_ENTERPRISE = WIFI_AUTH_ENTERPRISE,
  WIFI_AUTH_WPA3_PSK,
  WIFI_AUTH_WPA2_WPA3_PSK,
  WIFI_AUTH_WAPI_PSK,
  WIFI_AUTH_OWE,
  WIFI_AUTH_WPA3_ENT_192,
  WIFI_AUTH_WPA3_EXT_PSK,
  WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE,
  WIFI_AUTH_DPP,
  WIFI_AUTH_WPA3_ENTERPRISE,
  WIFI_AUTH_WPA2_WPA3_ENTERPRISE,
  WIFI_AUTH_MAX
} wifi_auth_mode_t;

typedef enum {
  WIFI_CIPHER_TYPE_NONE = 0,
  WIFI_CIPHER_TYPE_WEP40,
  WIFI_CIPHER_TYPE_WEP104,
  WIFI_CIPHER_TYPE_TKIP,
  WIFI_CIPHER_TYPE_CCMP,
  WIFI_CIPHER_TYPE_TKIP_CCMP,
  WIFI_CIPHER_TYPE_AES_CMAC128,
  WIFI_CIPHER_TYPE_SMS4,
  WIFI_CIPHER_TYPE_GCMP,
  WIFI_CIPHER_TYPE_GCMP256,
  WIFI_CIPHER_TYPE_AES_GMAC128,
  WIFI_CIPHER_TYPE_AES_GMAC256,
  WIFI_CIPHER_TYPE_UNKNOWN,
} wifi_cipher_type_t;

typedef enum {
  WIFI_SECOND_CHAN_NONE = 0,
  WIFI_SECOND_CHAN_ABOVE,
  WIFI_SECOND_CHAN_BELOW,
} wifi_second_chan_t;

typedef enum { WIFI_ALL_CHANNEL_SCAN = 0, WIFI_FAST_SCAN } wifi_scan_method_t;
typedef enum { WIFI_CONNECT_AP_BY_SIGNAL = 0, WIFI_CONNECT_AP_BY_SECURITY } wifi_sort_method_t;
typedef enum { WIFI_SCAN_TYPE_ACTIVE = 0, WIFI_SCAN_TYPE_PASSIVE } wifi_scan_type_t;

typedef struct {
  uint16_t min;
  uint16_t max;
} wifi_active_scan_time_t;

typedef struct {
  wifi_active_scan_time_t active;
  uint32_t passive;
} wifi_scan_time_t;

typedef struct {
  uint8_t* ssid;
  uint8_t* bssid;
  uint8_t channel;
  bool show_hidden;
  wifi_scan_type_t scan_type;
  wifi_scan_time_t scan_time;
  uint8_t home_chan_dwell_time;
} wifi_scan_config_t;

typedef struct {
  int8_t rssi;
  wifi_auth_mode_t authmode;
} wifi_scan_threshold_t;

typedef struct {
  bool capable;
  bool required;
} wifi_pmf_config_t;

typedef struct {
  uint8_t ssid[32];
  uint8_t password[64];
  wifi_scan_method_t scan_method;
  bool bssid_set;
  uint8_t bssid[6];
  uint8_t channel;
  uint16_t listen_interval;
  wifi_sort_method_t sort_method;
  wifi_scan_threshold_t threshold;
  wifi_pmf_config_t pmf_cfg;
  uint32_t rm_enabled : 1;
  uint32_t btm_enabled : 1;
  uint32_t mbo_enabled : 1;
  uint32_t ft_enabled : 1;
  uint32_t owe_enabled : 1;
  uint32_t transition_disable : 1;
  uint32_t reserved : 26;
  uint8_t failure_retry_cnt;
} wifi_sta_config_t;

// Only the station half exists: the access point is not simulated.
typedef union {
  wifi_sta_config_t sta;
} wifi_config_t;

typedef struct {
  uint8_t bssid[6];
  uint8_t ssid[33];
  uint8_t primary;
  wifi_second_chan_t second;
  int8_t rssi;
  wifi_auth_mode_t authmode;
  wifi_cipher_type_t pairwise_cipher;
  wifi_cipher_type_t group_cipher;
} wifi_ap_record_t;

ESP_EVENT_DECLARE_BASE(WIFI_EVENT);

typedef enum {
  WIFI_EVENT_WIFI_READY = 0,
  WIFI_EVENT_SCAN_DONE,
  WIFI_EVENT_STA_START,
  WIFI_EVENT_STA_STOP,
  WIFI_EVENT_STA_CONNECTED,
  WIFI_EVENT_STA_DISCONNECTED,
  WIFI_EVENT_STA_AUTHMODE_CHANGE,
} wifi_event_t;

typedef struct {
  uint32_t status;  // 0 for success, 1 for failure
  uint8_t number;
  uint8_t scan_id;
} wifi_event_sta_scan_done_t;

typedef struct {
  uint8_t ssid[32];
  uint8_t ssid_len;
  uint8_t bssid[6];
  uint8_t channel;
  wifi_auth_mode_t authmode;
  uint16_t aid;
} wifi_event_sta_connected_t;

typedef struct {
  uint8_t ssid[32];
  uint8_t ssid_len;
  uint8_t bssid[6];
  uint8_t reason;
  int8_t rssi;
} wifi_event_sta_disconnected_t;

typedef enum {
  WIFI_REASON_UNSPECIFIED = 1,
  WIFI_REASON_AUTH_EXPIRE = 2,
  WIFI_REASON_AUTH_LEAVE = 3,
  WIFI_REASON_ASSOC_EXPIRE = 4,
  WIFI_REASON_ASSOC_TOOMANY = 5,
  WIFI_REASON_NOT_AUTHED = 6,
  WIFI_REASON_NOT_ASSOCED = 7,
  WIFI_REASON_ASSOC_LEAVE = 8,
  WIFI_REASON_ASSOC_NOT_AUTHED = 9,
  WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15,
  WIFI_REASON_BEACON_TIMEOUT = 200,
  WIFI_REASON_NO_AP_FOUND = 201,
  WIFI_REASON_AUTH_FAIL = 202,
  WIFI_REASON_ASSOC_FAIL = 203,
  WIFI_REASON_HANDSHAKE_TIMEOUT = 204,
  WIFI_REASON_CONNECTION_FAIL = 205,
} wifi_err_reason_t;

#ifdef __cplusplus
}
#endif
