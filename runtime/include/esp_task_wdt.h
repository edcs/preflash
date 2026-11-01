#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t timeout_ms;
  uint32_t idle_core_mask;
  bool trigger_panic;
} esp_task_wdt_config_t;

// Watches subscribed tasks, as the device does, and needs CONFIG_ESP_TASK_WDT_INIT
// as the device does. A host has no starved idle task, so idle_core_mask is only
// checked. A panic stops the process rather than restarting it, so that it shows.
esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t* config);
esp_err_t esp_task_wdt_add(TaskHandle_t task_handle);
esp_err_t esp_task_wdt_reset(void);
esp_err_t esp_task_wdt_delete(TaskHandle_t task_handle);

#ifdef __cplusplus
}
#endif
