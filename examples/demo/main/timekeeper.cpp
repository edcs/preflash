#include "timekeeper.h"

#include <cstdint>
#include <cstdio>
#include <mutex>

#include "esp_log.h"
#include "esp_timer.h"
#include "rtc.h"

namespace timekeeper {
namespace {

constexpr const char* kTag = "time";
constexpr int64_t kMicrosPerSecond = 1'000'000;

// The console's task and the network's both set the time, and every task reads it.
std::mutex g_mutex;
time_t g_base = 0;
int64_t g_base_taken_us = 0;
Source g_source = Source::none;
time_t g_held = 0;

void adopt(time_t utc, Source from) {
  std::lock_guard lock(g_mutex);
  g_base_taken_us = esp_timer_get_time();
  g_base = utc;
  g_source = from;
}

}  // namespace

void start() {
  time_t utc;
  switch (rtc::read(&utc)) {
    case rtc::Status::ok:
      adopt(utc, Source::rtc);
      break;
    case rtc::Status::time_lost:
      ESP_LOGW(kTag, "the RTC lost the time");
      break;
    case rtc::Status::no_answer:
      ESP_LOGW(kTag, "no answer from the RTC");
      break;
  }
}

bool known() {
  std::lock_guard lock(g_mutex);
  return g_source != Source::none || g_held != 0;
}

time_t now() {
  std::lock_guard lock(g_mutex);
  if (g_held != 0) return g_held;
  return g_base + (esp_timer_get_time() - g_base_taken_us) / kMicrosPerSecond;
}

Source source() {
  std::lock_guard lock(g_mutex);
  return g_source;
}

void set(time_t utc, Source from) {
  adopt(utc, from);
  if (rtc::write(utc) != rtc::Status::ok) ESP_LOGW(kTag, "the RTC didn't take the time");
}

void hold(time_t utc) {
  std::lock_guard lock(g_mutex);
  g_held = utc;
}

void release() { hold(0); }

bool held() {
  std::lock_guard lock(g_mutex);
  return g_held != 0;
}

bool parse(const char* text, time_t* utc) {
  int year, month, day, hour, minute, second;
  char end = 0;
  const int found = std::sscanf(text, "%d-%d-%dT%d:%d:%d%c", &year, &month, &day, &hour, &minute, &second, &end);
  if (found != 7 || end != 'Z') return false;
  if (year < 2000) return false;
  // A date that doesn't exist, such as 31 February, comes back as a different one.
  const time_t candidate = epoch_from_date(year, month, day, hour, minute, second);
  std::tm date;
  gmtime_r(&candidate, &date);
  if (date.tm_year + 1900 != year || date.tm_mon + 1 != month || date.tm_mday != day || date.tm_hour != hour ||
      date.tm_min != minute || date.tm_sec != second) {
    return false;
  }
  *utc = candidate;
  return true;
}

// Days from the civil calendar, after Howard Hinnant's days_from_civil.
time_t epoch_from_date(int year, int month, int day, int hour, int minute, int second) {
  year -= month <= 2;
  const int era = year / 400;
  const int year_of_era = year - era * 400;
  const int day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  const int64_t days = era * int64_t{146097} + day_of_era - 719468;
  return days * 86400 + hour * 3600 + minute * 60 + second;
}

}  // namespace timekeeper
