#pragma once

#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ESP_LOG_NONE,
  ESP_LOG_ERROR,
  ESP_LOG_WARN,
  ESP_LOG_INFO,
  ESP_LOG_DEBUG,
  ESP_LOG_VERBOSE,
  ESP_LOG_MAX,
} esp_log_level_t;

uint32_t esp_log_timestamp(void);
void esp_log_write(esp_log_level_t level, const char* tag, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

#ifdef __cplusplus
}
#endif

#define ESP_LOGE(tag, format, ...) esp_log_write(ESP_LOG_ERROR, tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) esp_log_write(ESP_LOG_WARN, tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) esp_log_write(ESP_LOG_INFO, tag, format, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) esp_log_write(ESP_LOG_DEBUG, tag, format, ##__VA_ARGS__)
#define ESP_LOGV(tag, format, ...) esp_log_write(ESP_LOG_VERBOSE, tag, format, ##__VA_ARGS__)
