#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>

namespace preflash {

class I2cDevice {
 public:
  virtual ~I2cDevice() = default;
  virtual bool write(std::span<const uint8_t> data) = 0;  // false is a NACK
  virtual bool read(std::span<uint8_t> out) = 0;
};

class I2cBus {
 public:
  // Replaces and destroys any device already at the address.
  void add(uint8_t address, std::unique_ptr<I2cDevice> device);
  I2cDevice* device(uint8_t address);

  // One write, then one read if rx isn't empty, with no other task's traffic
  // between them. False if nothing answers or the device refuses.
  bool transfer(uint8_t address, std::span<const uint8_t> tx, std::span<uint8_t> rx);

 private:
  std::mutex mutex_;
  std::map<uint8_t, std::unique_ptr<I2cDevice>> devices_;
};

I2cBus& i2c_bus();

// The level on a pin. Nothing drives a pin until firmware or a test does, and
// such a pin reads low, because the model has no pull resistors.
void gpio_write(int pin, bool level);
bool gpio_read(int pin);

}  // namespace preflash
