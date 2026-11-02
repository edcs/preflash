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

// An ESP32-S3 has two I2C controllers, each with its own wires.
constexpr int kI2cPorts = 2;

I2cBus& i2c_bus(int port = 0);

// The pins of the board's I/O expander. The TCA9554 model and firmware's esp_io_expander
// calls are two ways to reach the same pins.
struct ExpanderPins {
  uint32_t outputs;  // pins that are outputs
  uint32_t levels;   // what those drive, kept while a pin is an input
};
ExpanderPins expander_pins();
// Change only the bits in mask.
void expander_set_outputs(uint32_t mask, uint32_t outputs);
void expander_set_levels(uint32_t mask, uint32_t levels);
// As the chip's power-on: every pin an input, every level high.
void expander_power_on();
// What's on the pin. Nothing drives an input here, so the chip's pull-up holds it high.
bool expander_level(int bit);

// Whether firmware has started this SPI bus.
bool spi_bus_started(int host);

// The level on a pin. Nothing drives a pin until firmware or a test does, and such a
// pin reads as its pull resistor holds it, or low if firmware has set none. What a
// test or the outside drives wins over a pull.
void gpio_write(int pin, bool level);
bool gpio_read(int pin);
// Whether firmware drives the pin as an output, and whether it holds the pad's level.
bool gpio_is_output(int pin);
bool gpio_is_held(int pin);
// Counts the times firmware has set the pin's direction, which takes it from any peripheral.
int gpio_direction_epoch(int pin);

}  // namespace preflash
