#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "driver/gpio.h"
#include "esp_system.h"
#include "preflash/board.h"
#include "preflash/bus.h"
#include "preflash/buttons.h"
#include "preflash/chips.h"
#include "preflash/hardware.h"
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

// Starts with none of the power variables set, and puts each back as it was.
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
  // Lets go of what the test pressed, so neighbours find the pins at rest, and then puts the
  // environment back.
  void TearDown() override {
    unsetenv("PREFLASH_HELD");
    buttons::load(board_with_buttons());
    buttons::load(preflash::Board{});
    for (size_t i = 0; i < std::size(kNames); ++i) {
      if (saved_[i]) {
        setenv(kNames[i], saved_[i]->c_str(), 1);
      } else {
        unsetenv(kNames[i]);
      }
    }
  }

  static constexpr const char* kNames[] = {"PREFLASH_RESET", "PREFLASH_POWER", "PREFLASH_HELD",
                                           "PREFLASH_RESTARTED", "PREFLASH_EXPANDER",
                                           "PREFLASH_RTC_CARRY"};
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

namespace {

// The 3.49 V2's power key: SYS_OUT on GPIO16, active low, held on by TCA9554 bit 6.
preflash::Board power_key_board() {
  return preflash::parse_board(R"({
    "name": "T", "display": {"width": 16, "height": 8}, "i2c": [{"chip": "tca9554", "address": 32}],
    "backlight": {"pin": 4, "active_low": true}, "audio_codec": "none",
    "buttons": [{"name": "PWR", "kind": "power", "pin": 16, "active_low": true,
                 "hold_expander_bit": 6},
                {"name": "BOOT", "kind": "gpio", "pin": 0, "active_low": true}]})");
}

// The same key, held on by a GPIO instead.
preflash::Board gpio_hold_board() {
  return preflash::parse_board(R"({
    "name": "T", "display": {"width": 16, "height": 8}, "i2c": [],
    "backlight": {"pin": 4, "active_low": true}, "audio_codec": "none",
    "buttons": [{"name": "PWR", "kind": "power", "pin": 16, "active_low": true,
                 "hold_pin": 18}]})");
}

constexpr uint32_t kHoldBit = 1u << 6;

}  // namespace

TEST_F(Power, OnBatteryWithNoHoldTheBoardGoesOff) {
  EXPECT_EQ(power::decide(false, false, false), Mode::Off);
}

TEST_F(Power, OnUsbReleasingTheHoldKeepsItOn) {
  EXPECT_EQ(power::decide(true, false, false), Mode::On);
}

TEST_F(Power, AHeldKeyKeepsABatteryBoardOn) {
  EXPECT_EQ(power::decide(false, true, false), Mode::On);
}

TEST_F(Power, ALatchedHoldKeepsItOnAfterTheKeyComesUp) {
  EXPECT_EQ(power::decide(false, false, true), Mode::On);
}

TEST_F(Power, TheHoldCountsOnlyAsAnOutputDrivenHigh) {
  const preflash::Board board = power_key_board();
  preflash::expander_power_on();
  // At power-on every pin is an input reading high from its pull-up: not a hold.
  EXPECT_FALSE(power::holding(board));
  preflash::expander_set_outputs(kHoldBit, kHoldBit);
  preflash::expander_set_levels(kHoldBit, 0);
  EXPECT_FALSE(power::holding(board));
  preflash::expander_set_levels(kHoldBit, kHoldBit);
  EXPECT_TRUE(power::holding(board));
  preflash::expander_power_on();
  EXPECT_FALSE(power::holding(board));
  EXPECT_FALSE(power::holding(board_with_buttons()));
}

TEST_F(Power, AGpioHoldCountsAsAnOutputDrivenHigh) {
  const preflash::Board board = gpio_hold_board();
  EXPECT_FALSE(power::holding(board));
  ASSERT_EQ(gpio_set_direction(GPIO_NUM_18, GPIO_MODE_OUTPUT), ESP_OK);
  ASSERT_EQ(gpio_set_level(GPIO_NUM_18, 1), ESP_OK);
  EXPECT_TRUE(power::holding(board));
  ASSERT_EQ(gpio_set_level(GPIO_NUM_18, 0), ESP_OK);
  EXPECT_FALSE(power::holding(board));
  ASSERT_EQ(gpio_set_direction(GPIO_NUM_18, GPIO_MODE_INPUT), ESP_OK);
}

TEST_F(Power, ABatteryLaunchStartsAPowerKeyBoardOff) {
  const preflash::Board board = power_key_board();
  preflash::hardware::load(board);
  buttons::load(board);
  EXPECT_FALSE(power::starts_off(board));  // USB is plugged in by default
  ASSERT_EQ(preflash::hardware::set("battery", "50"), "");
  ASSERT_EQ(preflash::hardware::set("usb", "unplugged"), "");
  EXPECT_TRUE(power::starts_off(board));
  ASSERT_EQ(buttons::set("PWR", true), "");
  EXPECT_FALSE(power::starts_off(board));
  ASSERT_EQ(buttons::set("PWR", false), "");
  // A re-run isn't a launch: the run before it set the mode.
  setenv("PREFLASH_POWER", "on", 1);
  EXPECT_FALSE(power::starts_off(board));
  unsetenv("PREFLASH_POWER");
  // A board with no power key has nothing to start it.
  EXPECT_FALSE(power::starts_off(board_with_buttons()));
  preflash::hardware::load(preflash::Board{});
}

TEST_F(Power, TheExpanderKeepsItsPinsThroughAResetButNotPowerOff) {
  preflash::expander_power_on();
  preflash::expander_set_outputs(kHoldBit, kHoldBit);
  preflash::expander_set_levels(kHoldBit, 0);
  const std::string carried = "PREFLASH_EXPANDER=40,ffffffbf";
  EXPECT_TRUE(has(power::env_for(Mode::On, Reset::Software), carried));
  EXPECT_TRUE(has(power::env_for(Mode::On, Reset::PowerOn), carried));
  EXPECT_TRUE(has(power::env_for(Mode::Download, Reset::PowerOn), carried));
  EXPECT_FALSE(names(power::env_for(Mode::Off, Reset::PowerOn), "PREFLASH_EXPANDER"));
  // Off, nothing powered the chip, and download mode passes on what it was given.
  setenv("PREFLASH_POWER", "off", 1);
  setenv("PREFLASH_EXPANDER", "40,ffffffff", 1);
  EXPECT_FALSE(names(power::env_for(Mode::On, Reset::PowerOn), "PREFLASH_EXPANDER"));
  setenv("PREFLASH_POWER", "download", 1);
  EXPECT_TRUE(has(power::env_for(Mode::On, Reset::PowerOn), "PREFLASH_EXPANDER=40,ffffffff"));
  // The next run's chip starts from what was carried.
  preflash::expander_power_on();
  {
    preflash::Tca9554 chip;
    EXPECT_EQ(preflash::expander_pins().outputs, kHoldBit);
    EXPECT_EQ(preflash::expander_pins().levels, 0xffffffffu);
  }
  EXPECT_EQ(std::getenv("PREFLASH_EXPANDER"), nullptr);
  preflash::Tca9554 fresh;
  EXPECT_EQ(preflash::expander_pins().outputs, 0u);
}

TEST_F(Power, SwitchingGoesThroughTheTestsStandIn) {
  std::optional<std::pair<Mode, Reset>> seen;
  power::set_enter_for_test([&seen](Mode next, Reset reason) { seen = {next, reason}; });
  power::switch_to(Mode::Off, Reset::PowerOn);
  power::set_enter_for_test(nullptr);
  ASSERT_TRUE(seen.has_value());
  EXPECT_EQ(seen->first, Mode::Off);
  EXPECT_EQ(seen->second, Reset::PowerOn);
}

// The AXP2101 restarting the board in place of a power-off cycles the rails, so the expander
// starts afresh, though the chip sees a power-on reset as for RST.
TEST_F(Power, APowerCycleDropsTheExpandersPins) {
  preflash::expander_power_on();
  preflash::expander_set_outputs(kHoldBit, kHoldBit);
  const std::vector<std::string> env = power::env_for(Mode::On, Reset::PowerCycle);
  EXPECT_FALSE(names(env, "PREFLASH_EXPANDER"));
  EXPECT_TRUE(has(env, "PREFLASH_RESET=poweron"));
  EXPECT_TRUE(has(env, "PREFLASH_POWER=on"));
  preflash::expander_power_on();
}

// The RTC runs on its own cell, so its time passes through an off run untouched.
TEST_F(Power, TheRtcKeepsTimeWhileOff) {
  setenv("PREFLASH_POWER", "off", 1);
  setenv("PREFLASH_RTC_CARRY", "1762300000:0", 1);
  EXPECT_TRUE(has(power::env_for(Mode::On, Reset::PowerOn), "PREFLASH_RTC_CARRY=1762300000:0"));
}

// Off, a press of PWR boots the board as soon as the listener is in, with no circuit thread
// needed: main puts it in before the window's hello can press anything. The button has a
// name no other test presses, as listeners stay for the rest of the run.
TEST_F(Power, OffAPressBootsTheBoardOnceTheListenerIsIn) {
  const preflash::Board board = preflash::parse_board(R"({
    "name": "T", "display": {"width": 16, "height": 8}, "i2c": [],
    "backlight": {"pin": 4, "active_low": true}, "audio_codec": "none",
    "buttons": [{"name": "P9", "kind": "axp2101"}]})");
  buttons::load(board);
  setenv("PREFLASH_POWER", "off", 1);
  std::atomic<int> switched{0};
  std::atomic<Mode> to{Mode::Off};
  power::set_enter_for_test([&](Mode next, Reset) {
    to = next;
    ++switched;
  });
  power::listen_power(board);
  ASSERT_EQ(buttons::set("P9", true, buttons::Owner::Window), "");
  for (int ms = 0; ms < 2000 && switched == 0; ++ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  buttons::set("P9", false, buttons::Owner::Window);
  power::set_enter_for_test(nullptr);
  EXPECT_EQ(switched, 1);
  EXPECT_EQ(to, Mode::On);
}
