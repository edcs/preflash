#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace preflash {

struct ChipEntry {
  std::string chip;
  uint8_t address;
  int bus = 0;
};

struct Backlight {
  int pin;
  bool active_low;
  std::optional<int> enable_pin;
  std::optional<int> enable_expander_bit;  // the same thing, on the TCA9554
};

struct Board {
  std::string name;
  std::string controller;
  int width, height;
  bool big_endian;  // the panel takes RGB565 high byte first from esp_lcd
  std::vector<ChipEntry> i2c;
  Backlight backlight;
  std::string audio_codec;
  std::optional<int> power_amp_bit;  // the expander bit that switches the speaker amp
};

// The board named by PREFLASH_BOARD, loaded on first use.
const Board& board();

// Reads the resolved board JSON. Throws std::runtime_error naming the bad field.
Board parse_board(const std::string& json);

}  // namespace preflash
