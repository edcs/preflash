#include "preflash/bus.h"
#include "preflash/chips.h"

namespace preflash {
namespace {

// Table 3.
constexpr uint8_t kInput = 0x00;
constexpr uint8_t kOutput = 0x01;
constexpr uint8_t kPolarity = 0x02;
constexpr uint8_t kConfig = 0x03;  // 1: input
constexpr uint32_t kPins = 0xff;

}  // namespace

Tca9554::Tca9554() : RegisterChip("tca9554") { expander_power_on(); }

uint8_t Tca9554::load(uint8_t reg) {
  const ExpanderPins pins = expander_pins();
  const auto config = static_cast<uint8_t>(~pins.outputs);
  switch (reg) {
    case kInput: {
      const uint32_t driven = pins.levels & pins.outputs;
      return static_cast<uint8_t>((driven | ~pins.outputs) ^ regs_[kPolarity]);
    }
    case kOutput:
      return static_cast<uint8_t>(pins.levels);
    case kPolarity:
      return regs_[kPolarity];
    case kConfig:
      return config;
    default:
      refuse(reg, "the chip has four registers");
  }
}

void Tca9554::store(uint8_t reg, uint8_t value) {
  switch (reg) {
    case kInput:  // writes to it have no effect
      break;
    case kOutput:
      expander_set_levels(kPins, value);
      break;
    case kPolarity:
      regs_[kPolarity] = value;
      break;
    case kConfig:
      expander_set_outputs(kPins, static_cast<uint8_t>(~value));
      break;
    default:
      refuse(reg, "the chip has four registers");
  }
}

}  // namespace preflash
