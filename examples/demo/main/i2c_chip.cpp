#include "i2c_chip.h"

#include <cstring>

namespace {

constexpr uint32_t kBusHz = 100'000;
constexpr int kTimeoutMs = 100;
constexpr size_t kLongestWrite = 16;

}  // namespace

void I2cChip::attach(i2c_master_bus_handle_t bus, uint8_t address) {
  i2c_device_config_t config = {};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = address;
  config.scl_speed_hz = kBusHz;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &config, &device_));
}

esp_err_t I2cChip::read(uint8_t reg, void* out, size_t length) const {
  return i2c_master_transmit_receive(device_, &reg, 1, static_cast<uint8_t*>(out), length, kTimeoutMs);
}

esp_err_t I2cChip::write(uint8_t reg, const void* data, size_t length) const {
  if (length > kLongestWrite) return ESP_ERR_INVALID_SIZE;
  uint8_t frame[kLongestWrite + 1] = {reg};
  std::memcpy(frame + 1, data, length);
  return i2c_master_transmit(device_, frame, length + 1, kTimeoutMs);
}
