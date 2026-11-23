#pragma once

#include <cmath>
#include <cstdint>

namespace preflash::winlink {

// The share of full brightness the screen shows for a backlight of 0-100%, as 0-255. Square-root
// response: 1% is dim but visible, like the panel in a dark room.
inline uint8_t backlight_level(int percent) {
  return static_cast<uint8_t>(std::lround(255 * std::sqrt(percent / 100.0)));
}

}  // namespace preflash::winlink
