#pragma once

#include <chrono>

#include "freertos/FreeRTOS.h"

namespace preflash {

inline std::chrono::microseconds ticks_to_duration(TickType_t ticks) {
  return std::chrono::microseconds(static_cast<uint64_t>(ticks) * 1000000ULL / configTICK_RATE_HZ);
}

}  // namespace preflash
