// One chip on an I2C bus, read and written by register. Every call reports failure instead of
// stopping, because a chip that stops answering is something the app shows, not a crash.
#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

class I2cChip {
 public:
  void attach(i2c_master_bus_handle_t bus, uint8_t address);
  esp_err_t read(uint8_t reg, void* out, size_t length) const;
  esp_err_t write(uint8_t reg, const void* data, size_t length) const;
  esp_err_t write(uint8_t reg, uint8_t value) const { return write(reg, &value, 1); }

 private:
  i2c_master_dev_handle_t device_ = nullptr;
};
