#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "preflash/chips.h"

namespace preflash {
namespace {

// Section 8.1, table 5.
constexpr uint8_t kControl1 = 0x00;
constexpr uint8_t kControl2 = 0x01;
constexpr uint8_t kOffset = 0x02;
constexpr uint8_t kRamByte = 0x03;
constexpr uint8_t kSeconds = 0x04;
constexpr uint8_t kMinutes = 0x05;
constexpr uint8_t kHours = 0x06;
constexpr uint8_t kDays = 0x07;
constexpr uint8_t kWeekdays = 0x08;
constexpr uint8_t kMonths = 0x09;
constexpr uint8_t kYears = 0x0a;
constexpr uint8_t kSecondAlarm = 0x0b;
constexpr uint8_t kWeekdayAlarm = 0x0f;
constexpr uint8_t kTimerMode = 0x11;
constexpr uint8_t kLastRegister = kTimerMode;

constexpr uint8_t kCapSelect = 0x01;          // section 8.2.1, table 6
constexpr uint8_t kOscillatorStopped = 0x80;  // section 8.3.1, table 19
constexpr int kDaysInWeek = 7;
constexpr int kSecondsInMinute = 60;
constexpr int kSecondsInHour = 3600;
constexpr int kSecondsInDay = 86400;
constexpr int kTmYearOf2000 = 100;

// What a restart carries over, in the environment: the moment the registers show,
// whether the oscillator was stopped, and registers 00h to 11h in hex.
constexpr const char* kCarryEnv = "PREFLASH_RTC_CARRY";
constexpr size_t kCarriedRegisters = kLastRegister + 1;

// The chips alive now, newest last. A board has one RTC; if a test makes more, the
// newest is the one that carries on.
std::mutex g_live_mutex;
std::vector<Pcf85063*> g_live;

void track(Pcf85063* chip) {
  std::lock_guard<std::mutex> lock(g_live_mutex);
  g_live.push_back(chip);
}

constexpr uint8_t to_bcd(int value) { return static_cast<uint8_t>((value / 10) << 4 | value % 10); }
constexpr int from_bcd(uint8_t value) { return (value >> 4) * 10 + (value & 0x0f); }

}  // namespace

Pcf85063::Pcf85063() : Pcf85063([] { return std::time(nullptr); }) {}

Pcf85063::Pcf85063(std::function<std::time_t()> clock)
    : RegisterChip("pcf85063"), clock_(std::move(clock)) {
  // Section 8.2.1.3, table 8, except that the time registers start from the
  // Mac's clock, as a chip does that's been running on its cell.
  std::fill(regs_.begin() + kSecondAlarm, regs_.begin() + kWeekdayAlarm + 1, 0x80);
  regs_[kTimerMode] = 0x18;
  counted_to_ = clock_();
  if (const char* carried = std::getenv(kCarryEnv)) {
    // A restart's own variable: it wins over PREFLASH_RTC_START, which only sets
    // where a fresh process begins. Taken once, so a later chip starts fresh.
    const std::string value = carried;
    unsetenv(kCarryEnv);
    restore(value);
    track(this);
    return;
  }
  std::tm t = {};
  gmtime_r(&counted_to_, &t);
  // A chosen start lets a test boot firmware at any time of day. The clock runs on from it.
  if (const char* start = std::getenv("PREFLASH_RTC_START")) {
    std::tm chosen = {};
    const char* end = strptime(start, "%Y-%m-%dT%H:%M:%SZ", &chosen);
    if (!end || *end != '\0')
      throw std::runtime_error(std::string("PREFLASH_RTC_START isn't a time like "
                                           "2026-11-01T12:00:00Z: ") + start);
    const std::time_t chosen_time = timegm(&chosen);
    gmtime_r(&chosen_time, &t);
    // strptime accepts dates like February 31st, which timegm rolls into March, and
    // the chip's year register only holds 2000 to 2099.
    char again[32];
    std::strftime(again, sizeof(again), "%Y-%m-%dT%H:%M:%SZ", &t);
    const int year = t.tm_year + 1900;
    if (std::string(again) != start || year < 2000 || year > 2099)
      throw std::runtime_error(std::string("PREFLASH_RTC_START isn't a real time from 2000 to "
                                           "2099 like 2026-11-01T12:00:00Z: ") + start);
  }
  set_fields(t);
  regs_[kWeekdays] = static_cast<uint8_t>(t.tm_wday);
  track(this);  // last, because a constructor that throws never runs the destructor
}

Pcf85063::~Pcf85063() {
  std::lock_guard<std::mutex> lock(g_live_mutex);
  g_live.erase(std::remove(g_live.begin(), g_live.end(), this), g_live.end());
}

void Pcf85063::save_for_restart() {
  std::lock_guard<std::mutex> lock(mutex_);
  count_time();
  std::string value = std::to_string(static_cast<long long>(counted_to_)) +
                      (oscillator_stopped_ ? " 1 " : " 0 ");
  char byte[3];
  for (size_t i = 0; i < kCarriedRegisters; ++i) {
    std::snprintf(byte, sizeof byte, "%02x", regs_[i]);
    value += byte;
  }
  setenv(kCarryEnv, value.c_str(), 1);
}

void Pcf85063::restore(const std::string& value) {
  long long counted = 0;
  int stopped = 0;
  char hex[2 * kCarriedRegisters + 1] = {};
  int end = 0;
  const bool parsed = std::sscanf(value.c_str(), "%lld %d %36s%n", &counted, &stopped, hex, &end) == 3 &&
                      std::strlen(hex) == 2 * kCarriedRegisters && value[end] == '\0';
  if (!parsed)
    throw std::runtime_error(std::string(kCarryEnv) + " isn't what a restart leaves: " + value);
  if (std::strspn(hex, "0123456789abcdefABCDEF") != 2 * kCarriedRegisters)
    throw std::runtime_error(std::string(kCarryEnv) + " isn't what a restart leaves: " + value);
  for (size_t i = 0; i < kCarriedRegisters; ++i) {
    unsigned byte = 0;
    std::sscanf(hex + 2 * i, "%2x", &byte);
    regs_[i] = static_cast<uint8_t>(byte);
  }
  counted_to_ = static_cast<std::time_t>(counted);
  oscillator_stopped_ = stopped != 0;
}

void carry_chips_across_restart() {
  // Held throughout, so the chip can't be destroyed while it is being saved.
  std::lock_guard<std::mutex> lock(g_live_mutex);
  if (!g_live.empty()) g_live.back()->save_for_restart();
}

// Section 8.4: an access blocks the counters, so all of one transfer is one moment.
bool Pcf85063::write(std::span<const uint8_t> data) {
  std::lock_guard<std::mutex> lock(mutex_);
  count_time();
  return RegisterChip::write(data);
}

bool Pcf85063::read(std::span<uint8_t> out) {
  std::lock_guard<std::mutex> lock(mutex_);
  count_time();
  return RegisterChip::read(out);
}

void Pcf85063::set_fields(const std::tm& t) {
  regs_[kSeconds] = to_bcd(t.tm_sec);
  regs_[kMinutes] = to_bcd(t.tm_min);
  regs_[kHours] = to_bcd(t.tm_hour);
  regs_[kDays] = to_bcd(t.tm_mday);
  regs_[kMonths] = to_bcd(t.tm_mon + 1);
  regs_[kYears] = to_bcd((t.tm_year - kTmYearOf2000) % 100);  // section 8.3.7: two digits
}

// The registers are the clock, as on the chip, and this counts them on by the
// seconds gone since. The date only moves at midnight, so firmware can set its
// fields in separate writes and pass through a day the month doesn't have. The
// datasheet doesn't say what an impossible date does at midnight: here it
// normalises, as timegm does.
void Pcf85063::count_time() {
  const std::time_t now = clock_();
  // A stopped oscillator counts nothing, so the time is held where it was set
  // and the stopped interval is not added when the flag is cleared.
  if (oscillator_stopped_) {
    counted_to_ = now;
    return;
  }
  const std::time_t elapsed = now - counted_to_;
  if (elapsed == 0) return;
  counted_to_ = now;

  std::tm t = {};
  t.tm_sec = from_bcd(regs_[kSeconds]);
  t.tm_min = from_bcd(regs_[kMinutes]);
  t.tm_hour = from_bcd(regs_[kHours]);
  const std::time_t second_of_day =
      t.tm_hour * kSecondsInHour + t.tm_min * kSecondsInMinute + t.tm_sec + elapsed;
  if (second_of_day >= 0 && second_of_day < kSecondsInDay) {
    regs_[kSeconds] = to_bcd(static_cast<int>(second_of_day % kSecondsInMinute));
    regs_[kMinutes] = to_bcd(static_cast<int>(second_of_day / kSecondsInMinute % 60));
    regs_[kHours] = to_bcd(static_cast<int>(second_of_day / kSecondsInHour));
    return;
  }

  t.tm_mday = from_bcd(regs_[kDays]);
  t.tm_mon = from_bcd(regs_[kMonths]) - 1;
  t.tm_year = from_bcd(regs_[kYears]) + kTmYearOf2000;
  const std::time_t before = timegm(&t);
  const std::time_t after = before + elapsed;
  gmtime_r(&after, &t);
  set_fields(t);
  // Table 25: the weekday is the firmware's to define, and only counts on from
  // what it was given, so it's never worked out from the date.
  const int days = static_cast<int>(after / kSecondsInDay - before / kSecondsInDay);
  regs_[kWeekdays] =
      static_cast<uint8_t>(((regs_[kWeekdays] + days) % kDaysInWeek + kDaysInWeek) % kDaysInWeek);
}

uint8_t Pcf85063::load(uint8_t reg) {
  // Control_2, alarms and timer have no model, as in store. A burst wraps to 00h, so
  // only setting the address can reach past the end.
  if (reg == kControl2 || reg >= kSecondAlarm) refuse(reg, "reading it has no model");
  if (reg == kSeconds) return regs_[reg] | (oscillator_stopped_ ? kOscillatorStopped : 0);
  return regs_[reg];
}

void Pcf85063::store(uint8_t reg, uint8_t value) {
  switch (reg) {
    case kControl1:  // section 8.2.1, table 6
      if (value & ~kCapSelect) refuse(reg, "only 24-hour running is modelled");
      regs_[reg] = value;
      break;
    case kOffset:   // section 8.2.3, table 12: fine tuning, which a host clock doesn't need
    case kRamByte:  // section 8.2.4, table 18
      regs_[reg] = value;
      break;
    case kSeconds:  // section 8.3.1, table 19: bit 7 is OS, section 8.3.1.1
      regs_[reg] = value & ~kOscillatorStopped;
      oscillator_stopped_ = (value & kOscillatorStopped) != 0;
      break;
    case kMinutes:  // section 8.3.2, table 21
      regs_[reg] = value & 0x7f;
      break;
    case kHours:  // section 8.3.3, table 22, 24-hour form
    case kDays:   // section 8.3.4, table 23
      regs_[reg] = value & 0x3f;
      break;
    case kWeekdays:  // section 8.3.5, table 24
      regs_[reg] = value & 0x07;
      break;
    case kMonths:  // section 8.3.6, table 26
      regs_[reg] = value & 0x1f;
      break;
    case kYears:  // section 8.3.7, table 28
      regs_[reg] = value;
      break;
    default:
      // Control_2, alarms and timer: firmware would wait for an interrupt that
      // never comes.
      refuse(reg, "writing it has no model");
  }
}

// Section 8: after the last register, the address wraps to 00h.
uint8_t Pcf85063::next(uint8_t reg) {
  return reg >= kLastRegister ? 0 : static_cast<uint8_t>(reg + 1);
}

}  // namespace preflash
