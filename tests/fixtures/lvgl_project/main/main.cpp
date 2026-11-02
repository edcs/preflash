#include <cstdio>
#include <cstdlib>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_console.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

constexpr const char* kTag = "fixture";

lv_display_t* g_display = nullptr;
esp_console_repl_t* g_repl = nullptr;

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

// A quarter second of a quiet tone at 16 kHz, written the way a chime would be.
int play_command(int, char**) {
  esp_codec_dev_handle_t speaker = bsp_audio_codec_speaker_init();
  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate = 16000;
  fs.channel = 1;
  fs.bits_per_sample = 16;
  if (esp_codec_dev_open(speaker, &fs) != ESP_CODEC_DEV_OK) return 1;
  esp_codec_dev_set_out_vol(speaker, 20);
  int16_t pcm[320];
  for (int i = 0; i < 320; ++i) pcm[i] = (i / 20) % 2 ? 4000 : -4000;
  for (int i = 0; i < 12; ++i) esp_codec_dev_write(speaker, pcm, sizeof(pcm));
  ESP_LOGI(kTag, "played");
  return 0;
}

void restart_now(void*) { esp_restart(); }

// Off the console's thread, as a button or a timer would, while stdin sits idle.
int restart_command(int, char**) {
  xTaskCreate(restart_now, "restart", 4096, nullptr, 2, nullptr);
  return 0;
}

// Far more than a client's queue holds, as fast as the host can print it.
int flood_command(int, char**) {
  for (int i = 0; i < 80000; ++i) std::printf("%099d\n", i);
  ESP_LOGI(kTag, "flood done");
  return 0;
}

// Never comes back, as a firmware command with a bug might not.
int hang_command(int, char**) {
  ESP_LOGI(kTag, "hanging");
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
  return 0;
}

// A call the simulator refuses, which stops the process.
int unsupported_command(int, char**) { return g_repl->del(g_repl); }

// The chips, as a driver reaches them. Each says what came back, so a test can see a forced
// failure arrive through the calls a real one would.
i2c_master_dev_handle_t chip(uint16_t address) {
  i2c_device_config_t config = {};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = address;
  config.scl_speed_hz = 400000;
  i2c_master_dev_handle_t handle = nullptr;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &handle));
  return handle;
}

int rtc_command(int, char**) {
  static i2c_master_dev_handle_t rtc = chip(0x51);
  const uint8_t seconds = 0x04;
  uint8_t value = 0;
  const esp_err_t err = i2c_master_transmit_receive(rtc, &seconds, 1, &value, 1, 100);
  ESP_LOGI(kTag, "rtc err %d stopped %d", err, value >> 7);
  return 0;
}

int accel_command(int, char**) {
  static i2c_master_dev_handle_t imu = chip(0x6b);
  const uint8_t setup[][2] = {{0x02, 0x40}, {0x08, 0x01}};
  esp_err_t err = ESP_OK;
  for (const auto& write : setup) {
    if (err == ESP_OK) err = i2c_master_transmit(imu, write, sizeof write, 100);
  }
  const uint8_t first = 0x35;
  uint8_t data[6] = {};
  if (err == ESP_OK) err = i2c_master_transmit_receive(imu, &first, 1, data, sizeof data, 100);
  const int x = static_cast<int16_t>(data[0] | data[1] << 8);
  const int y = static_cast<int16_t>(data[2] | data[3] << 8);
  ESP_LOGI(kTag, "accel err %d x %d y %d", err, x, y);
  return 0;
}

int power_command(int, char**) {
  static i2c_master_dev_handle_t pmu = chip(0x34);
  const uint8_t status = 0x00;
  const uint8_t gauge = 0xa4;
  uint8_t values[2] = {};
  esp_err_t err = i2c_master_transmit_receive(pmu, &status, 1, &values[0], 1, 100);
  if (err == ESP_OK) err = i2c_master_transmit_receive(pmu, &gauge, 1, &values[1], 1, 100);
  ESP_LOGI(kTag, "power err %d status %02x gauge %d", err, values[0], values[1]);
  return 0;
}

int save_command(int, char**) {
  ESP_ERROR_CHECK(nvs_flash_init());
  nvs_handle_t handle;
  ESP_ERROR_CHECK(nvs_open("fixture", NVS_READWRITE, &handle));
  ESP_ERROR_CHECK(nvs_set_blob(handle, "greeting", "hello", 5));
  ESP_ERROR_CHECK(nvs_commit(handle));
  nvs_close(handle);
  ESP_LOGI(kTag, "saved");
  return 0;
}

int pin_command(int, char**) {
  ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_7, GPIO_MODE_OUTPUT));
  ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_7, 1));
  ESP_LOGI(kTag, "pin set");
  return 0;
}

void wait_forever(void*) {
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

int spawn_command(int, char**) {
  xTaskCreate(wait_forever, "spawned", 2560, nullptr, 2, nullptr);
  ESP_LOGI(kTag, "spawned");
  return 0;
}

int fetch_command(int, char**) {
  esp_http_client_config_t config = {};
  config.url = "http://127.0.0.1:9/nothing";
  esp_http_client_handle_t client = esp_http_client_init(&config);
  const esp_err_t err = esp_http_client_perform(client);
  esp_http_client_cleanup(client);
  ESP_LOGI(kTag, "fetched, err %d", err);
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
  add_command("play", play_command);
  add_command("ping", ping_command);
  add_command("restart", restart_command);
  add_command("flood", flood_command);
  add_command("hang", hang_command);
  add_command("unsupported", unsupported_command);
  add_command("rtc", rtc_command);
  add_command("accel", accel_command);
  add_command("power", power_command);
  add_command("save", save_command);
  add_command("pin", pin_command);
  add_command("spawn", spawn_command);
  add_command("fetch", fetch_command);
  esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  esp_console_dev_usb_serial_jtag_config_t device = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&device, &config, &g_repl));
  ESP_ERROR_CHECK(esp_console_start_repl(g_repl));
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
