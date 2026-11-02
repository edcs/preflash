#include <array>
#include <mutex>
#include <utility>

#include "driver/i2c_master.h"
#include "preflash/bus.h"
#include "preflash/fail.h"
#include "preflash/hardware.h"
#include "preflash/trace.h"

struct i2c_master_bus_t {
  int port;
};

struct i2c_master_dev_t {
  int port;
  uint8_t address;
};

namespace preflash {

void I2cBus::add(uint8_t address, std::unique_ptr<I2cDevice> device) {
  std::lock_guard<std::mutex> lock(mutex_);
  devices_[address] = std::move(device);
}

std::unique_ptr<I2cDevice> I2cBus::take(uint8_t address) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = devices_.find(address);
  if (it == devices_.end()) return nullptr;
  std::unique_ptr<I2cDevice> device = std::move(it->second);
  devices_.erase(it);
  return device;
}

I2cDevice* I2cBus::device(uint8_t address) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = devices_.find(address);
  return it == devices_.end() ? nullptr : it->second.get();
}

bool I2cBus::transfer(uint8_t address, std::span<const uint8_t> tx, std::span<uint8_t> rx) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = devices_.find(address);
  const bool injected = it != devices_.end() && hardware::i2c_nacks(port_, address);
  const bool acked = it != devices_.end() && !injected && it->second->write(tx) &&
                     (rx.empty() || it->second->read(rx));
  if (trace::on()) trace::i2c(port_, address, tx, rx, acked, injected);
  return acked;
}

I2cBus& i2c_bus(int port) {
  static std::array<I2cBus, kI2cPorts> buses = {I2cBus(0), I2cBus(1)};
  return buses.at(port);
}

}  // namespace preflash

namespace {

constexpr uint16_t kLargest7BitAddress = 0x7f;
constexpr int kAnyPort = -1;

// Handles hold the bus and the address, not the device, so traffic always crosses the bus.
// ESP-IDF 5.4's transfers report a NACK this way. Only its bus probe says
// ESP_ERR_NOT_FOUND.
esp_err_t transfer(i2c_master_dev_handle_t device, std::span<const uint8_t> tx,
                   std::span<uint8_t> rx) {
  return preflash::i2c_bus(device->port).transfer(device->address, tx, rx) ? ESP_OK
                                                                           : ESP_ERR_INVALID_STATE;
}

}  // namespace

extern "C" {

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t* bus_config,
                             i2c_master_bus_handle_t* ret_bus_handle) {
  if (!bus_config || !ret_bus_handle) return ESP_ERR_INVALID_ARG;
  static std::array<i2c_master_bus_t, preflash::kI2cPorts> buses = {{{0}, {1}}};
  static std::array<bool, preflash::kI2cPorts> in_use{};
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  int port = bus_config->i2c_port;
  if (port == kAnyPort) {
    port = 0;
    while (port < preflash::kI2cPorts && in_use[port]) ++port;
    if (port == preflash::kI2cPorts) return ESP_ERR_NOT_FOUND;
  }
  if (port < 0 || port >= preflash::kI2cPorts) return ESP_ERR_INVALID_ARG;
  in_use[port] = true;
  *ret_bus_handle = &buses[port];
  return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus_handle,
                                    const i2c_device_config_t* dev_config,
                                    i2c_master_dev_handle_t* ret_handle) {
  if (!bus_handle || !dev_config || !ret_handle) return ESP_ERR_INVALID_ARG;
  if (dev_config->dev_addr_length != I2C_ADDR_BIT_LEN_7)
    preflash_unsupported("i2c_master_bus_add_device", "only 7-bit addresses are modelled");
  if (dev_config->flags.disable_ack_check)
    preflash_unsupported("i2c_master_bus_add_device", "every transfer checks its ACK");
  if (dev_config->device_address > kLargest7BitAddress) return ESP_ERR_INVALID_ARG;
  // Lives as long as the firmware: there's no call here to remove a device.
  *ret_handle =
      new i2c_master_dev_t{bus_handle->port, static_cast<uint8_t>(dev_config->device_address)};
  return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t i2c_dev, const uint8_t* write_buffer,
                              size_t write_size, int) {
  if (!i2c_dev || !write_buffer) return ESP_ERR_INVALID_ARG;
  return transfer(i2c_dev, {write_buffer, write_size}, {});
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t i2c_dev, const uint8_t* write_buffer,
                                      size_t write_size, uint8_t* read_buffer, size_t read_size,
                                      int) {
  if (!i2c_dev || !write_buffer || !read_buffer) return ESP_ERR_INVALID_ARG;
  return transfer(i2c_dev, {write_buffer, write_size}, {read_buffer, read_size});
}

}  // extern "C"
