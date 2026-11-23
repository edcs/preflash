// Firmware for the power key part of tests/integration/buttons.sh, on the 3.49 V2, laid over a
// copy of lvgl_project. The board stays on battery power while SYS_EN, TCA9554 bit 6, is driven
// high: `hold` drives it high, as firmware does once it has booted, and `release` drives it
// low, as firmware does to switch itself off. It draws nothing.
#include <cstdint>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"

namespace {

constexpr const char* kTag = "power";

constexpr uint16_t kExpander = 0x20;
constexpr uint8_t kOutputs = 0x01;
constexpr uint8_t kConfig = 0x03;  // 1: input
constexpr uint8_t kHold = 1 << 6;
constexpr int kTimeoutMs = 50;

i2c_master_dev_handle_t expander() {
  static i2c_master_dev_handle_t handle = [] {
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = kExpander;
    config.scl_speed_hz = 400000;
    i2c_master_dev_handle_t device = nullptr;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &device));
    return device;
  }();
  return handle;
}

uint8_t get(uint8_t reg) {
  uint8_t value = 0;
  ESP_ERROR_CHECK(i2c_master_transmit_receive(expander(), &reg, 1, &value, 1, kTimeoutMs));
  return value;
}

void put(uint8_t reg, uint8_t value) {
  const uint8_t data[] = {reg, value};
  ESP_ERROR_CHECK(i2c_master_transmit(expander(), data, sizeof data, kTimeoutMs));
}

// The level first, so the pin never drives low on its way to being an output.
int hold_command(int, char**) {
  put(kOutputs, get(kOutputs) | kHold);
  put(kConfig, get(kConfig) & ~kHold);
  ESP_LOGI(kTag, "holding");
  return 0;
}

int release_command(int, char**) {
  put(kOutputs, get(kOutputs) & ~kHold);
  ESP_LOGI(kTag, "released");
  return 0;
}

void add_command(const char* name, esp_console_cmd_func_t func) {
  esp_console_cmd_t cmd = {};
  cmd.command = name;
  cmd.func = func;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

}  // namespace

extern "C" void app_main(void) {
  add_command("hold", hold_command);
  add_command("release", release_command);
  esp_console_repl_t* repl = nullptr;
  esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  esp_console_dev_usb_serial_jtag_config_t device = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&device, &config, &repl));
  ESP_ERROR_CHECK(esp_console_start_repl(repl));
  ESP_LOGI(kTag, "up, reset reason %d", static_cast<int>(esp_reset_reason()));
}
