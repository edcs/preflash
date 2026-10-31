#include "preflash/chips.h"

namespace preflash {
namespace {

// Table 22.
constexpr uint8_t kWhoAmI = 0x00;
constexpr uint8_t kRevisionId = 0x01;
constexpr uint8_t kCtrl1 = 0x02;
constexpr uint8_t kCtrl2 = 0x03;
constexpr uint8_t kCtrl7 = 0x08;
constexpr uint8_t kStatus0 = 0x2e;
constexpr uint8_t kAccelX = 0x35;
constexpr uint8_t kAccelY = 0x37;
constexpr uint8_t kAccelZHigh = 0x3a;
constexpr uint8_t kReset = 0x60;

// Section 5.4, table 26.
constexpr uint8_t kCtrl1Default = 0x20;
constexpr uint8_t kAutoIncrement = 0x40;
constexpr uint8_t kBigEndian = 0x20;
constexpr uint8_t kSelfTest = 0x80;
constexpr int kFullScaleShift = 4;
constexpr int kLargestFullScale = 3;  // ±16 g
constexpr int kLsbPerGAt2G = 16384;
constexpr uint8_t kAccelEnable = 0x01;

constexpr uint8_t kAccelDataReady = 0x01;  // section 5.6, table 28, STATUS0
constexpr uint8_t kResetCommand = 0xb0;    // section 5.8, table 31

}  // namespace

Qmi8658::Qmi8658() : RegisterChip("qmi8658") { reset(); }

void Qmi8658::reset() {
  regs_.fill(0);
  regs_[kWhoAmI] = 0x05;      // section 5.3, table 25
  regs_[kRevisionId] = 0x68;  // table 22, though table 25 says 0x79
  regs_[kCtrl1] = kCtrl1Default;
}

// Section 5.7, table 29. Only stored while the accelerometer runs, so the
// registers keep their last value when it's off.
void Qmi8658::take_sample() {
  if (!(regs_[kCtrl7] & kAccelEnable)) return;
  const int range = regs_[kCtrl2] >> kFullScaleShift;
  const int y = kLsbPerGAt2G >> range;
  regs_[kAccelY] = static_cast<uint8_t>(y);
  regs_[kAccelY + 1] = static_cast<uint8_t>(y >> 8);
}

uint8_t Qmi8658::load(uint8_t reg) {
  if (reg == kStatus0) return regs_[kCtrl7] & kAccelEnable ? kAccelDataReady : 0;
  // Table 26, CTRL1 bit 5: regs_ holds little-endian, so swap within each pair.
  if (reg >= kAccelX && reg <= kAccelZHigh && (regs_[kCtrl1] & kBigEndian)) {
    return regs_[((reg - kAccelX) ^ 1) + kAccelX];
  }
  return regs_[reg];
}

void Qmi8658::store(uint8_t reg, uint8_t value) {
  switch (reg) {
    case kCtrl1:  // table 26: bits 7 and 0 change the interface and the clock
      if (value & ~(kAutoIncrement | kBigEndian)) refuse(reg, "only bits 6 and 5 are modelled");
      regs_[reg] = value;
      break;
    case kCtrl2:  // table 26: the rate is kept but has no effect, the sample never changes
      if (value & kSelfTest) refuse(reg, "self test has no model");
      if ((value & 0x7f) >> kFullScaleShift > kLargestFullScale) refuse(reg, "reserved full scale");
      regs_[reg] = value;
      take_sample();
      break;
    case kCtrl7:  // table 26
      if (value & ~kAccelEnable) refuse(reg, "only the accelerometer is modelled");
      regs_[reg] = value;
      take_sample();
      break;
    case kReset:  // section 5.8
      if (value != kResetCommand) refuse(reg, "0xb0 is the only reset command");
      reset();
      break;
    default:
      refuse(reg, "writing it has no model");
  }
}

// Table 26, CTRL1 bit 6.
uint8_t Qmi8658::next(uint8_t reg) {
  return regs_[kCtrl1] & kAutoIncrement ? static_cast<uint8_t>(reg + 1) : reg;
}

}  // namespace preflash
