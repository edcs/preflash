#include <gtest/gtest.h>

#include <string>

#include "bsp/display.h"
#include "preflash/board.h"

namespace {

const char* kBoard = R"({
  "name": "Test",
  "display": {"width": 480, "height": 320},
  "i2c": [{"chip": "pcf85063", "address": 81}, {"chip": "qmi8658", "address": 107}],
  "backlight": {"pin": 4, "active_low": true, "enable_pin": 43},
  "audio_codec": "es8311"
})";

}  // namespace

TEST(Board, ParsesResolvedJson) {
  preflash::Board b = preflash::parse_board(kBoard);
  EXPECT_EQ(b.name, "Test");
  EXPECT_EQ(b.width, 480);
  EXPECT_EQ(b.height, 320);
  ASSERT_EQ(b.i2c.size(), 2u);
  EXPECT_EQ(b.i2c[0].chip, "pcf85063");
  EXPECT_EQ(b.i2c[0].address, 0x51);
  EXPECT_EQ(b.i2c[1].address, 0x6b);
  EXPECT_EQ(b.backlight.pin, 4);
  EXPECT_TRUE(b.backlight.active_low);
  ASSERT_TRUE(b.backlight.enable_pin.has_value());
  EXPECT_EQ(*b.backlight.enable_pin, 43);
  EXPECT_EQ(b.audio_codec, "es8311");
}

TEST(Board, EnablePinIsOptional) {
  std::string json = kBoard;
  json.replace(json.find(", \"enable_pin\": 43"), 18, "");
  EXPECT_FALSE(preflash::parse_board(json).backlight.enable_pin.has_value());
}

TEST(Board, BadJsonNamesTheField) {
  std::string json = kBoard;
  json.replace(json.find("\"width\": 480, "), 14, "");
  try {
    preflash::parse_board(json);
    FAIL() << "expected a throw";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("display.width"), std::string::npos) << e.what();
  }
}

TEST(Board, UnparsableJsonThrows) {
  EXPECT_THROW(preflash::parse_board("{nope"), std::runtime_error);
}

TEST(Board, NonObjectRootNamesTheRoot) {
  try {
    preflash::parse_board("[]");
    FAIL() << "expected a throw";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("board: root must be an object"), std::string::npos) << e.what();
  }
}

namespace {

std::string board_error(std::string json) {
  try {
    preflash::parse_board(json);
  } catch (const std::runtime_error& e) {
    return e.what();
  }
  return "";
}

std::string with_replaced(const std::string& from, const std::string& to) {
  std::string json = kBoard;
  json.replace(json.find(from), from.size(), to);
  return json;
}

}  // namespace

TEST(Board, IntegersBeyondIntAreRejectedByName) {
  EXPECT_NE(board_error(with_replaced("\"width\": 480", "\"width\": 4294967296")).find("display.width"),
            std::string::npos);
  EXPECT_NE(board_error(with_replaced("\"width\": 480", "\"width\": 2147483648")).find("out of range"),
            std::string::npos);
  EXPECT_NE(board_error(with_replaced("\"pin\": 4", "\"pin\": -2147483649")).find("backlight.pin"),
            std::string::npos);
  EXPECT_NE(board_error(with_replaced("\"address\": 81", "\"address\": 18446744073709551615")).find("i2c[0].address"),
            std::string::npos);
  EXPECT_EQ(board_error(with_replaced("\"width\": 480", "\"width\": 2147483647")), "");
}

TEST(Board, DisplaySizeMustBePositive) {
  EXPECT_NE(board_error(with_replaced("\"width\": 480", "\"width\": 0")).find("display.width must be positive"),
            std::string::npos);
  EXPECT_NE(board_error(with_replaced("\"height\": 320", "\"height\": -5")).find("display.height must be positive"),
            std::string::npos);
}

TEST(Board, AChipIsOnBusZeroUnlessItSaysOtherwise) {
  std::string json = kBoard;
  json.replace(json.find("\"address\": 107"), 14, "\"address\": 107, \"bus\": 1");
  preflash::Board b = preflash::parse_board(json);
  EXPECT_EQ(b.i2c[0].bus, 0);
  EXPECT_EQ(b.i2c[1].bus, 1);
}

TEST(Board, ABusTheChipLacksNamesTheField) {
  std::string json = kBoard;
  json.replace(json.find("\"address\": 107"), 14, "\"address\": 107, \"bus\": 2");
  try {
    preflash::parse_board(json);
    FAIL() << "expected a throw";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("i2c[1].bus"), std::string::npos) << e.what();
  }
}

TEST(Board, ThePanelIsLittleEndianUnlessItSaysOtherwise) {
  EXPECT_FALSE(preflash::parse_board(kBoard).big_endian);
  std::string json = kBoard;
  json.replace(json.find("\"height\": 320"), 13, "\"height\": 320, \"byte_order\": \"be\"");
  EXPECT_TRUE(preflash::parse_board(json).big_endian);
}

TEST(Board, ABadByteOrderNamesTheField) {
  std::string json = kBoard;
  json.replace(json.find("\"height\": 320"), 13, "\"height\": 320, \"byte_order\": \"up\"");
  try {
    preflash::parse_board(json);
    FAIL() << "expected a throw";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("display.byte_order"), std::string::npos) << e.what();
  }
}

TEST(Board, ReadsTheController) {
  EXPECT_EQ(preflash::parse_board(kBoard).controller, "");
  std::string json = kBoard;
  json.replace(json.find("\"height\": 320"), 13, "\"height\": 320, \"controller\": \"axs15231b\"");
  EXPECT_EQ(preflash::parse_board(json).controller, "axs15231b");
}

TEST(Board, ExpanderBitsAreOptional) {
  preflash::Board b = preflash::parse_board(kBoard);
  EXPECT_FALSE(b.backlight.enable_expander_bit.has_value());
  EXPECT_FALSE(b.power_amp_bit.has_value());

  std::string json = kBoard;
  json.replace(json.find(", \"enable_pin\": 43"), 18, ", \"enable_expander_bit\": 1");
  json.replace(json.find("\"audio_codec\""), 13, "\"power_amp_bit\": 3, \"audio_codec\"");
  b = preflash::parse_board(json);
  EXPECT_EQ(b.backlight.enable_expander_bit, 1);
  EXPECT_EQ(b.power_amp_bit, 3);
}

TEST(Board, AnExpanderBitMustBeOnTheExpander) {
  std::string json = kBoard;
  json.replace(json.find(", \"enable_pin\": 43"), 18, ", \"enable_expander_bit\": 8");
  EXPECT_THROW(preflash::parse_board(json), std::runtime_error);
}

TEST(Board, BspResolutionComesFromTheBoard) {
  EXPECT_EQ(BSP_LCD_H_RES, preflash::board().width);
  EXPECT_EQ(BSP_LCD_V_RES, preflash::board().height);
}

namespace {

const char* kButtons = R"({
  "name": "Test",
  "display": {"width": 480, "height": 320},
  "i2c": [],
  "backlight": {"pin": 4, "active_low": true},
  "audio_codec": "none",
  "buttons": [
    {"name": "BOOT", "kind": "gpio", "pin": 0, "active_low": true},
    {"name": "X", "kind": "expander", "pin": 3, "active_low": false},
    {"name": "PWR", "kind": "axp2101"},
    {"name": "RST", "kind": "reset"},
    {"name": "P", "kind": "power", "pin": 16, "active_low": true, "hold_expander_bit": 6},
    {"name": "Q", "kind": "power", "pin": 17, "active_low": false, "hold_pin": 18}
  ]
})";

}  // namespace

TEST(Board, ParsesEachButtonKind) {
  using Kind = preflash::Button::Kind;
  const preflash::Board b = preflash::parse_board(kButtons);
  ASSERT_EQ(b.buttons.size(), 6u);
  EXPECT_EQ(b.buttons[0].name, "BOOT");
  EXPECT_EQ(b.buttons[0].kind, Kind::Gpio);
  EXPECT_EQ(b.buttons[0].pin, 0);
  EXPECT_TRUE(b.buttons[0].active_low);
  EXPECT_EQ(b.buttons[1].kind, Kind::Expander);
  EXPECT_EQ(b.buttons[1].pin, 3);
  EXPECT_FALSE(b.buttons[1].active_low);
  EXPECT_EQ(b.buttons[2].kind, Kind::Axp2101);
  EXPECT_EQ(b.buttons[3].kind, Kind::Reset);
  EXPECT_EQ(b.buttons[4].kind, Kind::Power);
  EXPECT_EQ(b.buttons[4].pin, 16);
  EXPECT_FALSE(b.buttons[4].hold_pin.has_value());
  ASSERT_TRUE(b.buttons[4].hold_expander_bit.has_value());
  EXPECT_EQ(*b.buttons[4].hold_expander_bit, 6);
  ASSERT_TRUE(b.buttons[5].hold_pin.has_value());
  EXPECT_EQ(*b.buttons[5].hold_pin, 18);
}

TEST(Board, AnUnknownButtonKindNamesIt) {
  std::string json = kButtons;
  json.replace(json.find("\"reset\""), 7, "\"nope\"");
  try {
    preflash::parse_board(json);
    FAIL() << "expected a throw";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("buttons[3].kind"), std::string::npos) << e.what();
  }
}

TEST(Board, NoButtonsIsAnEmptyList) {
  EXPECT_TRUE(preflash::parse_board(kBoard).buttons.empty());
}

TEST(Board, WiringSaysWhereEachButtonGoes) {
  const preflash::Board b = preflash::parse_board(kButtons);
  EXPECT_EQ(preflash::wiring(b.buttons[0]), "GPIO0, active low");
  EXPECT_EQ(preflash::wiring(b.buttons[1]), "TCA9554 bit 3, active high");
  EXPECT_EQ(preflash::wiring(b.buttons[2]), "AXP2101 PWRON");
  EXPECT_EQ(preflash::wiring(b.buttons[3]), "EN");
  EXPECT_EQ(preflash::wiring(b.buttons[4]), "GPIO16 key, TCA9554 bit 6 hold");
  EXPECT_EQ(preflash::wiring(b.buttons[5]), "GPIO17 key, GPIO18 hold");
}
