#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

struct QueueDefinition;
typedef struct QueueDefinition* QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t ticks_to_wait);
// From an ISR stand-in, which is a host thread: it never waits.
BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void* item, BaseType_t* task_woken);
BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t ticks_to_wait);

#ifdef __cplusplus
}
#endif
