#include <algorithm>
#include <chrono>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "preflash/chips.h"
#include "preflash/hardware.h"
#include "preflash/power.h"

namespace preflash {
namespace {

// Section 6.13.1, the register list. Section numbers are datasheet V1.0's, which V1.4 keeps.
constexpr uint8_t kStatus1 = 0x00;
constexpr uint8_t kStatus2 = 0x01;
constexpr uint8_t kChipId = 0x03;
constexpr uint8_t kCommon = 0x10;
constexpr uint8_t kPwroffEnable = 0x22;
constexpr uint8_t kLevels = 0x27;
constexpr uint8_t kIrqEnable0 = 0x40;
constexpr uint8_t kIrqEnable1 = 0x41;
constexpr uint8_t kIrqEnable2 = 0x42;
constexpr uint8_t kIrqStatus0 = 0x48;
constexpr uint8_t kIrqStatus1 = 0x49;
constexpr uint8_t kIrqStatus2 = 0x4a;
constexpr uint8_t kPrecharge = 0x61;
constexpr uint8_t kCharge = 0x62;
constexpr uint8_t kTermination = 0x63;
constexpr uint8_t kChargeVoltage = 0x64;
constexpr uint8_t kGauge = 0xa4;

constexpr uint8_t kVbusGood = 0x20;        // 6.13.2.1
constexpr uint8_t kBatteryPresent = 0x08;  // 6.13.2.1
constexpr uint8_t kSoftOff = 0x01;         // 6.13.2.7
// 6.13.2.2: system on, with the battery's current and the charger's state in the other bits.
constexpr uint8_t kNoCurrentNotCharging = 0x15;
constexpr uint8_t kCharging = 0x32;
constexpr uint8_t kChargeDone = 0x14;
constexpr uint8_t kDischarging = 0x55;

// 6.13.2.19: bit 1 lets a PWRON hold past OFFLEVEL power off, and bit 0 makes that a restart.
constexpr uint8_t kOffOnHold = 0x02;
constexpr uint8_t kRestartOnHold = 0x01;
// 6.13.2.41 and 6.13.2.44: PWRON's interrupts, the same bits in enable and status.
constexpr uint8_t kShortPress = 0x08;
constexpr uint8_t kLongPress = 0x04;
constexpr uint8_t kNegativeEdge = 0x02;  // PWRON pulled low: the key goes down
constexpr uint8_t kPositiveEdge = 0x01;  // and comes up

// 6.13.2.24: IRQLEVEL in bits 5:4, 1 s to 2.5 s, and OFFLEVEL in bits 3:2, 4 s to 10 s.
int64_t irq_level_ms(uint8_t levels) { return 1000 + 500 * (levels >> 4 & 0x3); }
int64_t off_level_ms(uint8_t levels) { return 4000 + 2000 * (levels >> 2 & 0x3); }

int64_t steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint8_t status1() {
  return (hardware::usb_in() ? kVbusGood : 0) | (hardware::battery_percent() >= 0 ? kBatteryPresent : 0);
}

uint8_t status2() {
  const int percent = hardware::battery_percent();
  if (percent < 0) return kNoCurrentNotCharging;
  if (!hardware::usb_in()) return kDischarging;
  return percent < 100 ? kCharging : kChargeDone;
}

// The chips alive now, the last the board's, as Pcf85063 keeps them.
std::mutex g_live_mutex;
std::vector<Axp2101*> g_live;
std::once_flag g_ticking;

// Times a key held down on the board's chip, every kPowerPollMs, for as long as the run lasts.
void tick_forever() {
  for (;;) {
    std::optional<power::Mode> next;
    {
      std::lock_guard<std::mutex> lock(g_live_mutex);
      if (!g_live.empty()) next = g_live.back()->tick();
    }
    // Outside the lock and on this thread, which holds nothing enter needs.
    if (next) power::switch_to(*next, power::Reset::PowerOn);
    std::this_thread::sleep_for(std::chrono::milliseconds(power::kPowerPollMs));
  }
}

}  // namespace

Axp2101::Axp2101() : Axp2101(steady_ms) {}

Axp2101::Axp2101(std::function<int64_t()> clock_ms)
    : RegisterChip("axp2101"), clock_(std::move(clock_ms)) {
  // The datasheet leaves this value out. It's the one drivers check for.
  regs_[kChipId] = 0x4a;
  regs_[kCommon] = 0x30;  // 6.13.2.7
  // 6.13.2.19: bits 1 and 0 come from the eFuse. These say a long hold powers off, which is
  // what the datasheet's section 6.5.4.3 describes.
  regs_[kPwroffEnable] = 0x06;
  // 6.13.2.24: IRQLEVEL 1.5 s, OFFLEVEL 6 s, and ONLEVEL from the eFuse, here 512 ms.
  regs_[kLevels] = 0x15;
  regs_[kIrqEnable0] = 0xff;     // 6.13.2.40
  regs_[kIrqEnable1] = 0xfc;     // 6.13.2.41: the edges are off
  regs_[kIrqEnable2] = 0x5f;     // 6.13.2.42
  regs_[kPrecharge] = 0x05;      // 6.13.2.59: 125 mA
  regs_[kCharge] = 0x08;         // 6.13.2.60: set by eFuse, so this 200 mA is a stand-in
  regs_[kTermination] = 0x15;    // 6.13.2.61: on, 125 mA
  regs_[kChargeVoltage] = 0x03;  // 6.13.2.62: 4.2 V
  std::lock_guard<std::mutex> lock(g_live_mutex);
  g_live.push_back(this);
}

Axp2101::~Axp2101() {
  std::lock_guard<std::mutex> lock(g_live_mutex);
  g_live.erase(std::remove(g_live.begin(), g_live.end(), this), g_live.end());
}

// The soft power-off acts once the write is done and the chip's lock is free, as the board
// goes dark the moment the bit lands.
bool Axp2101::write(std::span<const uint8_t> data) {
  bool off = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RegisterChip::write(data);
    off = std::exchange(soft_off_, false);
  }
  if (off) power::switch_to(power::Mode::Off, power::Reset::PowerOn);
  return true;
}

bool Axp2101::read(std::span<uint8_t> out) {
  std::lock_guard<std::mutex> lock(mutex_);
  return RegisterChip::read(out);
}

void Axp2101::raise(uint8_t bits) { regs_[kIrqStatus1] |= bits & regs_[kIrqEnable1]; }

// 6.5.4.1: the chip tells a long press, a short press and both edges apart. The datasheet
// says no more about a short press than its name, so it's raised as a key that comes up
// before IRQLEVEL. Firmware reads it in 6.13.2.44 and writes 1 to clear it.
void Axp2101::pwron(bool down) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (down) {
    pressed_at_ = clock_();
    long_raised_ = false;
    off_named_ = false;
    raise(kNegativeEdge);
    return;
  }
  // The press that powered the board on began before this chip did, and raised nothing.
  if (!pressed_at_) return;
  raise(kPositiveEdge);
  if (!long_raised_ && clock_() - *pressed_at_ < irq_level_ms(regs_[kLevels])) raise(kShortPress);
  pressed_at_.reset();
}

std::optional<power::Mode> Axp2101::tick() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!pressed_at_) return std::nullopt;
  const int64_t held = clock_() - *pressed_at_;
  if (!long_raised_ && held >= irq_level_ms(regs_[kLevels])) {
    long_raised_ = true;
    raise(kLongPress);
  }
  // 6.5.4.3: a hold past OFFLEVEL powers off, if REG22H[1] lets it, or restarts.
  const uint8_t enable = regs_[kPwroffEnable];
  if (off_named_ || !(enable & kOffOnHold) || held < off_level_ms(regs_[kLevels])) {
    return std::nullopt;
  }
  off_named_ = true;
  return enable & kRestartOnHold ? power::Mode::On : power::Mode::Off;
}

uint8_t Axp2101::load(uint8_t reg) {
  switch (reg) {
    case kStatus1:
      return status1();
    case kStatus2:
      return status2();
    case kGauge:  // 6.13.2.88: 0 with nothing to measure
      return static_cast<uint8_t>(std::max(hardware::battery_percent(), 0));
    case kIrqStatus0:  // 6.13.2.43 and 6.13.2.45: nothing modelled raises these
    case kIrqStatus2:
      return 0;
    case kChipId:
    case kCommon:
    case kPwroffEnable:
    case kLevels:
    case kIrqEnable0:
    case kIrqEnable1:
    case kIrqEnable2:
    case kIrqStatus1:
    case kPrecharge:
    case kCharge:
    case kTermination:
    case kChargeVoltage:
      return regs_[reg];
    default:
      refuse(reg, "reading it has no model");
  }
}

// The charger's settings are only stored, because no battery means nothing to
// charge. Each mask leaves out the read-only bits.
void Axp2101::store(uint8_t reg, uint8_t value) {
  switch (reg) {
    case kCommon:  // 6.13.2.7
      if ((value ^ regs_[reg]) & ~kSoftOff) refuse(reg, "only the soft power-off bit is modelled");
      if (value & kSoftOff) soft_off_ = true;
      break;
    case kPwroffEnable:  // 6.13.2.19
      regs_[reg] = value & 0x07;
      break;
    case kLevels:  // 6.13.2.24
      regs_[reg] = value & 0x3f;
      break;
    case kIrqEnable0:  // 6.13.2.40
    case kIrqEnable1:  // 6.13.2.41
      regs_[reg] = value;
      break;
    case kIrqEnable2:  // 6.13.2.42: bit 5 is reserved
      regs_[reg] = value & 0xdf;
      break;
    case kIrqStatus1:  // 6.13.2.44: write 1 to clear
      regs_[reg] &= static_cast<uint8_t>(~value);
      break;
    case kIrqStatus0:  // 6.13.2.43 and 6.13.2.45: always clear here
    case kIrqStatus2:
      break;
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

void axp2101_pwron(bool down) {
  {
    std::lock_guard<std::mutex> lock(g_live_mutex);
    if (g_live.empty()) return;
    g_live.back()->pwron(down);
  }
  if (!down) return;
  std::call_once(g_ticking, [] {
    try {
      std::thread(tick_forever).detach();
    } catch (const std::system_error&) {
      // With no thread a hold never times out, and presses still raise their interrupts.
    }
  });
}

}  // namespace preflash
