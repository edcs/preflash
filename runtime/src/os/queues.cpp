#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "preflash/fail.h"
#include "ticks.h"

// Items are copied in and out by value, as FreeRTOS does.
struct QueueDefinition {
  QueueDefinition(UBaseType_t length, UBaseType_t item_size)
      : length(length), item_size(item_size) {}
  const UBaseType_t length;
  const UBaseType_t item_size;
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::vector<uint8_t>> items;
};

extern "C" {

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
  if (length == 0) return nullptr;
  return new QueueDefinition(length, item_size);
}

BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t ticks_to_wait) {
  if (!queue) preflash_unsupported("xQueueSend", "the queue handle is null");
  std::unique_lock<std::mutex> lock(queue->mutex);
  auto room = [queue] { return queue->items.size() < queue->length; };
  if (ticks_to_wait == portMAX_DELAY) {
    queue->changed.wait(lock, room);
  } else if (!queue->changed.wait_for(lock, preflash::ticks_to_duration(ticks_to_wait), room)) {
    return pdFALSE;
  }
  const auto* bytes = static_cast<const uint8_t*>(item);
  queue->items.emplace_back(bytes, bytes + queue->item_size);
  lock.unlock();
  queue->changed.notify_all();
  return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t ticks_to_wait) {
  if (!queue) preflash_unsupported("xQueueReceive", "the queue handle is null");
  std::unique_lock<std::mutex> lock(queue->mutex);
  auto any = [queue] { return !queue->items.empty(); };
  if (ticks_to_wait == portMAX_DELAY) {
    queue->changed.wait(lock, any);
  } else if (!queue->changed.wait_for(lock, preflash::ticks_to_duration(ticks_to_wait), any)) {
    return pdFALSE;
  }
  if (queue->item_size > 0) std::memcpy(item, queue->items.front().data(), queue->item_size);
  queue->items.pop_front();
  lock.unlock();
  queue->changed.notify_all();
  return pdTRUE;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void) { return new QueueDefinition(1, 0); }

SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max_count, UBaseType_t initial_count) {
  if (max_count == 0 || initial_count > max_count) return nullptr;
  auto* semaphore = new QueueDefinition(max_count, 0);
  semaphore->items.resize(initial_count);
  return semaphore;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return xSemaphoreCreateCounting(1, 1); }

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
  return xQueueSend(semaphore, nullptr, 0);
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t semaphore, BaseType_t* task_woken) {
  if (task_woken) *task_woken = pdFALSE;  // the host's scheduler has already been told
  return xSemaphoreGive(semaphore);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait) {
  return xQueueReceive(semaphore, nullptr, ticks_to_wait);
}

UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t semaphore) {
  std::lock_guard<std::mutex> lock(semaphore->mutex);
  return static_cast<UBaseType_t>(semaphore->items.size());
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore) { delete semaphore; }

}  // extern "C"
