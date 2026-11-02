#include <gtest/gtest.h>

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "esp_http_client.h"
#include "preflash/board.h"
#include "preflash/bus.h"
#include "preflash/chips.h"
#include "preflash/hardware.h"
#include "preflash/net.h"
#include "preflash/trace.h"

using nlohmann::json;
using preflash::Axp2101;
using preflash::Board;
using preflash::Pcf85063;
using preflash::Qmi8658;
namespace fs = std::filesystem;
namespace hardware = preflash::hardware;

namespace {

constexpr uint8_t kRtc = 0x51;
constexpr uint8_t kMotion = 0x6b;

Board four_inch() {
  Board board{};
  board.i2c = {{"pcf85063", kRtc, 0}, {"qmi8658", kMotion, 0}, {"axp2101", 0x34, 0}};
  return board;
}

class Hardware : public ::testing::Test {
 protected:
  void SetUp() override {
    state_ = fs::temp_directory_path() / ("pfhw" + std::to_string(getpid()));
    fs::remove_all(state_);
    fs::create_directories(state_);
    setenv("PREFLASH_STATE", state_.c_str(), 1);
    hardware::load(four_inch());
  }
  void TearDown() override {
    fs::remove_all(state_);
    hardware::load(four_inch());
    unsetenv("PREFLASH_STATE");
    hardware::load(Board{});
  }

  json item(const std::string& name) {
    for (const json& entry : hardware::describe()) {
      if (entry["name"] == name) return entry;
    }
    return json();
  }

  void reboot() { hardware::load(four_inch()); }

  json faults() { return json::parse(std::ifstream(state_ / "faults.json")); }

  fs::path state_;
};

int16_t axis(preflash::I2cDevice& chip, uint8_t reg) {
  chip.write(std::vector<uint8_t>{reg});
  uint8_t bytes[2];
  chip.read({&bytes[0], 1});
  chip.write(std::vector<uint8_t>{static_cast<uint8_t>(reg + 1)});
  chip.read({&bytes[1], 1});
  return static_cast<int16_t>(bytes[0] | bytes[1] << 8);
}

uint8_t reg(preflash::I2cDevice& chip, uint8_t address) {
  chip.write(std::vector<uint8_t>{address});
  uint8_t value = 0;
  chip.read({&value, 1});
  return value;
}

void put(preflash::I2cDevice& chip, uint8_t address, uint8_t value) {
  chip.write(std::vector<uint8_t>{address, value});
}

void accel_on(Qmi8658& imu) {
  put(imu, 0x02, 0x00);  // little-endian
  put(imu, 0x08, 0x01);
}

}  // namespace

TEST_F(Hardware, StartsHealthyWithTheMockupsWords) {
  EXPECT_EQ(item("rotation")["value"], "0");
  EXPECT_EQ(item("usb")["options"][0]["label"], "plugged in");
  EXPECT_EQ(item("battery")["options"][0]["label"], "not fitted");
  EXPECT_EQ(item("wifi")["options"][0]["label"], "Mac's connection");
  EXPECT_EQ(item("internet")["options"][0]["label"], "real");
  EXPECT_EQ(item("rtc")["options"][1]["label"], "flat backup cell");
  EXPECT_EQ(item("i2c")["value"], "healthy");
  EXPECT_EQ(item("motion")["value"], "healthy");
  EXPECT_EQ(item("wifi_join")["options"][0]["label"], "succeeds");
  EXPECT_EQ(item("rtc")["group"], "Break something");
  EXPECT_EQ(item("internet")["group"], "Network");
}

TEST_F(Hardware, ABoardShowsOnlyWhatItsChipsCanDo) {
  Board board{};
  board.i2c = {{"pcf85063", kRtc, 0}, {"axs15231b", 0x3b, 1}};
  hardware::load(board);
  EXPECT_TRUE(item("rotation").is_null());
  EXPECT_TRUE(item("usb").is_null());
  EXPECT_TRUE(item("battery").is_null());
  EXPECT_TRUE(item("motion").is_null());
  EXPECT_FALSE(item("rtc").is_null());
  bool touch = false;
  const json i2c = item("i2c");
  for (const json& option : i2c["options"]) touch = touch || option["label"] == "NACK AXS15231B";
  EXPECT_TRUE(touch);
}

TEST_F(Hardware, UnknownNamesAndValuesSaySo) {
  EXPECT_EQ(hardware::set("toaster", "on"), "no hardware called toaster");
  EXPECT_EQ(hardware::set("rtc", "soggy"), "rtc takes healthy or flat");
  EXPECT_EQ(hardware::set("wifi", "other"), "wifi takes mac");
}

TEST_F(Hardware, RotationTurnsGravityLive) {
  Qmi8658 imu;
  accel_on(imu);
  EXPECT_EQ(axis(imu, 0x37), 16384);
  EXPECT_EQ(hardware::set("rotation", "90"), "");
  EXPECT_EQ(axis(imu, 0x35), 16384);  // the right edge is down
  EXPECT_EQ(axis(imu, 0x37), 0);
  EXPECT_EQ(hardware::set("rotation", "180"), "");
  EXPECT_EQ(axis(imu, 0x35), 0);
  EXPECT_EQ(axis(imu, 0x37), -16384);
  EXPECT_EQ(hardware::set("rotation", "270"), "");
  EXPECT_EQ(axis(imu, 0x35), -16384);
  EXPECT_EQ(item("rotation")["applies"], "now");
}

TEST_F(Hardware, RotationKeepsToTheSensorsFullScale) {
  Qmi8658 imu;
  accel_on(imu);
  put(imu, 0x03, 0x10);  // ±4 g
  hardware::set("rotation", "180");
  EXPECT_EQ(axis(imu, 0x37), -8192);
}

TEST_F(Hardware, ASensorSwitchedOffKeepsItsLastSample) {
  Qmi8658 imu;
  hardware::set("rotation", "90");
  EXPECT_EQ(axis(imu, 0x35), 0);
}

TEST_F(Hardware, BatteryAndUsbReachThePowerChip) {
  Axp2101 pmu;
  EXPECT_EQ(reg(pmu, 0x00), 0x20);
  EXPECT_EQ(reg(pmu, 0x01), 0x15);
  EXPECT_EQ(reg(pmu, 0xa4), 0);
  EXPECT_EQ(hardware::set("battery", "50"), "");
  EXPECT_EQ(reg(pmu, 0x00), 0x28);  // VBUS good, battery present
  EXPECT_EQ(reg(pmu, 0x01), 0x32);  // charging
  EXPECT_EQ(reg(pmu, 0xa4), 50);
  EXPECT_EQ(hardware::set("usb", "unplugged"), "");
  EXPECT_EQ(reg(pmu, 0x00), 0x08);
  EXPECT_EQ(reg(pmu, 0x01), 0x55);  // discharging
  EXPECT_EQ(hardware::set("battery", "100"), "");
  EXPECT_EQ(hardware::set("usb", "plugged"), "");
  EXPECT_EQ(reg(pmu, 0x01), 0x14);  // charge done
}

TEST_F(Hardware, NoPowerAtAllIsRefused) {
  EXPECT_NE(hardware::set("usb", "unplugged"), "");
  EXPECT_EQ(item("usb")["value"], "plugged");
  hardware::set("battery", "50");
  hardware::set("usb", "unplugged");
  EXPECT_NE(hardware::set("battery", "none"), "");
}

TEST(HardwareDeathTest, ThePowerChipSwitchesTheBoardOff) {
  EXPECT_EXIT(
      {
        Axp2101 pmu;
        put(pmu, 0x10, 0x31);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(Axp2101, TheCommonRegisterKeepsItsOtherBits) {
  Axp2101 pmu;
  put(pmu, 0x10, 0x30);
  EXPECT_EQ(reg(pmu, 0x10), 0x30);
}

TEST(Axp2101DeathTest, OtherCommonBitsAreUnmodelled) {
  Axp2101 pmu;
  EXPECT_DEATH(put(pmu, 0x10, 0x02), "axp2101 register 0x10");
}

TEST_F(Hardware, AFlatCellWaitsForTheNextBoot) {
  EXPECT_EQ(item("rtc")["applies"], "boot");
  EXPECT_EQ(hardware::set("rtc", "flat"), "");
  EXPECT_EQ(item("rtc")["value"], "healthy");
  EXPECT_EQ(item("rtc")["next"], "flat");
  EXPECT_EQ(faults()["rtc"], "flat");
  EXPECT_FALSE(hardware::rtc_flat());
  {
    Pcf85063 running([] { return std::time_t(1000000000); });
    EXPECT_EQ(reg(running, 0x04) & 0x80, 0);
  }
  reboot();
  EXPECT_EQ(item("rtc")["value"], "flat");
  EXPECT_TRUE(hardware::rtc_flat());
  Pcf85063 flat([] { return std::time_t(1000000000); });
  EXPECT_EQ(reg(flat, 0x04), 0x80);  // the flag, and no time to go with it
  EXPECT_EQ(reg(flat, 0x0a), 0x00);  // the year 2000
}

TEST_F(Hardware, ARestartCarriesAFlatCellToo) {
  Pcf85063 before([] { return std::time_t(1000000000); });
  hardware::set("rtc", "flat");
  before.save_for_restart();
  reboot();
  Pcf85063 after([] { return std::time_t(1000000000); });
  EXPECT_EQ(reg(after, 0x04) & 0x80, 0x80);
}

TEST_F(Hardware, ChangingYourMindBeforeTheBootCancelsIt) {
  hardware::set("rtc", "flat");
  hardware::set("rtc", "healthy");
  EXPECT_EQ(item("rtc")["next"], "healthy");
  reboot();
  EXPECT_FALSE(hardware::rtc_flat());
}

TEST_F(Hardware, LiveFailuresOutlastABootToo) {
  hardware::set("internet", "offline");
  EXPECT_FALSE(preflash::net_online());
  reboot();
  EXPECT_EQ(item("internet")["value"], "offline");
  EXPECT_FALSE(preflash::net_online());
  hardware::set("internet", "real");
  EXPECT_TRUE(preflash::net_online());
}

TEST_F(Hardware, ABadFileStartsHealthy) {
  std::ofstream(state_ / "faults.json") << "not json";
  reboot();
  EXPECT_EQ(item("rtc")["value"], "healthy");
}

TEST_F(Hardware, AValueTheBoardCantTakeIsLeftOut) {
  std::ofstream(state_ / "faults.json") << R"({"i2c": "tea", "rtc": "flat"})";
  reboot();
  EXPECT_EQ(item("i2c")["value"], "healthy");
  EXPECT_EQ(item("rtc")["value"], "flat");
}

TEST_F(Hardware, WithNoStateFolderOnlyLiveChangesWork) {
  unsetenv("PREFLASH_STATE");
  EXPECT_EQ(hardware::set("rotation", "90"), "");
  EXPECT_NE(hardware::set("rtc", "flat"), "");
  hardware::set("rotation", "0");
}

TEST_F(Hardware, AChangeThatCantBeSavedChangesNothing) {
  const fs::path file = state_ / "a file";
  std::ofstream(file) << "in the way";
  const std::string blocked = (file / "state").string();
  setenv("PREFLASH_STATE", blocked.c_str(), 1);
  EXPECT_NE(hardware::set("internet", "offline"), "");
  EXPECT_NE(hardware::set("rtc", "flat"), "");
  EXPECT_EQ(item("internet")["value"], "real");
  EXPECT_EQ(item("internet")["next"], "real");
  EXPECT_EQ(item("rtc")["next"], "healthy");
  EXPECT_TRUE(preflash::net_online());
}

TEST_F(Hardware, ForcedNacksCoverAChipOrAllAndSayTheyWereInjected) {
  EXPECT_FALSE(hardware::i2c_nacks(0, kRtc));
  EXPECT_EQ(hardware::set("i2c", "pcf85063"), "");
  EXPECT_EQ(item("i2c")["applies"], "now");
  EXPECT_TRUE(hardware::i2c_nacks(0, kRtc));
  EXPECT_FALSE(hardware::i2c_nacks(0, kMotion));
  EXPECT_FALSE(hardware::i2c_nacks(1, kRtc));
  EXPECT_EQ(hardware::set("i2c", "all"), "");
  EXPECT_TRUE(hardware::i2c_nacks(0, kMotion));
  EXPECT_TRUE(hardware::i2c_nacks(0, kRtc));
  hardware::set("i2c", "healthy");
  EXPECT_FALSE(hardware::i2c_nacks(0, kRtc));
}

TEST_F(Hardware, ABusTransferSeesTheFailure) {
  preflash::i2c_bus().add(kRtc, std::make_unique<Pcf85063>());
  const uint8_t address = 0x04;
  uint8_t data[1];
  ASSERT_TRUE(preflash::i2c_bus().transfer(kRtc, {&address, 1}, data));

  preflash::trace::Record row{};
  const int id = preflash::trace::subscribe([&row](const preflash::trace::Record& r) {
    if (std::string(r.data).starts_with("NACK")) row = r;
  });
  hardware::set("i2c", "all");
  EXPECT_FALSE(preflash::i2c_bus().transfer(kRtc, {&address, 1}, data));
  hardware::set("i2c", "healthy");
  EXPECT_TRUE(preflash::i2c_bus().transfer(kRtc, {&address, 1}, data));
  for (int tries = 0; tries < 200 && !row.injected; ++tries) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  preflash::trace::unsubscribe(id);
  EXPECT_TRUE(row.injected);
  EXPECT_STREQ(row.data, "NACK (injected)");
  EXPECT_STREQ(row.op, "read");
  preflash::i2c_bus().take(kRtc);
}

TEST_F(Hardware, AMissingMotionSensorWaitsForTheBootAndThenNacks) {
  EXPECT_EQ(item("motion")["applies"], "boot");
  hardware::set("motion", "gone");
  EXPECT_FALSE(hardware::i2c_nacks(0, kMotion));
  reboot();
  EXPECT_TRUE(hardware::i2c_nacks(0, kMotion));
  EXPECT_FALSE(hardware::i2c_nacks(0, kRtc));
  // A healthy bus setting doesn't bring it back.
  hardware::set("i2c", "pcf85063");
  hardware::set("i2c", "healthy");
  EXPECT_TRUE(hardware::i2c_nacks(0, kMotion));
}

TEST_F(Hardware, TheBootNoticeNamesWhatIsForced) {
  EXPECT_EQ(hardware::forced_summary(), "");
  hardware::set("rtc", "flat");
  hardware::set("internet", "offline");
  reboot();
  const std::string summary = hardware::forced_summary();
  EXPECT_NE(summary.find("flat backup cell"), std::string::npos) << summary;
  EXPECT_NE(summary.find("offline"), std::string::npos) << summary;
}
