// The AXP2101 power chip: what feeds the board, and a soft switch to turn it off.
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

}  // namespace power
