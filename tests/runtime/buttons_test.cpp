#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "preflash/board.h"
#include "preflash/buttons.h"
#include "preflash/bus.h"
#include "preflash/chips.h"

namespace buttons = preflash::buttons;
using preflash::gpio_read;

namespace {

// PWR on the AXP2101, BOOT on GPIO0, RST on EN, and a key on the expander's bit 3.
preflash::Board board_with_buttons() {
  return preflash::parse_board(R"({
    "name": "T", "display": {"width": 16, "height": 8}, "i2c": [],
    "backlight": {"pin": 4, "active_low": true}, "audio_codec": "none",
    "buttons": [{"name": "PWR", "kind": "axp2101"},
                {"name": "BOOT", "kind": "gpio", "pin": 0, "active_low": true},
                {"name": "RST", "kind": "reset"},
                {"name": "KEY", "kind": "expander", "pin": 3, "active_low": true}]})");
}

uint8_t input_register(preflash::Tca9554& chip) {
  const std::array<uint8_t, 1> start = {0x00};
  EXPECT_TRUE(chip.write(start));
  std::array<uint8_t, 1> out{};
  EXPECT_TRUE(chip.read(out));
  return out[0];
}

class Buttons : public testing::Test {
 protected:
  void SetUp() override {
    unsetenv("PREFLASH_HELD");
    buttons::load(board_with_buttons());
  }
  // An empty board lets go of nothing, but leaves the button list empty: what this suite
  // pressed was released above, so neighbours find the pins at rest.
  void TearDown() override {
    unsetenv("PREFLASH_HELD");
    buttons::load(board_with_buttons());
    buttons::load(preflash::Board{});
  }
};

}  // namespace

TEST_F(Buttons, APressDrivesAnActiveLowPinLow) {
  EXPECT_TRUE(gpio_read(0));  // released at boot
  EXPECT_EQ(buttons::set("BOOT", true), "");
  EXPECT_FALSE(gpio_read(0));
  EXPECT_TRUE(buttons::down("BOOT"));
  EXPECT_EQ(buttons::held(), std::vector<std::string>{"BOOT"});
  EXPECT_EQ(buttons::held_env(), "BOOT:script");
  EXPECT_EQ(buttons::set("BOOT", false), "");
  EXPECT_TRUE(gpio_read(0));
  EXPECT_FALSE(buttons::down("BOOT"));
  EXPECT_EQ(buttons::held_env(), "");
}

TEST_F(Buttons, ATca9554ButtonDrivesItsInputBit) {
  preflash::Tca9554 expander;
  EXPECT_EQ(input_register(expander) & 0x08, 0x08);
  ASSERT_EQ(buttons::set("KEY", true), "");
  EXPECT_EQ(input_register(expander) & 0x08, 0x00);
  EXPECT_EQ(input_register(expander) | 0x08, 0xff);  // the other inputs stay pulled high
  EXPECT_FALSE(preflash::expander_level(3));
  ASSERT_EQ(buttons::set("KEY", false), "");
  EXPECT_EQ(input_register(expander), 0xff);
}

TEST_F(Buttons, HeldButtonsComeBackFromTheEnvironment) {
  setenv("PREFLASH_HELD", "PWR:window,BOOT", 1);  // a bare name is a script's
  buttons::load(board_with_buttons());
  EXPECT_TRUE(buttons::down("PWR"));
  EXPECT_TRUE(buttons::down("BOOT"));
  EXPECT_FALSE(gpio_read(0));  // held through the restart, so low from the start
  EXPECT_FALSE(buttons::down("RST"));
}

TEST_F(Buttons, LoadingABoardDropsHeldButtonsItLacks) {
  setenv("PREFLASH_HELD", "PWR,FOO", 1);
  buttons::load(board_with_buttons());
  EXPECT_EQ(buttons::held(), std::vector<std::string>{"PWR"});
}

TEST_F(Buttons, AnUnknownNameListsTheBoardsButtons) {
  EXPECT_EQ(buttons::set("FOO", true), "no button named FOO: the board has PWR, BOOT, RST, KEY");
  EXPECT_TRUE(buttons::held().empty());
}

TEST_F(Buttons, PwrAndRstOnlyChangeWhatsHeldAndTellListeners) {
  // Listeners stay for the life of the process, so this one owns what it fills.
  auto heard = std::make_shared<std::vector<std::pair<std::string, bool>>>();
  buttons::on_change([heard](const std::string& name, bool down) { heard->emplace_back(name, down); });
  EXPECT_EQ(buttons::set("RST", true), "");
  EXPECT_EQ(buttons::set("RST", true), "");  // already down: nothing new
  EXPECT_EQ(buttons::set("PWR", true), "");
  EXPECT_EQ(buttons::set("RST", false), "");
  EXPECT_EQ(buttons::held(), std::vector<std::string>{"PWR"});
  const std::vector<std::pair<std::string, bool>> want = {
      {"RST", true}, {"PWR", true}, {"RST", false}};
  EXPECT_EQ(*heard, want);
}

TEST_F(Buttons, DescribeListsEachButtonInBoardOrder) {
  ASSERT_EQ(buttons::set("BOOT", true), "");
  const nlohmann::json want = nlohmann::json::array({
      {{"name", "PWR"}, {"wiring", "AXP2101 PWRON"}, {"down", false}},
      {{"name", "BOOT"}, {"wiring", "GPIO0, active low"}, {"down", true}},
      {{"name", "RST"}, {"wiring", "EN"}, {"down", false}},
      {{"name", "KEY"}, {"wiring", "TCA9554 bit 3, active low"}, {"down", false}},
  });
  EXPECT_EQ(buttons::describe(), want);
}

using buttons::Owner;

TEST_F(Buttons, AButtonHeldByBothOwnersStaysDownUntilBothLetGo) {
  auto edges = std::make_shared<int>(0);
  buttons::on_change([edges](const std::string&, bool) { ++*edges; });
  ASSERT_EQ(buttons::set("BOOT", true, Owner::Window), "");
  ASSERT_EQ(buttons::set("BOOT", true, Owner::Script), "");
  EXPECT_EQ(*edges, 1);  // one edge: the button only went down once
  ASSERT_EQ(buttons::set("BOOT", false, Owner::Window), "");
  EXPECT_TRUE(buttons::down("BOOT"));
  EXPECT_FALSE(gpio_read(0));
  ASSERT_EQ(buttons::set("BOOT", false, Owner::Script), "");
  EXPECT_FALSE(buttons::down("BOOT"));
  EXPECT_TRUE(gpio_read(0));
  EXPECT_EQ(*edges, 2);
  EXPECT_EQ(buttons::held(), std::vector<std::string>{});
}

TEST_F(Buttons, BothOwnersComeBackThroughAnExec) {
  ASSERT_EQ(buttons::set("BOOT", true, Owner::Window), "");
  ASSERT_EQ(buttons::set("PWR", true, Owner::Script), "");
  ASSERT_EQ(buttons::set("KEY", true, Owner::Window), "");
  ASSERT_EQ(buttons::set("KEY", true, Owner::Script), "");
  const std::string env = buttons::held_env();
  EXPECT_EQ(env, "BOOT:window,PWR:script,KEY:window,KEY:script");
  setenv("PREFLASH_HELD", env.c_str(), 1);
  buttons::load(board_with_buttons());
  EXPECT_EQ(buttons::held_by(Owner::Window), (std::vector<std::string>{"BOOT", "KEY"}));
  EXPECT_EQ(buttons::held_by(Owner::Script), (std::vector<std::string>{"PWR", "KEY"}));
  EXPECT_EQ(buttons::held(), (std::vector<std::string>{"BOOT", "PWR", "KEY"}));
  EXPECT_FALSE(gpio_read(0));
}

// A reset in progress takes RST's press with it, whoever held it, so the new run starts with
// RST up and BOOT still down.
TEST_F(Buttons, RstHeldThroughAnExecComesBackUp) {
  ASSERT_EQ(buttons::set("BOOT", true, Owner::Script), "");
  ASSERT_EQ(buttons::set("RST", true, Owner::Script), "");
  ASSERT_EQ(buttons::set("RST", true, Owner::Window), "");
  setenv("PREFLASH_HELD", buttons::held_env().c_str(), 1);
  buttons::load(board_with_buttons());
  EXPECT_FALSE(buttons::down("RST"));
  EXPECT_TRUE(buttons::held_by(Owner::Window).empty());
  EXPECT_EQ(buttons::held(), std::vector<std::string>{"BOOT"});
}

namespace {

int64_t realtime_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Waits up to a second for the button to come up.
bool comes_up(const std::string& name) {
  for (int i = 0; i < 100; ++i) {
    if (!buttons::down(name)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

// A tap's release that an exec cuts off still comes, in the next run, when the tap would have
// let go: the press carries its deadline across.
TEST_F(Buttons, ATapCutOffByAnExecLetsGoInTheNextRun) {
  const int64_t deadline = realtime_ms() + 300;
  ASSERT_EQ(buttons::press_until("BOOT", Owner::Script, deadline), "");
  ASSERT_EQ(buttons::set("KEY", true, Owner::Window), "");
  const std::string env = buttons::held_env();
  EXPECT_EQ(env, "BOOT:script@" + std::to_string(deadline) + ",KEY:window");
  setenv("PREFLASH_HELD", env.c_str(), 1);
  buttons::load(board_with_buttons());
  EXPECT_TRUE(buttons::down("BOOT"));
  EXPECT_FALSE(gpio_read(0));
  EXPECT_TRUE(comes_up("BOOT"));
  EXPECT_GE(realtime_ms(), deadline);
  EXPECT_TRUE(gpio_read(0));
  EXPECT_TRUE(buttons::down("KEY"));  // a hold with no deadline stays
}

TEST_F(Buttons, ADeadlineAlreadyPastLetsGoAsTheRunStarts) {
  setenv("PREFLASH_HELD", "BOOT:script@1", 1);
  buttons::load(board_with_buttons());
  EXPECT_TRUE(comes_up("BOOT"));
}

// A release that came in time leaves nothing for the next run, and a load in between stops a
// release that was waiting.
TEST_F(Buttons, ATapsOwnReleaseEndsItsDeadline) {
  ASSERT_EQ(buttons::press_until("BOOT", Owner::Script, realtime_ms() + 60000), "");
  ASSERT_EQ(buttons::set("BOOT", false, Owner::Script), "");
  EXPECT_EQ(buttons::held_env(), "");
  setenv("PREFLASH_HELD", ("BOOT:script@" + std::to_string(realtime_ms() + 200)).c_str(), 1);
  buttons::load(board_with_buttons());
  unsetenv("PREFLASH_HELD");
  buttons::load(board_with_buttons());
  ASSERT_EQ(buttons::set("BOOT", true, Owner::Script), "");
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  EXPECT_TRUE(buttons::down("BOOT"));
}

TEST_F(Buttons, ATapPressesAndLetsGo) {
  ASSERT_EQ(buttons::tap("BOOT", 20, Owner::Script), "");
  EXPECT_FALSE(buttons::down("BOOT"));
  EXPECT_NE(buttons::tap("NOPE", 20, Owner::Script), "");
}
