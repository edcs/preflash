// The PCF85063 real-time clock, which keeps the time on its own cell while the board is off.
#pragma once

#include <ctime>

#include "driver/i2c_master.h"

namespace rtc {

enum class Status {
  ok,
  no_answer,  // nothing on the bus at its address
  time_lost,  // the chip answers, but its cell went flat and the time means nothing
};

void start(i2c_master_bus_handle_t bus);

// Fills in utc when the answer is ok.
Status read(time_t* utc);

// Setting the time clears a lost-time mark.
Status write(time_t utc);

}  // namespace rtc
