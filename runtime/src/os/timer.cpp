#include <chrono>

#include "esp_timer.h"

namespace {
// A function-local static, so a global constructor elsewhere can call
// esp_timer_get_time() before the start time exists.
std::chrono::steady_clock::time_point start_time() {
  static const auto start = std::chrono::steady_clock::now();
  return start;
}
}  // namespace

extern "C" int64_t esp_timer_get_time(void) {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                               start_time())
      .count();
}
