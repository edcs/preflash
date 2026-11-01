#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include "esp_task_wdt.h"
#include "esp_timer.h"

// Off unless the project starts the watchdog, when every call reports that it
// was never initialised, as on the device.
#ifdef CONFIG_ESP_TASK_WDT_INIT

namespace {

#ifndef CONFIG_FREERTOS_NUMBER_OF_CORES
#define CONFIG_FREERTOS_NUMBER_OF_CORES 2
#endif

#ifdef CONFIG_ESP_TASK_WDT_PANIC
constexpr bool kPanicAtBoot = true;
#else
constexpr bool kPanicAtBoot = false;
#endif

struct Watchdog {
  std::mutex mutex;
  std::map<TaskHandle_t, bool> tasks;  // each with whether it has reset this round
  uint64_t timeout_us = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000000ULL;
  bool panic = kPanicAtBoot;
  esp_timer_handle_t timer = nullptr;
};

Watchdog& watchdog() {
  static Watchdog* w = new Watchdog;  // never destroyed: tasks outlive static destruction
  return *w;
}

void expired(void*);

// Starts a new round, and the wait for its end if there's a task to wait for.
void feed(Watchdog& w) {
  if (!w.timer) {
    const esp_timer_create_args_t args = {expired, nullptr, ESP_TIMER_TASK, "task_wdt", false};
    esp_timer_create(&args, &w.timer);
  }
  esp_timer_stop(w.timer);
  for (auto& [task, reset] : w.tasks) reset = false;
  if (!w.tasks.empty()) esp_timer_start_once(w.timer, w.timeout_us);
}

void expired(void*) {
  Watchdog& w = watchdog();
  std::lock_guard<std::mutex> lock(w.mutex);
  std::string late;
  for (const auto& [task, reset] : w.tasks) {
    if (!reset) late += std::string(" ") + pcTaskGetName(task);
  }
  // Fired just as the last task unsubscribed or reset: there's nobody to blame.
  if (late.empty()) return;
  std::fprintf(stderr, "preflash: task watchdog: these tasks didn't reset in time:%s\n",
               late.c_str());
  if (w.panic) std::abort();
  feed(w);
}

}  // namespace

extern "C" {

esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t* config) {
  if (!config || config->idle_core_mask >= (1U << CONFIG_FREERTOS_NUMBER_OF_CORES)) {
    return ESP_ERR_INVALID_ARG;
  }
  Watchdog& w = watchdog();
  std::lock_guard<std::mutex> lock(w.mutex);
  w.timeout_us = config->timeout_ms * 1000ULL;
  w.panic = config->trigger_panic;
  feed(w);
  return ESP_OK;
}

esp_err_t esp_task_wdt_add(TaskHandle_t task_handle) {
  if (!task_handle) task_handle = xTaskGetCurrentTaskHandle();
  Watchdog& w = watchdog();
  std::lock_guard<std::mutex> lock(w.mutex);
  if (w.tasks.count(task_handle)) return ESP_ERR_INVALID_ARG;
  w.tasks[task_handle] = false;
  feed(w);
  return ESP_OK;
}

esp_err_t esp_task_wdt_reset(void) {
  Watchdog& w = watchdog();
  std::lock_guard<std::mutex> lock(w.mutex);
  auto entry = w.tasks.find(xTaskGetCurrentTaskHandle());
  if (entry == w.tasks.end()) return ESP_ERR_NOT_FOUND;
  entry->second = true;
  for (const auto& [task, reset] : w.tasks) {
    if (!reset) return ESP_OK;  // the round ends when every task has reset
  }
  feed(w);
  return ESP_OK;
}

esp_err_t esp_task_wdt_delete(TaskHandle_t task_handle) {
  if (!task_handle) task_handle = xTaskGetCurrentTaskHandle();
  Watchdog& w = watchdog();
  std::lock_guard<std::mutex> lock(w.mutex);
  if (!w.tasks.erase(task_handle)) return ESP_ERR_NOT_FOUND;
  feed(w);
  return ESP_OK;
}

}  // extern "C"

#else

extern "C" {
esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t* config) {
  return config ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
}
esp_err_t esp_task_wdt_add(TaskHandle_t) { return ESP_ERR_INVALID_STATE; }
esp_err_t esp_task_wdt_reset(void) { return ESP_ERR_INVALID_STATE; }
esp_err_t esp_task_wdt_delete(TaskHandle_t) { return ESP_ERR_INVALID_STATE; }
}

#endif
