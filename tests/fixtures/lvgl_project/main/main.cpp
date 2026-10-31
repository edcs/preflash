#include <cstdlib>

#include "bsp/esp-bsp.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char* kTag = "fixture";

lv_display_t* g_display = nullptr;

void on_clicked(lv_event_t*) { ESP_LOGI(kTag, "clicked"); }

void hold_lock(void*) {
  bsp_display_lock(0);
  ESP_LOGI(kTag, "holding the lock");
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

int rotate_command(int argc, char** argv) {
  if (argc != 2) return 1;
  const int degrees = std::atoi(argv[1]);
  bsp_display_lock(0);
  bsp_display_rotate(g_display, static_cast<lv_display_rotation_t>(degrees / 90));
  lv_refr_now(g_display);
  bsp_display_unlock();
  ESP_LOGI(kTag, "rotated %d", degrees);
  return 0;
}

int brightness_command(int argc, char** argv) {
  if (argc != 2) return 1;
  return bsp_display_brightness_set(std::atoi(argv[1]));
}

int hold_command(int, char**) {
  xTaskCreate(hold_lock, "hold", 4096, nullptr, 2, nullptr);
  return 0;
}

int ping_command(int, char**) {
  ESP_LOGI(kTag, "pong");
  return 0;
}

int restart_command(int, char**) {
  esp_restart();
  return 0;
}

void add_command(const char* name, esp_console_cmd_func_t func) {
  esp_console_cmd_t cmd = {};
  cmd.command = name;
  cmd.func = func;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void start_console() {
  add_command("rotate", rotate_command);
  add_command("brightness", brightness_command);
  add_command("hold", hold_command);
  add_command("ping", ping_command);
  add_command("restart", restart_command);
  esp_console_repl_t* repl = nullptr;
  esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  esp_console_dev_usb_serial_jtag_config_t device = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&device, &config, &repl));
  ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

}  // namespace

extern "C" void app_main(void) {
  bsp_display_cfg_t cfg = {.lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG()};
  cfg.lvgl_port_cfg.task_stack = 16384;
  g_display = bsp_display_start_with_config(&cfg);
  bsp_display_brightness_set(100);

  bsp_display_lock(0);
  lv_obj_t* title = lv_label_create(lv_screen_active());
  lv_label_set_text(title, "Preflash");
  lv_obj_set_pos(title, 40, 20);
  lv_obj_t* button = lv_button_create(lv_screen_active());
  lv_obj_set_pos(button, 40, 60);
  lv_obj_set_size(button, 120, 50);
  lv_obj_add_event_cb(button, on_clicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* label = lv_label_create(button);
  lv_label_set_text(label, "Press");
  lv_obj_center(label);
  lv_refr_now(g_display);
  bsp_display_unlock();

  start_console();
  ESP_LOGI(kTag, "ready, reset reason %d", static_cast<int>(esp_reset_reason()));
}
