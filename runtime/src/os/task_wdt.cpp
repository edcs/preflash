#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "preflash/fail.h"

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
  int64_t deadline = 0;  // when this round ends, or 0 with nothing to wait for
};

Watchdog& watchdog() {
  static Watchdog* w = new Watchdog;  // never destroyed: tasks outlive static destruction
  return *w;
}

void expired(void*);

// Reports whichever tasks haven't reset this round. False if none are late.
bool report_late(const Watchdog& w) {
  std::string late;
  for (const auto& [task, reset] : w.tasks) {
    if (!reset) late += std::string(" ") + pcTaskGetName(task);
  }
  if (late.empty()) return false;
  std::fprintf(stderr, "preflash: task watchdog: these tasks didn't reset in time:%s\n",
               late.c_str());
  if (w.panic) std::abort();
  return true;
}

// Starts a new round, and the wait for its end if there's a task to wait for.
esp_err_t feed(Watchdog& w) {
  if (!w.timer) {
    const esp_timer_create_args_t args = {expired, nullptr, ESP_TIMER_TASK, "task_wdt", false};
    const esp_err_t err = esp_timer_create(&args, &w.timer);
    if (err != ESP_OK) return err;
  }
  esp_timer_stop(w.timer);  // not armed is fine: there may have been nothing to wait for
  for (auto& [task, reset] : w.tasks) reset = false;
  w.deadline = 0;
  if (w.tasks.empty()) return ESP_OK;
  w.deadline = esp_timer_get_time() + static_cast<int64_t>(w.timeout_us);
  return esp_timer_start_once(w.timer, w.timeout_us);
}

void expired(void*) {
  Watchdog& w = watchdog();
  std::lock_guard<std::mutex> lock(w.mutex);
  // Fired just as the last task unsubscribed or reset: there's nobody to blame.
  if (!report_late(w)) return;
  if (feed(w) != ESP_OK) preflash_unsupported("esp_task_wdt", "the watchdog's timer stopped");
}

}  // namespace

extern "C" {

esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t* config) {
  if (!config || config->idle_core_mask >= (1U << CONFIG_FREERTOS_NUMBER_OF_CORES)) {
    return ESP_ERR_INVALID_ARG;
  }
  // The device's result depends on its hardware timer, and here it would fire forever.
  if (config->timeout_ms == 0) {
    preflash_unsupported("esp_task_wdt_reconfigure", "a timeout of 0 ms has no model");
  }
  Watchdog& w = watchdog();
  std::lock_guard<std::mutex> lock(w.mutex);
  w.timeout_us = config->timeout_ms * 1000ULL;
  w.panic = config->trigger_panic;
  return feed(w);
}

esp_err_t esp_task_wdt_add(TaskHandle_t task_handle) {
  if (!task_handle) task_handle = xTaskGetCurrentTaskHandle();
  Watchdog& w = watchdog();
  std::lock_guard<std::mutex> lock(w.mutex);
  if (w.tasks.count(task_handle)) return ESP_ERR_INVALID_ARG;
  w.tasks[task_handle] = false;
  const esp_err_t err = feed(w);
  if (err != ESP_OK) w.tasks.erase(task_handle);
  return err;
}

esp_err_t esp_task_wdt_reset(void) {
  Watchdog& w = watchdog();
  std::lock_guard<std::mutex> lock(w.mutex);
  auto entry = w.tasks.find(xTaskGetCurrentTaskHandle());
  if (entry == w.tasks.end()) return ESP_ERR_NOT_FOUND;
  // The timer task can be held up by another timer's callback, where the device's
  // hardware timer wouldn't be, so a reset that comes too late still counts as late.
  if (w.deadline != 0 && esp_timer_get_time() >= w.deadline) report_late(w);
  entry->second = true;
  for (const auto& [task, reset] : w.tasks) {
    if (!reset) return ESP_OK;  // the round ends when every task has reset
  }
  return feed(w);
}

esp_err_t esp_task_wdt_delete(TaskHandle_t task_handle) {
  if (!task_handle) task_handle = xTaskGetCurrentTaskHandle();
  Watchdog& w = watchdog();
  std::lock_guard<std::mutex> lock(w.mutex);
  if (!w.tasks.erase(task_handle)) return ESP_ERR_NOT_FOUND;
  return feed(w);
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
