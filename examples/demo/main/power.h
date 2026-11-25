// The AXP2101 power chip: what feeds the board, and a soft switch to turn it off, and its power key.
#pragma once

#include "driver/i2c_master.h"

namespace power {

enum class Charge { none, charging, done, discharging };

struct Reading {
  bool answered = false;
  bool usb = false;
  bool battery = false;
  int percent = 0;
  Charge charge = Charge::none;
};

void start(i2c_master_bus_handle_t bus);
Reading read();
void turn_off();

// The power key is wired to the chip, not to a pin, so the chip says what happened to it:
// nothing, a short press, or a long one.
enum class Key { none, tap, hold };

// Asks the chip to report the key's presses.
void watch_key();
// What the key did since the last call. Reading it clears it.
Key take_key();

}  // namespace power
