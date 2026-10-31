#include <chrono>
#include <condition_variable>
#include <mutex>
#include <system_error>
#include <thread>

#include "freertos/task.h"
#include "ticks.h"

// A task is a detached thread that owns one notification count. Tasks are
// never freed: the handle must stay valid for as long as anyone holds it, and
// FreeRTOS firmware that deletes tasks isn't supported yet.
struct tskTaskControlBlock {
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

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char*, uint32_t, void* parameter,
                                   UBaseType_t, TaskHandle_t* created, BaseType_t) {
  if (!task) return pdFAIL;
  auto* block = new tskTaskControlBlock;
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

void vTaskDelay(TickType_t ticks) {
  // FreeRTOS treats a zero delay as a yield; sleeping for zero may not reschedule.
  if (ticks == 0) {
    std::this_thread::yield();
    return;
  }
  std::this_thread::sleep_for(preflash::ticks_to_duration(ticks));
}

BaseType_t xTaskNotifyGive(TaskHandle_t task) {
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
