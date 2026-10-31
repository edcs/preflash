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
  // Removes a device and hands it back, or null if there's none, so that a wrapper
  // can be built around it and added in its place.
  std::unique_ptr<I2cDevice> take(uint8_t address);
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
// Whether firmware drives the pin as an output, and whether it holds the pad's level.
bool gpio_is_output(int pin);
bool gpio_is_held(int pin);

}  // namespace preflash
