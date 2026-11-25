// The AXP2101's PWRON key and its power-off, from datasheet V1.4: section 6.5.4 for the key's
// behaviour and 6.13.2 for the registers.
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "preflash/chips.h"
#include "preflash/power.h"

using preflash::Axp2101;
using preflash::power::Mode;
using preflash::power::Reset;

namespace {

constexpr uint8_t kCommon = 0x10;
constexpr uint8_t kPwroffEnable = 0x22;
constexpr uint8_t kLevels = 0x27;
constexpr uint8_t kIrqEnable1 = 0x41;
constexpr uint8_t kIrqStatus1 = 0x49;

constexpr uint8_t kShortPress = 0x08;
constexpr uint8_t kLongPress = 0x04;
constexpr uint8_t kNegativeEdge = 0x02;
constexpr uint8_t kPositiveEdge = 0x01;

uint8_t reg(preflash::I2cDevice& chip, uint8_t address) {
  chip.write(std::vector<uint8_t>{address});
  uint8_t value = 0;
  chip.read({&value, 1});
  return value;
}

void put(preflash::I2cDevice& chip, uint8_t address, uint8_t value) {
  chip.write(std::vector<uint8_t>{address, value});
}

// A chip on a clock the test moves by hand.
struct Rig {
  int64_t now = 0;
  Axp2101 pmu{[this] { return now; }};
};

}  // namespace

TEST(Axp2101, SoftOffEntersOffInsteadOfQuitting) {
  std::vector<std::pair<Mode, Reset>> seen;
  preflash::power::set_enter_for_test(
      [&seen](Mode next, Reset reason) { seen.emplace_back(next, reason); });
  Axp2101 pmu;
  put(pmu, kCommon, 0x31);
  preflash::power::set_enter_for_test(nullptr);
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].first, Mode::Off);
  EXPECT_EQ(seen[0].second, Reset::PowerOn);
}

// 6.13.2.19, 6.13.2.24, 6.13.2.41: the eFuse leaves some bits to the maker, and these are the
// stand-ins: power-off on a long hold enabled, a 512 ms ONLEVEL.
TEST(Axp2101, TheKeysRegistersStartAsTheDatasheetSays) {
  Axp2101 pmu;
  EXPECT_EQ(reg(pmu, kPwroffEnable), 0x06);
  EXPECT_EQ(reg(pmu, kLevels), 0x15);  // IRQLEVEL 1.5 s, OFFLEVEL 6 s
  EXPECT_EQ(reg(pmu, kIrqEnable1), 0xfc);
  EXPECT_EQ(reg(pmu, kIrqStatus1), 0x00);
  EXPECT_EQ(reg(pmu, 0x40), 0xff);
  EXPECT_EQ(reg(pmu, 0x42), 0x5f);
  EXPECT_EQ(reg(pmu, 0x48), 0x00);
  EXPECT_EQ(reg(pmu, 0x4a), 0x00);
}

TEST(Axp2101, APwronPressSetsItsInterruptBits) {
  Rig rig;
  rig.pmu.pwron(true);
  EXPECT_EQ(reg(rig.pmu, kIrqStatus1), 0x00);  // edges are off by default
  rig.now = 200;
  rig.pmu.pwron(false);
  EXPECT_EQ(reg(rig.pmu, kIrqStatus1), kShortPress);
  put(rig.pmu, kIrqStatus1, kShortPress);  // write 1 to clear
  EXPECT_EQ(reg(rig.pmu, kIrqStatus1), 0x00);

  put(rig.pmu, kIrqEnable1, 0xff);
  rig.pmu.pwron(true);
  EXPECT_EQ(reg(rig.pmu, kIrqStatus1), kNegativeEdge);
  rig.now = 300;
  rig.pmu.pwron(false);
  EXPECT_EQ(reg(rig.pmu, kIrqStatus1), kNegativeEdge | kShortPress | kPositiveEdge);
  // Clearing one bit leaves the others.
  put(rig.pmu, kIrqStatus1, kNegativeEdge);
  EXPECT_EQ(reg(rig.pmu, kIrqStatus1), kShortPress | kPositiveEdge);

  // A disabled interrupt isn't raised.
  put(rig.pmu, kIrqStatus1, 0xff);
  put(rig.pmu, kIrqEnable1, 0x00);
  rig.pmu.pwron(true);
  rig.pmu.pwron(false);
  EXPECT_EQ(reg(rig.pmu, kIrqStatus1), 0x00);
}

TEST(Axp2101, ALongPwronHoldSwitchesOff) {
  Rig rig;
  rig.pmu.pwron(true);
  rig.now = 1499;
  EXPECT_EQ(rig.pmu.tick(), std::nullopt);
  EXPECT_EQ(reg(rig.pmu, kIrqStatus1), 0x00);
  rig.now = 1500;  // IRQLEVEL
  EXPECT_EQ(rig.pmu.tick(), std::nullopt);
  EXPECT_EQ(reg(rig.pmu, kIrqStatus1), kLongPress);
  rig.now = 5999;
  EXPECT_EQ(rig.pmu.tick(), std::nullopt);
  rig.now = 6000;  // OFFLEVEL
  EXPECT_EQ(rig.pmu.tick(), Mode::Off);
  EXPECT_EQ(rig.pmu.tick(), std::nullopt);  // once
  // A long press is no short one when it ends.
  put(rig.pmu, kIrqStatus1, 0xff);
  rig.pmu.pwron(false);
  EXPECT_EQ(reg(rig.pmu, kIrqStatus1), 0x00);
}

TEST(Axp2101, TheLevelsAndTheEnableSetTheLongHold) {
  Rig rig;
  put(rig.pmu, kLevels, 0x30);  // IRQLEVEL 2.5 s, OFFLEVEL 4 s
  rig.pmu.pwron(true);
  rig.now = 2499;
  rig.pmu.tick();
  EXPECT_EQ(reg(rig.pmu, kIrqStatus1), 0x00);
  rig.now = 2500;
  rig.pmu.tick();
  EXPECT_EQ(reg(rig.pmu, kIrqStatus1), kLongPress);
  rig.now = 4000;
  EXPECT_EQ(rig.pmu.tick(), Mode::Off);
  rig.pmu.pwron(false);

  // REG22H[1] off: a hold never switches off.
  put(rig.pmu, kPwroffEnable, 0x04);
  rig.now = 10000;
  rig.pmu.pwron(true);
  rig.now = 30000;
  EXPECT_EQ(rig.pmu.tick(), std::nullopt);
  rig.pmu.pwron(false);

  // REG22H[0] on: the PMU restarts the board instead.
  put(rig.pmu, kPwroffEnable, 0x07);
  rig.pmu.pwron(true);
  rig.now = 34000;
  EXPECT_EQ(rig.pmu.tick(), Mode::On);
}

// The press that powered the board on began before the chip did, and its release is no
// short press.
TEST(Axp2101, AReleaseWithNoPressRaisesNothing) {
  Rig rig;
  put(rig.pmu, kIrqEnable1, 0xff);
  rig.pmu.pwron(false);
  EXPECT_EQ(reg(rig.pmu, kIrqStatus1), 0x00);
}

// A key let go between two ticks still counts for as long as it was held: a tap as long as
// IRQLEVEL is a long press, and one as long as OFFLEVEL switches off at the next tick.
TEST(Axp2101, AReleaseBeforeTheTickStillReachesTheLevels) {
  Rig rig;
  rig.pmu.pwron(true);
  rig.now = 1500;  // IRQLEVEL, and no tick saw it
  rig.pmu.pwron(false);
  EXPECT_EQ(reg(rig.pmu, kIrqStatus1), kLongPress);
  EXPECT_EQ(rig.pmu.tick(), std::nullopt);

  put(rig.pmu, kIrqStatus1, 0xff);
  rig.now = 10000;
  rig.pmu.pwron(true);
  rig.now = 16000;  // OFFLEVEL, and no tick saw it
  rig.pmu.pwron(false);
  EXPECT_EQ(reg(rig.pmu, kIrqStatus1), kLongPress);
  EXPECT_EQ(rig.pmu.tick(), Mode::Off);
  EXPECT_EQ(rig.pmu.tick(), std::nullopt);  // once
}
