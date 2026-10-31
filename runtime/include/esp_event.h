#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_event_base.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_EVENT_ANY_BASE NULL
#define ESP_EVENT_ANY_ID -1

typedef void (*esp_event_handler_t)(void* event_handler_arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data);
typedef struct esp_event_handler_instance* esp_event_handler_instance_t;
typedef struct esp_event_loop* esp_event_loop_handle_t;

typedef struct {
  int32_t queue_size;
  const char* task_name;  // a loop without a task has to be run by hand, which isn't supported
  UBaseType_t task_priority;
  uint32_t task_stack_size;
  BaseType_t task_core_id;
} esp_event_loop_args_t;

esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_loop_delete_default(void);
esp_err_t esp_event_loop_create(const esp_event_loop_args_t* args, esp_event_loop_handle_t* loop);
esp_err_t esp_event_loop_delete(esp_event_loop_handle_t loop);

esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id, esp_event_handler_t handler,
                                     void* arg);
esp_err_t esp_event_handler_register_with(esp_event_loop_handle_t loop, esp_event_base_t base,
                                          int32_t id, esp_event_handler_t handler, void* arg);
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t handler, void* arg,
                                              esp_event_handler_instance_t* instance);
esp_err_t esp_event_handler_instance_register_with(esp_event_loop_handle_t loop,
                                                   esp_event_base_t base, int32_t id,
                                                   esp_event_handler_t handler, void* arg,
                                                   esp_event_handler_instance_t* instance);
esp_err_t esp_event_handler_unregister(esp_event_base_t base, int32_t id,
                                       esp_event_handler_t handler);
esp_err_t esp_event_handler_unregister_with(esp_event_loop_handle_t loop, esp_event_base_t base,
                                            int32_t id, esp_event_handler_t handler);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
                                                esp_event_handler_instance_t instance);
esp_err_t esp_event_handler_instance_unregister_with(esp_event_loop_handle_t loop,
                                                     esp_event_base_t base, int32_t id,
                                                     esp_event_handler_instance_t instance);

// The data is copied, so the caller's buffer can go straight away.
esp_err_t esp_event_post(esp_event_base_t base, int32_t id, const void* data, size_t data_size,
                         TickType_t ticks_to_wait);
esp_err_t esp_event_post_to(esp_event_loop_handle_t loop, esp_event_base_t base, int32_t id,
                            const void* data, size_t data_size, TickType_t ticks_to_wait);

esp_err_t esp_event_loop_run(esp_event_loop_handle_t loop, TickType_t ticks_to_run);

#ifdef __cplusplus
}
#endif
