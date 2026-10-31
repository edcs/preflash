#include <algorithm>

#include "preflash/chips.h"

namespace preflash {
namespace {

// Section 8.1, table 5.
constexpr uint8_t kControl1 = 0x00;
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
constexpr int kTmYearOf2000 = 100;

constexpr uint8_t to_bcd(int value) { return static_cast<uint8_t>((value / 10) << 4 | value % 10); }
constexpr int from_bcd(uint8_t value) { return (value >> 4) * 10 + (value & 0x0f); }

}  // namespace

Pcf85063::Pcf85063() : RegisterChip("pcf85063") {
  // Section 8.2.1.3, table 8. The time registers aren't here because they're
  // filled in from the clock.
  std::fill(regs_.begin() + kSecondAlarm, regs_.begin() + kWeekdayAlarm + 1, 0x80);
  regs_[kTimerMode] = 0x18;
}

bool Pcf85063::write(std::span<const uint8_t> data) {
  freeze_time();
  time_written_ = false;
  RegisterChip::write(data);
  if (time_written_) set_time();
  return true;
}

// Section 8.4: an access blocks the counters, so all of one transfer is one moment.
bool Pcf85063::read(std::span<uint8_t> out) {
  freeze_time();
  return RegisterChip::read(out);
}

void Pcf85063::freeze_time() {
  const std::time_t now = std::time(nullptr) + ahead_of_mac_;
  std::tm t = {};
  gmtime_r(&now, &t);
  regs_[kSeconds] = to_bcd(t.tm_sec) | (oscillator_stopped_ ? kOscillatorStopped : 0);
  regs_[kMinutes] = to_bcd(t.tm_min);
  regs_[kHours] = to_bcd(t.tm_hour);
  regs_[kDays] = to_bcd(t.tm_mday);
  regs_[kWeekdays] = static_cast<uint8_t>((t.tm_wday + weekday_shift_) % kDaysInWeek);
  regs_[kMonths] = to_bcd(t.tm_mon + 1);
  regs_[kYears] = to_bcd(t.tm_year - kTmYearOf2000);
}

// The chip has no idea of a valid date, and what it does after being given one
// isn't specified. This normalises it, as timegm does.
void Pcf85063::set_time() {
  std::tm t = {};
  t.tm_sec = from_bcd(regs_[kSeconds] & ~kOscillatorStopped);
  t.tm_min = from_bcd(regs_[kMinutes]);
  t.tm_hour = from_bcd(regs_[kHours]);
  t.tm_mday = from_bcd(regs_[kDays]);
  t.tm_mon = from_bcd(regs_[kMonths]) - 1;
  t.tm_year = from_bcd(regs_[kYears]) + kTmYearOf2000;
  const std::time_t written = timegm(&t);
  ahead_of_mac_ = written - std::time(nullptr);
  // Table 25: the weekday is the firmware's to define, and only counts on from
  // what it was given, so it's kept apart from the date.
  weekday_shift_ = (regs_[kWeekdays] - t.tm_wday + kDaysInWeek) % kDaysInWeek;
  oscillator_stopped_ = (regs_[kSeconds] & kOscillatorStopped) != 0;
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
      regs_[reg] = value;
      time_written_ = true;
      break;
    case kMinutes:  // section 8.3.2, table 21
      regs_[reg] = value & 0x7f;
      time_written_ = true;
      break;
    case kHours:  // section 8.3.3, table 22, 24-hour form
    case kDays:   // section 8.3.4, table 23
      regs_[reg] = value & 0x3f;
      time_written_ = true;
      break;
    case kWeekdays:  // section 8.3.5, table 24
      regs_[reg] = value & 0x07;
      time_written_ = true;
      break;
    case kMonths:  // section 8.3.6, table 26
      regs_[reg] = value & 0x1f;
      time_written_ = true;
      break;
    case kYears:  // section 8.3.7, table 28
      regs_[reg] = value;
      time_written_ = true;
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
