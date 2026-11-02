// The QMI8658 motion sensor, used for which way up the board is.
#pragma once

#include "driver/i2c_master.h"

namespace motion {

struct Reading {
  bool answered = false;
  int x_mg = 0;  // gravity in thousandths of a g
  int y_mg = 0;
  int degrees = 0;  // how far the board has turned clockwise: 0, 90, 180 or 270
};

void start(i2c_master_bus_handle_t bus);
Reading read();

}  // namespace motion
