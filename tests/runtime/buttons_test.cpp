#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <memory>
#include <string>
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
  void TearDown() override {
    unsetenv("PREFLASH_HELD");
    buttons::load(board_with_buttons());
  }
};

}  // namespace

TEST_F(Buttons, APressDrivesAnActiveLowPinLow) {
  EXPECT_TRUE(gpio_read(0));  // released at boot
  EXPECT_EQ(buttons::set("BOOT", true), "");
  EXPECT_FALSE(gpio_read(0));
  EXPECT_TRUE(buttons::down("BOOT"));
  EXPECT_EQ(buttons::held(), std::vector<std::string>{"BOOT"});
  EXPECT_EQ(buttons::held_env(), "BOOT");
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
  setenv("PREFLASH_HELD", "PWR,BOOT", 1);
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
