#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

#include "preflash/audio.h"

// A clock for the codec's pacing that only moves when it is slept on or told to. Each
// sleep jumps it to the wake time asked for and notes that time, so a test checks the
// waits the codec asks for, not how promptly a loaded machine woke from them.
class FakeClock {
 public:
  using time_point = std::chrono::steady_clock::time_point;

  // Far from steady_clock's zero, which the codec may use to mean "never".
  time_point now = time_point(std::chrono::hours(1));
  std::vector<time_point> wakes;

  // Time passing outside the codec, as firmware's own work would take it.
  void advance(std::chrono::steady_clock::duration d) { now += d; }

  // The wakes asked for since `from`, in microseconds after it, from the first'th on.
  std::vector<long long> wakes_after(time_point from, size_t first = 0) const {
    std::vector<long long> out;
    for (size_t i = first; i < wakes.size(); ++i)
      out.push_back(std::chrono::duration_cast<std::chrono::microseconds>(wakes[i] - from).count());
    return out;
  }

  preflash::audio::Clock clock() {
    return {[this] { return now; },
            [this](time_point wake) {
              wakes.push_back(wake);
              if (wake > now) now = wake;
            }};
  }
};
