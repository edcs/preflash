#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tskTaskControlBlock;
typedef struct tskTaskControlBlock* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);

#define tskNO_AFFINITY ((BaseType_t)0x7FFFFFFF)

// Every task is a host thread. The stack size, priority and core are accepted
// and ignored: the host scheduler decides.
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char* name, uint32_t stack_depth,
                                   void* parameter, UBaseType_t priority, TaskHandle_t* created,
                                   BaseType_t core_id);
static inline BaseType_t xTaskCreate(TaskFunction_t task, const char* name, uint32_t stack_depth,
                                     void* parameter, UBaseType_t priority,
                                     TaskHandle_t* created) {
  return xTaskCreatePinnedToCore(task, name, stack_depth, parameter, priority, created,
                                 tskNO_AFFINITY);
}

void vTaskDelay(TickType_t ticks);

// A task can only delete itself, with its own handle or nullptr. As on the device,
// that ends it on the spot and its C++ destructors don't run. Any other handle aborts.
void vTaskDelete(TaskHandle_t task);

TaskHandle_t xTaskGetCurrentTaskHandle(void);
char* pcTaskGetName(TaskHandle_t task);

BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_count_on_exit, TickType_t ticks_to_wait);

#ifdef __cplusplus
}
#endif
