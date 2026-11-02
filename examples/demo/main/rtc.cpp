#include "rtc.h"

#include "i2c_chip.h"
#include "timekeeper.h"

namespace rtc {
namespace {

constexpr uint8_t kAddress = 0x51;
constexpr uint8_t kFirstTimeRegister = 0x04;  // seconds, then minutes up to years
constexpr uint8_t kOscillatorStopped = 0x80;  // top bit of seconds
constexpr int kBaseYear = 2000;

I2cChip g_chip;

constexpr uint8_t to_bcd(int value) { return static_cast<uint8_t>((value / 10) << 4 | value % 10); }
constexpr int from_bcd(uint8_t value) { return (value >> 4) * 10 + (value & 0x0f); }

}  // namespace

void start(i2c_master_bus_handle_t bus) { g_chip.attach(bus, kAddress); }

Status read(time_t* utc) {
  uint8_t regs[7];
  if (g_chip.read(kFirstTimeRegister, regs, sizeof regs) != ESP_OK) return Status::no_answer;
  if (regs[0] & kOscillatorStopped) return Status::time_lost;
  *utc = timekeeper::epoch_from_date(kBaseYear + from_bcd(regs[6]), from_bcd(regs[5] & 0x1f), from_bcd(regs[3] & 0x3f),
                                     from_bcd(regs[2] & 0x3f), from_bcd(regs[1] & 0x7f), from_bcd(regs[0] & 0x7f));
  return Status::ok;
}

Status write(time_t utc) {
  std::tm date;
  gmtime_r(&utc, &date);
  const uint8_t regs[7] = {
      to_bcd(date.tm_sec),     to_bcd(date.tm_min),      to_bcd(date.tm_hour),
      to_bcd(date.tm_mday),    static_cast<uint8_t>(date.tm_wday),
      to_bcd(date.tm_mon + 1), to_bcd((date.tm_year + 1900 - kBaseYear) % 100),
  };
  return g_chip.write(kFirstTimeRegister, regs, sizeof regs) == ESP_OK ? Status::ok : Status::no_answer;
}

}  // namespace rtc
