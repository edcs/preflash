#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

struct QueueDefinition;
typedef struct QueueDefinition* QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t ticks_to_wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t ticks_to_wait);

#ifdef __cplusplus
}
#endif
