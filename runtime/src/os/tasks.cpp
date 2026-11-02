#include <pthread.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

#include "esp_timer.h"
#include "freertos/task.h"
#include "preflash/fail.h"
#include "ticks.h"

// A task is a detached thread that owns one notification count. Blocks are
// never freed, even once a task has deleted itself: a handle must stay valid for
// as long as anyone holds it.
struct tskTaskControlBlock {
  std::string name;
  std::mutex mutex;
  std::condition_variable wake;
  uint32_t notifications = 0;
};

namespace {

thread_local tskTaskControlBlock* t_self = nullptr;

// Threads that weren't made by xTaskCreate (app_main's) get a block on first use.
tskTaskControlBlock* current_task() {
  if (!t_self) t_self = new tskTaskControlBlock;
  return t_self;
}

}  // namespace

extern "C" {

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char* name, uint32_t, void* parameter,
                                   UBaseType_t, TaskHandle_t* created, BaseType_t) {
  if (!task) return pdFAIL;
  auto* block = new tskTaskControlBlock;
  if (name) block->name = name;
  // Set before the thread starts, so the task can see its own handle.
  if (created) *created = block;
  try {
    std::thread([task, parameter, block] {
      t_self = block;
      task(parameter);
    }).detach();
  } catch (const std::system_error&) {
    if (created) *created = nullptr;
    delete block;
    return pdFAIL;
  }
  return pdPASS;
}

TickType_t xTaskGetTickCount(void) {
  return static_cast<TickType_t>(esp_timer_get_time() * configTICK_RATE_HZ / 1000000);
}

void vTaskDelayUntil(TickType_t* previous_wake, TickType_t increment) {
  const TickType_t wake = *previous_wake + increment;
  *previous_wake = wake;
  const TickType_t left = wake - xTaskGetTickCount();
  // Unsigned, so a wake time already gone shows as a huge number.
  if (left != 0 && left <= increment) std::this_thread::sleep_for(preflash::ticks_to_duration(left));
}

void vTaskDelay(TickType_t ticks) {
  // FreeRTOS treats a zero delay as a yield; sleeping for zero may not reschedule.
  if (ticks == 0) {
    std::this_thread::yield();
    return;
  }
  std::this_thread::sleep_for(preflash::ticks_to_duration(ticks));
}

void vTaskDelete(TaskHandle_t task) {
  // A host thread can't be stopped from outside without leaving its locks held.
  if (task && task != current_task()) {
    preflash_unsupported("vTaskDelete", "a task can only delete itself");
  }
  // Ends the thread here without unwinding it, as FreeRTOS would.
  pthread_exit(nullptr);
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current_task(); }

char* pcTaskGetName(TaskHandle_t task) {
  return (task ? task : current_task())->name.data();
}

BaseType_t xTaskNotifyGive(TaskHandle_t task) {
  if (!task) preflash_unsupported("xTaskNotifyGive", "the task handle is null");
  {
    std::lock_guard<std::mutex> lock(task->mutex);
    ++task->notifications;
  }
  task->wake.notify_one();
  return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_count_on_exit, TickType_t ticks_to_wait) {
  tskTaskControlBlock* self = current_task();
  std::unique_lock<std::mutex> lock(self->mutex);
  auto ready = [self] { return self->notifications > 0; };
  if (ticks_to_wait == portMAX_DELAY) {
    self->wake.wait(lock, ready);
  } else {
    self->wake.wait_for(lock, preflash::ticks_to_duration(ticks_to_wait), ready);
  }
  const uint32_t before = self->notifications;
  if (before > 0) self->notifications = clear_count_on_exit ? 0 : before - 1;
  return before;
}

}  // extern "C"
