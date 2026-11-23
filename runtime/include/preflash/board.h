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

// A hardware button, which acts on a pin (gpio, expander, power), the AXP2101's PWRON
// (axp2101) or the chip's EN (reset). A power key's hold is the output firmware drives high
// to stay on, a pin or an expander bit.
struct Button {
  std::string name;
  enum class Kind { Gpio, Expander, Axp2101, Reset, Power } kind = Kind::Gpio;
  int pin = -1;  // the GPIO, the expander bit, or the power key's input
  bool active_low = true;
  std::optional<int> hold_pin, hold_expander_bit;
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
  std::vector<Button> buttons;       // in the board file's order, which is the strip's
};

// The board named by PREFLASH_BOARD, loaded on first use.
const Board& board();

// Where a button goes, for the window's strip: "GPIO0, active low", "AXP2101 PWRON", "EN",
// "GPIO16 key, TCA9554 bit 6 hold".
std::string wiring(const Button& button);

// Reads the resolved board JSON. Throws std::runtime_error naming the bad field.
Board parse_board(const std::string& json);

}  // namespace preflash
