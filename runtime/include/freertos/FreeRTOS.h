#pragma once

#include <stddef.h>
#include <stdint.h>

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#ifndef CONFIG_FREERTOS_HZ
#error "CONFIG_FREERTOS_HZ is not defined: the project's sdkconfig must set it."
#endif

#if CONFIG_FREERTOS_HZ > 1000
#error "CONFIG_FREERTOS_HZ above 1000 makes portTICK_PERIOD_MS zero; ESP-IDF allows at most 1000."
#endif

#define configTICK_RATE_HZ CONFIG_FREERTOS_HZ

typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;
typedef uint32_t TickType_t;

#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define portTICK_PERIOD_MS ((TickType_t)(1000 / configTICK_RATE_HZ))
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000))

#define pdFALSE ((BaseType_t)0)
#define pdTRUE ((BaseType_t)1)
#define pdPASS (pdTRUE)
#define pdFAIL (pdFALSE)
