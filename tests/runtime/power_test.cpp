#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "esp_system.h"
#include "preflash/board.h"
#include "preflash/buttons.h"
#include "preflash/power.h"

namespace buttons = preflash::buttons;
namespace power = preflash::power;
using power::Mode;
using power::Reset;

namespace {

preflash::Board board_with_buttons() {
  return preflash::parse_board(R"({
    "name": "T", "display": {"width": 16, "height": 8}, "i2c": [],
    "backlight": {"pin": 4, "active_low": true}, "audio_codec": "none",
    "buttons": [{"name": "PWR", "kind": "axp2101"},
                {"name": "BOOT", "kind": "gpio", "pin": 0, "active_low": true},
                {"name": "RST", "kind": "reset"},
                {"name": "KEY", "kind": "gpio", "pin": 5, "active_low": true}]})");
}

// Puts each variable back as it was, however the test ends.
class Power : public testing::Test {
 protected:
  void SetUp() override {
    for (const char* name : kNames) {
      const char* value = std::getenv(name);
      saved_.push_back(value ? std::optional<std::string>(value) : std::nullopt);
      unsetenv(name);
    }
    buttons::load(board_with_buttons());
  }
  void TearDown() override {
    for (size_t i = 0; i < std::size(kNames); ++i) {
      if (saved_[i]) {
        setenv(kNames[i], saved_[i]->c_str(), 1);
      } else {
        unsetenv(kNames[i]);
      }
    }
    unsetenv("PREFLASH_HELD");
    buttons::load(board_with_buttons());
    buttons::load(preflash::Board{});
  }

  static constexpr const char* kNames[] = {"PREFLASH_RESET", "PREFLASH_POWER", "PREFLASH_HELD",
                                           "PREFLASH_RESTARTED"};
  std::vector<std::optional<std::string>> saved_;
};

bool has(const std::vector<std::string>& env, const std::string& entry) {
  return std::find(env.begin(), env.end(), entry) != env.end();
}

bool names(const std::vector<std::string>& env, const std::string& name) {
  return std::any_of(env.begin(), env.end(),
                     [&name](const std::string& e) { return e.rfind(name + "=", 0) == 0; });
}

}  // namespace

TEST_F(Power, ResetReasonFollowsTheEnvironment) {
  EXPECT_EQ(esp_reset_reason(), ESP_RST_POWERON);
  setenv("PREFLASH_RESET", "sw", 1);
  EXPECT_EQ(esp_reset_reason(), ESP_RST_SW);
  setenv("PREFLASH_RESET", "poweron", 1);
  EXPECT_EQ(esp_reset_reason(), ESP_RST_POWERON);
  // What older runs set means nothing now.
  unsetenv("PREFLASH_RESET");
  setenv("PREFLASH_RESTARTED", "1", 1);
  EXPECT_EQ(esp_reset_reason(), ESP_RST_POWERON);
}

TEST_F(Power, ModeFollowsTheEnvironment) {
  EXPECT_EQ(power::mode(), Mode::On);
  setenv("PREFLASH_POWER", "off", 1);
  EXPECT_EQ(power::mode(), Mode::Off);
  setenv("PREFLASH_POWER", "download", 1);
  EXPECT_EQ(power::mode(), Mode::Download);
  setenv("PREFLASH_POWER", "on", 1);
  EXPECT_EQ(power::mode(), Mode::On);
  setenv("PREFLASH_POWER", "sideways", 1);
  EXPECT_EQ(power::mode(), Mode::On);
  EXPECT_STREQ(power::name(Mode::On), "on");
  EXPECT_STREQ(power::name(Mode::Off), "off");
  EXPECT_STREQ(power::name(Mode::Download), "download");
}

TEST_F(Power, EnterCarriesHeldButtons) {
  ASSERT_EQ(buttons::set("BOOT", true), "");
  setenv("PREFLASH_RESTARTED", "1", 1);
  setenv("PREFLASH_POWER", "on", 1);
  const std::vector<std::string> env = power::env_for(Mode::Download, Reset::PowerOn);
  EXPECT_TRUE(has(env, "PREFLASH_HELD=BOOT:script"));
  EXPECT_TRUE(has(env, "PREFLASH_POWER=download"));
  EXPECT_TRUE(has(env, "PREFLASH_RESET=poweron"));
  EXPECT_FALSE(names(env, "PREFLASH_RESTARTED"));
  // One of each, and the rest of the environment comes along.
  EXPECT_EQ(std::count_if(env.begin(), env.end(),
                          [](const std::string& e) { return e.rfind("PREFLASH_POWER=", 0) == 0; }),
            1);
  if (const char* path = std::getenv("PATH")) EXPECT_TRUE(has(env, std::string("PATH=") + path));
}

TEST_F(Power, NothingHeldLeavesHeldOut) {
  setenv("PREFLASH_HELD", "BOOT:script", 1);
  const std::vector<std::string> env = power::env_for(Mode::On, Reset::Software);
  EXPECT_FALSE(names(env, "PREFLASH_HELD"));
  EXPECT_TRUE(has(env, "PREFLASH_RESET=sw"));
  EXPECT_TRUE(has(env, "PREFLASH_POWER=on"));
}

TEST_F(Power, ReleasingRstBootsOrEntersDownloadWithBootDown) {
  const preflash::Board board = board_with_buttons();
  EXPECT_EQ(power::after_release(board, "RST"), Mode::On);
  ASSERT_EQ(buttons::set("BOOT", true), "");
  EXPECT_EQ(power::after_release(board, "RST"), Mode::Download);
  ASSERT_EQ(buttons::set("BOOT", false), "");
  ASSERT_EQ(buttons::set("KEY", true), "");
  EXPECT_EQ(power::after_release(board, "RST"), Mode::On);
  // From download mode, as from on.
  setenv("PREFLASH_POWER", "download", 1);
  EXPECT_EQ(power::after_release(board, "RST"), Mode::On);
}

TEST_F(Power, OnlyRstResets) {
  const preflash::Board board = board_with_buttons();
  EXPECT_EQ(power::after_release(board, "BOOT"), std::nullopt);
  EXPECT_EQ(power::after_release(board, "PWR"), std::nullopt);
  EXPECT_EQ(power::after_release(board, "nothing"), std::nullopt);
}

TEST_F(Power, RstDoesNothingWhileOff) {
  setenv("PREFLASH_POWER", "off", 1);
  EXPECT_EQ(power::after_release(board_with_buttons(), "RST"), std::nullopt);
}
