// The board's keys, read two ways. BOOT is a plain GPIO, so a pin interrupt counts its presses.
// PWR is wired to the AXP2101, so the chip's interrupt status says whether it was tapped or
// held, and the firmware decides what a hold does: here, switch the board off.
#pragma once

#include <cstdint>

#include "driver/i2c_master.h"

namespace buttons {

struct State {
  bool boot_down = false;
  uint32_t boot_presses = 0;  // counted by the pin interrupt
  bool has_power_key = false;
  uint32_t power_taps = 0;
  uint32_t power_holds = 0;
  bool hold_turns_off = true;
};

// With a power chip, its key is watched too. Without one, only BOOT is.
void start(bool has_power_chip);

State latest();

// Whether a long press of PWR switches the board off. On by default.
void set_hold_turns_off(bool on);

}  // namespace buttons
