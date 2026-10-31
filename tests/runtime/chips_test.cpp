#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "preflash/board.h"
#include "preflash/bus.h"
#include "preflash/chips.h"
#include "preflash/panel.h"

namespace fs = std::filesystem;

using preflash::attach_chips;
using preflash::Axp2101;
using preflash::Board;
using preflash::i2c_bus;
using preflash::I2cDevice;
using preflash::Pcf85063;
using preflash::Qmi8658;
using preflash::register_chip;
using preflash::RegisterChip;

namespace {

void put(I2cDevice& chip, uint8_t reg, uint8_t value) {
  const std::array<uint8_t, 2> frame = {reg, value};
  ASSERT_TRUE(chip.write(frame));
}

template <size_t N>
std::array<uint8_t, N> take(I2cDevice& chip, uint8_t reg) {
  const std::array<uint8_t, 1> start = {reg};
  EXPECT_TRUE(chip.write(start));
  std::array<uint8_t, N> out{};
  EXPECT_TRUE(chip.read(out));
  return out;
}

uint8_t take(I2cDevice& chip, uint8_t reg) { return take<1>(chip, reg)[0]; }

constexpr int bcd(int value) { return (value / 10) << 4 | value % 10; }
constexpr int decimal(int value) { return (value >> 4) * 10 + (value & 0x0f); }

// The RTC's seven time registers as a Unix time.
std::time_t rtc_seconds(const std::array<uint8_t, 7>& regs) {
  std::tm t = {};
  t.tm_sec = decimal(regs[0] & 0x7f);
  t.tm_min = decimal(regs[1] & 0x7f);
  t.tm_hour = decimal(regs[2] & 0x3f);
  t.tm_mday = decimal(regs[3] & 0x3f);
  t.tm_mon = decimal(regs[5] & 0x1f) - 1;
  t.tm_year = decimal(regs[6]) + 100;
  return timegm(&t);
}

int16_t little_endian(uint8_t low, uint8_t high) {
  return static_cast<int16_t>(low | (high << 8));
}

constexpr uint8_t kCtrl1 = 0x02;
constexpr uint8_t kCtrl2 = 0x03;
constexpr uint8_t kCtrl7 = 0x08;
constexpr uint8_t kAccelX = 0x35;
constexpr uint8_t kAutoIncrementLittleEndian = 0x40;

}  // namespace

TEST(Registry, UnknownChipNamesIt) {
  Board board{};
  board.i2c = {{"made_up_chip", 0x10}};
  try {
    attach_chips(board);
    FAIL() << "attach_chips accepted a chip with no model";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("made_up_chip"), std::string::npos) << e.what();
  }
}

TEST(Registry, RejectsTwoChipsAtOneAddress) {
  Board board{};
  board.i2c = {{"axp2101", 0x20}, {"pcf85063", 0x20}};
  EXPECT_THROW(attach_chips(board), std::runtime_error);
}

TEST(Registry, RegisteredChipIsAttached) {
  register_chip("test_chip", [] { return std::make_unique<RegisterChip>("test_chip"); });
  Board board{};
  board.i2c = {{"test_chip", 0x21}};
  attach_chips(board);
  EXPECT_NE(i2c_bus().device(0x21), nullptr);
}

TEST(Registry, EveryChipOnTheStockBoardHasAModel) {
  Board board{};
  board.i2c = {{"tca9554", 0x20}, {"es8311", 0x18},  {"es7210", 0x40},  {"gt911", 0x5d},
               {"axp2101", 0x34}, {"pcf85063", 0x51}, {"qmi8658", 0x6b}};
  attach_chips(board);
  for (const auto& entry : board.i2c)
    EXPECT_NE(i2c_bus().device(entry.address), nullptr) << entry.chip;
}

TEST(RegisterChip, AutoIncrements) {
  RegisterChip chip("plain");
  const std::array<uint8_t, 4> frame = {0x10, 0xaa, 0xbb, 0xcc};
  ASSERT_TRUE(chip.write(frame));
  EXPECT_EQ(take<3>(chip, 0x10), (std::array<uint8_t, 3>{0xaa, 0xbb, 0xcc}));
}

TEST(RegisterChip, ReadCarriesOnFromTheLastOne) {
  RegisterChip chip("plain");
  const std::array<uint8_t, 3> frame = {0x20, 0x01, 0x02};
  ASSERT_TRUE(chip.write(frame));
  EXPECT_EQ(take(chip, 0x20), 0x01);
  std::array<uint8_t, 1> next{};
  ASSERT_TRUE(chip.read(next));
  EXPECT_EQ(next[0], 0x02);
}

TEST(Pcf85063, ReadsTheMacClockAsBcd) {
  Pcf85063 rtc;
  const std::time_t before = std::time(nullptr);
  const auto regs = take<7>(rtc, 0x04);
  const std::time_t after = std::time(nullptr);

  EXPECT_EQ(regs[0] & 0x80, 0) << "oscillator stop flag";
  const std::time_t seen = rtc_seconds(regs);
  EXPECT_GE(seen, before);
  EXPECT_LE(seen, after);

  std::tm t = {};
  gmtime_r(&seen, &t);
  EXPECT_EQ(regs[4], t.tm_wday);
  EXPECT_EQ(regs[6], bcd(t.tm_year - 100));
}

TEST(Pcf85063, WriteMovesTheClock) {
  Pcf85063 rtc;
  // Sunday 2 March 2031, 12:34:56.
  const std::array<uint8_t, 8> frame = {0x04, 0x56, 0x34, 0x12, 0x02, 0x00, 0x03, 0x31};
  ASSERT_TRUE(rtc.write(frame));
  const auto regs = take<7>(rtc, 0x04);

  std::tm t = {};
  t.tm_sec = 56, t.tm_min = 34, t.tm_hour = 12, t.tm_mday = 2, t.tm_mon = 2, t.tm_year = 131;
  const std::time_t expected = timegm(&t);
  EXPECT_GE(rtc_seconds(regs), expected);
  EXPECT_LE(rtc_seconds(regs), expected + 2);
  EXPECT_EQ(regs[4], 0);
}

TEST(Pcf85063, SettingOneFieldKeepsTheOthers) {
  Pcf85063 rtc;
  // Wednesday 15 January 2031, noon.
  const std::array<uint8_t, 8> frame = {0x04, 0x00, 0x00, 0x12, 0x15, 0x03, 0x01, 0x31};
  ASSERT_TRUE(rtc.write(frame));
  put(rtc, 0x0a, 0x32);
  const auto regs = take<7>(rtc, 0x04);
  EXPECT_EQ(regs[6], 0x32);
  EXPECT_EQ(regs[5], 0x01);
  EXPECT_EQ(regs[3], 0x15);
}

TEST(Pcf85063, DateSetOneFieldAtATimeCanPassThroughAnImpossibleDay) {
  Pcf85063 rtc;
  // 10 February, then the 31st, then January: only the end of that is a date.
  const std::array<uint8_t, 8> frame = {0x04, 0x00, 0x00, 0x12, 0x10, 0x01, 0x02, 0x31};
  ASSERT_TRUE(rtc.write(frame));
  put(rtc, 0x07, 0x31);
  put(rtc, 0x09, 0x01);
  EXPECT_EQ(take(rtc, 0x07), 0x31);
  EXPECT_EQ(take(rtc, 0x09), 0x01);
}

TEST(Pcf85063, MidnightMovesTheDateAndTheWeekday) {
  Pcf85063 rtc;
  // 23:59:59 on the last day of February 2031, with a weekday of the firmware's own.
  const std::array<uint8_t, 8> frame = {0x04, 0x59, 0x59, 0x23, 0x28, 0x06, 0x02, 0x31};
  ASSERT_TRUE(rtc.write(frame));
  sleep(2);
  const auto regs = take<7>(rtc, 0x04);
  EXPECT_EQ(regs[2], 0x00) << "hours";
  EXPECT_EQ(regs[3], 0x01) << "day";
  EXPECT_EQ(regs[4], 0x00) << "weekday";
  EXPECT_EQ(regs[5], 0x03) << "month";
}

TEST(Pcf85063, WeekdayIsWhateverWasWritten) {
  // The chip counts weekdays without checking them against the date.
  Pcf85063 rtc;
  const uint8_t today = take(rtc, 0x08);
  const uint8_t other = (today + 3) % 7;
  put(rtc, 0x08, other);
  EXPECT_EQ(take(rtc, 0x08), other);
}

TEST(Pcf85063, FlatCellSetsOscillatorStop) {
  Pcf85063 rtc;
  rtc.set_lost_power(true);
  EXPECT_EQ(take(rtc, 0x04) & 0x80, 0x80);
}

TEST(Pcf85063, WritingSecondsClearsOscillatorStop) {
  Pcf85063 rtc;
  rtc.set_lost_power(true);
  put(rtc, 0x04, 0x00);
  EXPECT_EQ(take(rtc, 0x04) & 0x80, 0);
}

TEST(Pcf85063, AddressWrapsAfterTheLastRegister) {
  Pcf85063 rtc;
  EXPECT_EQ(take<2>(rtc, 0x11), (std::array<uint8_t, 2>{0x18, 0x00}));
}

TEST(Pcf85063, ResetValuesOfRegistersItDoesntRun) {
  Pcf85063 rtc;
  EXPECT_EQ(take(rtc, 0x0b), 0x80);
  EXPECT_EQ(take(rtc, 0x0f), 0x80);
  EXPECT_EQ(take(rtc, 0x10), 0x00);
}

TEST(Pcf85063, RamByteAndOffsetReadBack) {
  Pcf85063 rtc;
  put(rtc, 0x02, 0x12);
  put(rtc, 0x03, 0x5a);
  EXPECT_EQ(take(rtc, 0x02), 0x12);
  EXPECT_EQ(take(rtc, 0x03), 0x5a);
}

TEST(Pcf85063, RunningInTwentyFourHourModeIsAccepted) {
  Pcf85063 rtc;
  put(rtc, 0x00, 0x00);
  EXPECT_EQ(take(rtc, 0x00), 0x00);
}

TEST(Pcf85063DeathTest, UnmodelledWriteNamesChipAndRegister) {
  Pcf85063 rtc;
  EXPECT_DEATH(put(rtc, 0x0b, 0x00), "pcf85063 register 0x0b");
  EXPECT_DEATH(put(rtc, 0x01, 0x80), "pcf85063 register 0x01");
}

TEST(Pcf85063DeathTest, ControlBitsWithNoModelStopTheRun) {
  Pcf85063 rtc;
  EXPECT_DEATH(put(rtc, 0x00, 0x20), "pcf85063 register 0x00");  // STOP
  EXPECT_DEATH(put(rtc, 0x00, 0x58), "pcf85063 register 0x00");  // software reset
}

TEST(Qmi8658, WhoAmIIsChipId) {
  Qmi8658 imu;
  EXPECT_EQ(take(imu, 0x00), 0x05);
}

TEST(Qmi8658, UprightReadsOneGOnY) {
  Qmi8658 imu;
  put(imu, kCtrl1, kAutoIncrementLittleEndian);
  put(imu, kCtrl2, 0x08);  // ±2 g
  put(imu, kCtrl7, 0x01);
  const auto data = take<6>(imu, kAccelX);
  EXPECT_EQ(little_endian(data[0], data[1]), 0);
  EXPECT_EQ(little_endian(data[2], data[3]), 16384);
  EXPECT_EQ(little_endian(data[4], data[5]), 0);
}

TEST(Qmi8658, AccelOffUntilEnabled) {
  Qmi8658 imu;
  put(imu, kCtrl1, kAutoIncrementLittleEndian);
  put(imu, kCtrl2, 0x08);
  EXPECT_EQ(take<6>(imu, kAccelX), (std::array<uint8_t, 6>{}));
  EXPECT_EQ(take(imu, 0x2e) & 0x01, 0) << "no data ready yet";
  put(imu, kCtrl7, 0x01);
  EXPECT_NE(take<6>(imu, kAccelX), (std::array<uint8_t, 6>{}));
  EXPECT_EQ(take(imu, 0x2e) & 0x01, 0x01) << "data ready";
}

TEST(Qmi8658, FullScaleSetsTheSensitivity) {
  Qmi8658 imu;
  put(imu, kCtrl1, kAutoIncrementLittleEndian);
  put(imu, kCtrl2, 0x38);  // ±16 g
  put(imu, kCtrl7, 0x01);
  const auto data = take<6>(imu, kAccelX);
  EXPECT_EQ(little_endian(data[2], data[3]), 2048);
}

TEST(Qmi8658, PowersUpBigEndianWithoutAutoIncrement) {
  Qmi8658 imu;
  put(imu, kCtrl7, 0x01);
  EXPECT_EQ(take(imu, kCtrl1), 0x20);
  // 16384 is 0x4000, and big-endian puts the high byte in the low register.
  EXPECT_EQ(take(imu, 0x37), 0x40);
  EXPECT_EQ(take(imu, 0x38), 0x00);
  // The address stays put, so a burst repeats one register.
  EXPECT_EQ(take<2>(imu, 0x37), (std::array<uint8_t, 2>{0x40, 0x40}));
}

TEST(Qmi8658, ResetRestoresTheDefaults) {
  Qmi8658 imu;
  put(imu, kCtrl1, kAutoIncrementLittleEndian);
  put(imu, kCtrl7, 0x01);
  put(imu, 0x60, 0xb0);
  EXPECT_EQ(take(imu, kCtrl1), 0x20);
  EXPECT_EQ(take(imu, kCtrl7), 0x00);
  EXPECT_EQ(take(imu, 0x38), 0x00);
  EXPECT_EQ(take(imu, 0x00), 0x05);
}

TEST(Qmi8658DeathTest, UnmodelledWriteNamesChipAndRegister) {
  Qmi8658 imu;
  EXPECT_DEATH(put(imu, 0x04, 0x00), "qmi8658 register 0x04");  // gyroscope
  EXPECT_DEATH(put(imu, kCtrl7, 0x02), "qmi8658 register 0x08");  // gyroscope on
  EXPECT_DEATH(put(imu, 0x60, 0x01), "qmi8658 register 0x60");
}

TEST(Axp2101, ReportsVbusGoodAndNoBattery) {
  Axp2101 pmu;
  const uint8_t status = take(pmu, 0x00);
  EXPECT_EQ(status & 0x20, 0x20) << "VBUS good";
  EXPECT_EQ(status & 0x08, 0x00) << "battery present";
  EXPECT_EQ(take(pmu, 0x01) & 0x07, 0x05) << "not charging";
  EXPECT_EQ(take(pmu, 0xa4), 0x00) << "gauge";
}

TEST(Axp2101, ChipIdReads) {
  Axp2101 pmu;
  EXPECT_EQ(take(pmu, 0x03), 0x4a);
}

TEST(Axp2101, ChargerSettingsReadBack) {
  Axp2101 pmu;
  put(pmu, 0x61, 0x01);
  put(pmu, 0x62, 0x04);
  put(pmu, 0x63, 0x11);
  put(pmu, 0x64, 0x02);
  EXPECT_EQ(take(pmu, 0x61), 0x01);
  EXPECT_EQ(take(pmu, 0x62), 0x04);
  EXPECT_EQ(take(pmu, 0x63), 0x11);
  EXPECT_EQ(take(pmu, 0x64), 0x02);
}

TEST(Axp2101, ChargerDefaults) {
  Axp2101 pmu;
  EXPECT_EQ(take(pmu, 0x61), 0x05);
  EXPECT_EQ(take(pmu, 0x63), 0x15);
  EXPECT_EQ(take(pmu, 0x64), 0x03);
}

TEST(Axp2101, ReadOnlyBitsStayClear) {
  Axp2101 pmu;
  put(pmu, 0x64, 0xfa);
  EXPECT_EQ(take(pmu, 0x64), 0x02);
}

TEST(Axp2101DeathTest, UnmodelledWriteNamesChipAndRegister) {
  Axp2101 pmu;
  EXPECT_DEATH(put(pmu, 0x10, 0x31), "axp2101 register 0x10");  // soft power-off
  EXPECT_DEATH(put(pmu, 0x90, 0x01), "axp2101 register 0x90");
}

// The board loads once per process, so this runs in a process of its own with
// a board that has an enable pin.
TEST(Backlight, EnablePinBlanksTheScreen) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  const fs::path path =
      fs::temp_directory_path() / ("preflash_backlight_" + std::to_string(getpid()) + ".json");
  std::ofstream(path) << R"({"name": "Test", "display": {"width": 16, "height": 8}, "i2c": [],
                             "backlight": {"pin": 4, "active_low": true, "enable_pin": 43},
                             "audio_codec": "none"})";
  EXPECT_EXIT(
      {
        setenv("PREFLASH_BOARD", path.c_str(), 1);
        preflash::panel::set_brightness(70);
        preflash::gpio_write(43, true);
        if (preflash::panel::brightness() != 70) _exit(1);
        preflash::gpio_write(43, false);
        if (preflash::panel::brightness() != 0) _exit(2);
        // Whatever the level, and it comes back without being set again.
        preflash::panel::set_brightness(100);
        if (preflash::panel::brightness() != 0) _exit(3);
        preflash::gpio_write(43, true);
        if (preflash::panel::brightness() != 100) _exit(4);
        // Firmware's own setting isn't touched by the pin.
        preflash::gpio_write(43, false);
        if (bsp_display_brightness_get() != 100) _exit(5);
        _exit(0);
      },
      testing::ExitedWithCode(0), "");
  fs::remove(path);
}

TEST(BspI2c, HandleReachesTheBus) {
  EXPECT_EQ(bsp_i2c_init(), ESP_OK);
  i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
  ASSERT_NE(bus, nullptr);
  i2c_bus().add(0x34, std::make_unique<Axp2101>());

  i2c_device_config_t config = {};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = 0x34;
  i2c_master_dev_handle_t device = nullptr;
  ASSERT_EQ(i2c_master_bus_add_device(bus, &config, &device), ESP_OK);
  const uint8_t reg = 0x00;
  uint8_t status = 0;
  ASSERT_EQ(i2c_master_transmit_receive(device, &reg, 1, &status, 1, 100), ESP_OK);
  EXPECT_EQ(status & 0x20, 0x20);
}

TEST(BspBacklight, PinComesFromTheBoard) {
  // The panel suite's test board puts it on GPIO 4.
  EXPECT_EQ(BSP_LCD_BACKLIGHT, GPIO_NUM_4);
}
