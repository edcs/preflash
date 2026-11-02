// What the app needs from a board. board_4b.cpp reaches its screen through Waveshare's board
// package, and board_349.cpp through esp_lcd and plain drivers. Nothing else knows which.
#pragma once

#include "driver/i2c_master.h"
#include "esp_codec_dev.h"

namespace board {

// Display and touch running, backlight off. LVGL is ready for use under a Lock.
void start();

const char* name();

// The bus the RTC, motion sensor and power chip share.
i2c_master_bus_handle_t sensor_bus();

void set_brightness(int percent);

// The turns the board's layout survives. A square panel takes all four. A long thin one only
// makes sense upright or upside down.
bool can_turn_to(int degrees);
void turn_to(int degrees);

bool has_power_chip();

// The speaker's codec, or null on a board the demo plays no sound on.
esp_codec_dev_handle_t speaker();

// Holds LVGL's lock for one scope. Every LVGL call outside its own task needs one.
class Lock {
 public:
  Lock();
  ~Lock();
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

}  // namespace board
