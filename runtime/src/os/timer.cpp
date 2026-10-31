#include <chrono>

#include "esp_timer.h"

namespace {
// Fixed at load, so the first caller doesn't start the clock at zero.
const auto kStart = std::chrono::steady_clock::now();
}  // namespace

extern "C" int64_t esp_timer_get_time(void) {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                               kStart)
      .count();
}
