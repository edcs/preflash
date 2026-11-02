// Reads the board's chips on a schedule, and keeps the newest answers for everyone else. An
// esp_timer wakes a task that the task watchdog looks after, so a stuck sensor loop can't go
// unnoticed.
#pragma once

#include "motion.h"
#include "power.h"
#include "rtc.h"

namespace monitor {

struct Snapshot {
  rtc::Status rtc = rtc::Status::no_answer;
  motion::Reading motion;
  power::Reading power;
  int shown_degrees = 0;  // the turn the screen is following
};

// Reads everything once before it returns, so the first frame has real values.
void start();
Snapshot latest();

// Stops feeding the watchdog, which then panics, as a stuck task would make it.
void hang();

}  // namespace monitor
