#include <gtest/gtest.h>

#include <string>

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
