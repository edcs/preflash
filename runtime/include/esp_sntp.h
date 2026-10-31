#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ESP_SNTP_OPMODE_POLL,
  ESP_SNTP_OPMODE_LISTENONLY,
} esp_sntp_operatingmode_t;

typedef enum {
  SNTP_SYNC_MODE_IMMED,
  SNTP_SYNC_MODE_SMOOTH,
} sntp_sync_mode_t;

typedef enum {
  SNTP_SYNC_STATUS_RESET,
  SNTP_SYNC_STATUS_COMPLETED,
  SNTP_SYNC_STATUS_IN_PROGRESS,
} sntp_sync_status_t;

typedef void (*sntp_sync_time_cb_t)(struct timeval* tv);

void esp_sntp_setoperatingmode(esp_sntp_operatingmode_t operating_mode);
void esp_sntp_setservername(uint8_t idx, const char* server);
const char* esp_sntp_getservername(uint8_t idx);

// The Mac's clock is already right, so a sync reports shortly after init and the
// system clock is left alone. Only the callback and the status show it.
void esp_sntp_init(void);
void esp_sntp_stop(void);
bool esp_sntp_enabled(void);

void sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t callback);
void sntp_set_sync_mode(sntp_sync_mode_t sync_mode);
sntp_sync_mode_t sntp_get_sync_mode(void);
// Reading COMPLETED resets it, as on the device.
sntp_sync_status_t sntp_get_sync_status(void);
void sntp_set_sync_status(sntp_sync_status_t sync_status);
void sntp_restart(void);
uint32_t sntp_get_sync_interval(void);
void sntp_set_sync_interval(uint32_t interval_ms);

#ifdef __cplusplus
}
#endif
