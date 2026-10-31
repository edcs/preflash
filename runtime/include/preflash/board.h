#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace preflash {

struct ChipEntry {
  std::string chip;
  uint8_t address;
};

struct Backlight {
  int pin;
  bool active_low;
  std::optional<int> enable_pin;
};

struct Board {
  std::string name;
  int width, height;
  std::vector<ChipEntry> i2c;
  Backlight backlight;
  std::string audio_codec;
};

// The board named by PREFLASH_BOARD, loaded on first use.
const Board& board();

// Reads the resolved board JSON. Throws std::runtime_error naming the bad field.
Board parse_board(const std::string& json);

}  // namespace preflash
