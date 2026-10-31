#include "preflash/chips.h"

namespace preflash {
namespace {

// Section 6.13.1, the register list.
constexpr uint8_t kStatus1 = 0x00;
constexpr uint8_t kStatus2 = 0x01;
constexpr uint8_t kChipId = 0x03;
constexpr uint8_t kCommon = 0x10;
constexpr uint8_t kPrecharge = 0x61;
constexpr uint8_t kCharge = 0x62;
constexpr uint8_t kTermination = 0x63;
constexpr uint8_t kChargeVoltage = 0x64;

}  // namespace

Axp2101::Axp2101() : RegisterChip("axp2101") {
  // 6.13.2.1: VBUS good, and bit 3 clear because there's no battery.
  regs_[kStatus1] = 0x20;
  // 6.13.2.2: system on, battery current standby, not charging.
  regs_[kStatus2] = 0x15;
  // The datasheet leaves this value out. It's the one drivers check for.
  regs_[kChipId] = 0x4a;
  regs_[kCommon] = 0x30;         // 6.13.2.7
  regs_[kPrecharge] = 0x05;      // 6.13.2.59: 125 mA
  regs_[kCharge] = 0x08;         // 6.13.2.60: set by eFuse, so this 200 mA is a stand-in
  regs_[kTermination] = 0x15;    // 6.13.2.61: on, 125 mA
  regs_[kChargeVoltage] = 0x03;  // 6.13.2.62: 4.2 V
  // The gauge at 0xa4 (6.13.2.88) stays at its default of 0, with nothing to measure.
}

// The charger's settings are only stored, because no battery means nothing to
// charge. Each mask leaves out the read-only bits.
void Axp2101::store(uint8_t reg, uint8_t value) {
  switch (reg) {
    case kPrecharge:  // 6.13.2.59
      regs_[reg] = value & 0x0f;
      break;
    case kCharge:  // 6.13.2.60
      regs_[reg] = value & 0x1f;
      break;
    case kTermination:  // 6.13.2.61
      regs_[reg] = value & 0x1f;
      break;
    case kChargeVoltage:  // 6.13.2.62
      regs_[reg] = value & 0x07;
      break;
    default:
      refuse(reg, "writing it has no model");
  }
}

}  // namespace preflash
