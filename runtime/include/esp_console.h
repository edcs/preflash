#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t max_history_len;
  const char* history_save_path;
  uint32_t task_stack_size;
  uint32_t task_priority;
  BaseType_t task_core_id;
  const char* prompt;
  size_t max_cmdline_length;
} esp_console_repl_config_t;

#define ESP_CONSOLE_REPL_CONFIG_DEFAULT() \
  {                                       \
      .max_history_len = 32,              \
      .history_save_path = NULL,          \
      .task_stack_size = 4096,            \
      .task_priority = 2,                 \
      .task_core_id = tskNO_AFFINITY,     \
      .prompt = NULL,                     \
      .max_cmdline_length = 0,            \
  }

// Empty on a device too; the byte keeps the struct the same size in C and C++.
typedef struct {
  char reserved;
} esp_console_dev_usb_serial_jtag_config_t;

#define ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT() {}

typedef int (*esp_console_cmd_func_t)(int, char**);
typedef int (*esp_console_cmd_func_with_context_t)(void*, int, char**);

typedef struct {
  const char* command;
  const char* help;
  const char* hint;
  esp_console_cmd_func_t func;
  void* argtable;
  esp_console_cmd_func_with_context_t func_w_context;
  void* context;
} esp_console_cmd_t;

typedef struct esp_console_repl_s esp_console_repl_t;
struct esp_console_repl_s {
  esp_err_t (*del)(esp_console_repl_t* repl);
};

esp_err_t esp_console_cmd_register(const esp_console_cmd_t* cmd);

// Only the USB serial/JTAG flavour exists here; the host's stdin and stdout
// stand in for it. The UART and CDC flavours are left out so firmware that
// needs them fails to build.
esp_err_t esp_console_new_repl_usb_serial_jtag(const esp_console_dev_usb_serial_jtag_config_t* dev_config,
                                               const esp_console_repl_config_t* repl_config,
                                               esp_console_repl_t** ret_repl);
esp_err_t esp_console_start_repl(esp_console_repl_t* repl);

#ifdef __cplusplus
}
#endif
