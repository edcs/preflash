// Runs a fixed list of driver calls against each chip on the board and prints what came back as
// JSON lines, for preflash check. It waits for a key first, so that nothing is lost while the
// host's serial monitor connects, and it runs once, because the chips only start from their
// power-on state once.
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "steps.h"

namespace {

constexpr int kPorts = 2;
constexpr int kTimeoutMs = 200;
constexpr int kHexPerByte = 2;
constexpr size_t kLongestLine = 320;
constexpr uint32_t kUsbBufferBytes = 1024;
static_assert(kUsbBufferBytes > kLongestLine, "the driver drops a line too long for its buffer");
constexpr TickType_t kPromptEvery = pdMS_TO_TICKS(2000);

// Only code built for the chip can claim to have run on one. Under Preflash this file builds for
// the Mac, and the models are answering themselves.
#if defined(__XTENSA__)
constexpr const char* kSource = "board";
constexpr const char* kNote = "esp-idf " IDF_VER;
#else
constexpr const char* kSource = "synthetic";
constexpr const char* kNote = "Run under Preflash, so the chip models answered themselves.";
#endif

// The driver's write waits for the host, where printf drops what the host hasn't read in time.
void say(const char* format, ...) {
  static char line[kLongestLine];
  va_list args;
  va_start(args, format);
  const int length = vsnprintf(line, sizeof line, format, args);
  va_end(args);
  usb_serial_jtag_write_bytes(line, length, portMAX_DELAY);
}

void to_hex(const uint8_t* bytes, size_t length, char* out) {
  for (size_t i = 0; i < length; ++i) snprintf(out + i * kHexPerByte, kHexPerByte + 1, "%02x", bytes[i]);
  out[length * kHexPerByte] = '\0';
}

i2c_master_bus_handle_t open_bus(const Bus& wiring) {
  i2c_master_bus_config_t config = {};
  config.i2c_port = static_cast<i2c_port_t>(wiring.port);
  config.sda_io_num = static_cast<gpio_num_t>(wiring.sda);
  config.scl_io_num = static_cast<gpio_num_t>(wiring.scl);
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.glitch_ignore_cnt = 7;
  config.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t bus = nullptr;
  ESP_ERROR_CHECK(i2c_new_master_bus(&config, &bus));
  return bus;
}

i2c_master_bus_handle_t bus_for(int port) {
  static i2c_master_bus_handle_t open[kPorts] = {};
  for (const Bus& wiring : kBoard.buses) {
    if (wiring.port != port) continue;
    if (!open[port]) open[port] = open_bus(wiring);
    return open[port];
  }
  ESP_ERROR_CHECK(ESP_ERR_NOT_FOUND);
  return nullptr;
}

class Run {
 public:
  void chip(const Chip& chip) {
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = chip.address;
    config.scl_speed_hz = chip.bus_hz;
    i2c_master_dev_handle_t device = nullptr;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_for(chip.bus), &config, &device));
    kept_ = false;
    for (const Step& step : chip.steps) {
      if (step.write_length > 0) transaction(chip, device, step);
      if (step.wait_ms > 0) vTaskDelay(pdMS_TO_TICKS(step.wait_ms));
    }
  }

  int transactions() const { return count_; }

 private:
  void transaction(const Chip& chip, i2c_master_dev_handle_t device, const Step& step) {
    uint8_t write[kLongestWrite + 1];
    size_t write_length = step.write_length;
    std::memcpy(write, step.write.data(), write_length);
    if (step.put_back) {
      if (!kept_) return;  // the read before it failed, so there's nothing to put back
      write[write_length++] = kept_byte_;
    }

    uint8_t read[kLongestRead] = {};
    const esp_err_t err = step.read_length > 0
                              ? i2c_master_transmit_receive(device, write, write_length, read, step.read_length, kTimeoutMs)
                              : i2c_master_transmit(device, write, write_length, kTimeoutMs);
    if (step.keep && err == ESP_OK) {
      kept_ = true;
      kept_byte_ = read[0];
    }

    static char written_hex[(kLongestWrite + 1) * kHexPerByte + 1];
    static char read_hex[kLongestRead * kHexPerByte + 1];
    to_hex(write, write_length, written_hex);
    to_hex(read, err == ESP_OK ? step.read_length : 0, read_hex);
    say("{\"type\":\"tx\",\"seq\":%d,\"chip\":\"%s\",\"bus\":%d,\"addr\":%d,\"write\":\"%s\",\"read_len\":%d,\"read\":\"%s\","
        "\"err\":\"%s\"}\n",
        ++count_, chip.name, chip.bus, chip.address, written_hex, step.read_length, read_hex, esp_err_to_name(err));
  }

  int count_ = 0;
  bool kept_ = false;
  uint8_t kept_byte_ = 0;
};

void wait_for_key() {
  uint8_t key;
  do {
    say("Press Enter to start the capture.\n");
  } while (usb_serial_jtag_read_bytes(&key, 1, kPromptEvery) == 0);
}

}  // namespace

extern "C" void app_main(void) {
  usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  usb.tx_buffer_size = kUsbBufferBytes;
  ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
  wait_for_key();

  say("{\"type\":\"capture\",\"format\":1,\"source\":\"%s\",\"board\":\"%s\",\"note\":\"%s\"}\n", kSource, kBoard.name, kNote);
  Run run;
  for (const Chip& chip : kBoard.chips) run.chip(chip);
  say("{\"type\":\"end\",\"transactions\":%d}\n", run.transactions());
  say("Capture finished. Save the lines above that start with {.\n");
}
