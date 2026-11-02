// What the app keeps in NVS, so that it survives a restart.
#pragma once

#include <cstdint>

namespace settings {

struct Values {
  uint8_t brightness = 80;
  uint8_t volume = 60;
  uint32_t boots = 0;
  char ssid[33] = "";
  char password[65] = "";
};

// Reads what's saved and counts this boot.
void start();

Values get();
void set_brightness(int percent);
void set_volume(int percent);
void set_wifi(const char* ssid, const char* password);

// Back to the defaults, keeping the boot count.
void reset();

}  // namespace settings
