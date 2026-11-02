// The app's idea of the time. It starts from the RTC, corrects itself from SNTP, and counts
// esp_timer's microseconds in between. It can also hold still at a chosen moment, for a
// photograph of the screen or a repeatable comparison.
#pragma once

#include <cstddef>
#include <ctime>

namespace timekeeper {

enum class Source { none, rtc, network, console };

// Picks up the time from the RTC, which must have started.
void start();

bool known();
time_t now();
Source source();

// A correction from outside. The RTC gets it too.
void set(time_t utc, Source from);

// While held, now() stays at utc and anything that moves on the screen stops.
void hold(time_t utc);
void release();
bool held();

// Reads 2026-07-14T09:41:00Z. False if it isn't that.
bool parse(const char* text, time_t* utc);

time_t epoch_from_date(int year, int month, int day, int hour, int minute, int second);

}  // namespace timekeeper
