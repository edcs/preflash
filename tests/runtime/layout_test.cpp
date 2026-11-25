#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "window/layout.h"

namespace {

using namespace preflash::window;

TEST(Layout, TheNarrowBoardGetsTheMinimumWidth) {
  const Layout l = layout_for(172, 640, 0, 3, 1);
  EXPECT_EQ(l.window_w, 250);
  EXPECT_EQ(l.panel.w, 172);
  EXPECT_EQ(l.panel.x, 39);
  EXPECT_EQ(l.panel.y, 40);
  EXPECT_EQ(l.buttons.size(), 3u);
}

TEST(Layout, TurningSwapsThePanelsShape) {
  const Layout l = layout_for(172, 640, 90, 3, 1);
  EXPECT_EQ(l.panel.w, 640);
  EXPECT_EQ(l.panel.h, 172);
  EXPECT_EQ(l.window_w, 640);
  EXPECT_EQ(l.window_h, 40 + 172 + 8 + 16);
}

TEST(Layout, RotateSitsAtTheRightOfTheStrip) {
  for (int rotation : {0, 90}) {
    const Layout l = layout_for(172, 640, rotation, 3, 1);
    EXPECT_EQ(l.rotate.x + l.rotate.w, l.window_w - 10);
    EXPECT_EQ(l.rotate.h, 28);
    EXPECT_EQ(l.rotate.y, (40 - 28) / 2);
  }
}

TEST(Layout, ButtonsRunFromTheLeftInOrder) {
  const Layout l = layout_for(480, 480, 0, 3, 1);
  ASSERT_EQ(l.buttons.size(), 3u);
  EXPECT_EQ(l.buttons[0].x, 10);
  for (size_t i = 1; i < l.buttons.size(); ++i) {
    EXPECT_EQ(l.buttons[i].x, l.buttons[i - 1].x + l.buttons[i - 1].w + 6);
    EXPECT_EQ(l.buttons[i].h, 28);
  }
}

TEST(Layout, AStripOfManyButtonsWidensTheWindow) {
  const Layout l = layout_for(172, 640, 0, 8, 1);
  EXPECT_LE(l.buttons.back().x + l.buttons.back().w + 6, l.rotate.x);
  EXPECT_EQ(l.panel.x, (l.window_w - 172) / 2);
}

TEST(Layout, ReshapesForANewBoard) {
  const Layout first = layout_for(480, 480, 0, 3, 1);
  EXPECT_EQ(first.window_h, 40 + 480 + 8 + 16);
  const Layout l = layout_for(172, 640, 0, 2, 2);
  EXPECT_EQ(l.window_h, 40 + 640 + 8 + 32);
  EXPECT_EQ(l.status.y, 40 + 640);
  EXPECT_EQ(l.status.h, 8 + 32);
  EXPECT_EQ(l.status.w, l.window_w);
  EXPECT_EQ(l.buttons.size(), 2u);
}

// Where panel pixel (x, y) of a w x h panel shows in a picture turned clockwise.
std::pair<int, int> shown_at(int x, int y, int w, int h, int rotation) {
  switch (rotation) {
    case 90: return {h - 1 - y, x};
    case 180: return {w - 1 - x, h - 1 - y};
    case 270: return {y, w - 1 - x};
    default: return {x, y};
  }
}

TEST(Layout, AClickMapsThroughEachTurn) {
  for (int rotation : {0, 90, 180, 270}) {
    const Layout l = layout_for(172, 640, rotation, 3, 1);
    const auto [sx, sy] = shown_at(10, 20, 172, 640, rotation);
    const auto got = to_panel(l, rotation, 172, 640, l.panel.x + sx, l.panel.y + sy);
    ASSERT_TRUE(got.has_value()) << rotation;
    EXPECT_EQ(got->first, 10) << rotation;
    EXPECT_EQ(got->second, 20) << rotation;
  }
}

TEST(Layout, AClickOffThePanelMapsToNothing) {
  const Layout l = layout_for(172, 640, 0, 3, 1);
  EXPECT_FALSE(to_panel(l, 0, 172, 640, 100, 20).has_value());       // the strip
  EXPECT_FALSE(to_panel(l, 0, 172, 640, 5, 300).has_value());         // black beside the panel
  EXPECT_FALSE(to_panel(l, 0, 172, 640, 100, 40 + 640).has_value());  // the status bar
  EXPECT_TRUE(to_panel(l, 0, 172, 640, 39, 40).has_value());
}

TEST(Turn, AQuarterTurnMovesTheTopLeftToTheTopRight) {
  const std::vector<uint8_t> in = {0xA1, 0xA2, 0xB1, 0xB2};  // [A, B], two bytes a pixel
  std::vector<uint8_t> out;
  turn_rgb565(in.data(), 2, 1, 90, &out);
  EXPECT_EQ(out, (std::vector<uint8_t>{0xA1, 0xA2, 0xB1, 0xB2}));  // [A; B]
  turn_rgb565(in.data(), 2, 1, 180, &out);
  EXPECT_EQ(out, (std::vector<uint8_t>{0xB1, 0xB2, 0xA1, 0xA2}));  // [B, A]
  turn_rgb565(in.data(), 2, 1, 0, &out);
  EXPECT_EQ(out, in);
}

TEST(Turn, AQuarterTurnOfASquare) {
  // 2x2 [A B; C D] clockwise is [C A; D B], and anticlockwise [B D; A C].
  const std::vector<uint8_t> in = {'A', 0, 'B', 0, 'C', 0, 'D', 0};
  std::vector<uint8_t> out;
  turn_rgb565(in.data(), 2, 2, 90, &out);
  EXPECT_EQ(out, (std::vector<uint8_t>{'C', 0, 'A', 0, 'D', 0, 'B', 0}));
  turn_rgb565(in.data(), 2, 2, 270, &out);
  EXPECT_EQ(out, (std::vector<uint8_t>{'B', 0, 'D', 0, 'A', 0, 'C', 0}));
}

TEST(StatusLines, TheLongBoardNameWrapsAt250) {
  EXPECT_EQ(status_lines_for("ESP32-S3-Touch-LCD-3.49-V2", "0°", 250), 2);
  EXPECT_EQ(status_lines_for("ESP32-S3-Touch-LCD-4B", "0°", 480), 1);
}

TEST(StatusLines, AWordTooLongForALineIsBroken) {
  // 222 pixels hold 27 glyphs, so 60 with no space take three lines.
  EXPECT_EQ(status_lines_for(std::string(60, 'x'), "", 250), 3);
}

TEST(StatusLines, WrapKeepsWholeWords) {
  const auto lines = wrap_words("aaaa bbbb cccc", 9 * 8 + 28);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].start, 0u);
  EXPECT_EQ(lines[0].length, 9u);  // "aaaa bbbb"
  EXPECT_EQ(lines[1].start, 10u);
  EXPECT_EQ(lines[1].length, 4u);  // "cccc"
}

TEST(StatusLines, WrapCountsEverySpaceKept) {
  // A line keeps the whole gap between its words, so two spaces take two glyphs.
  const auto lines = wrap_words("aaaa  bbbb", 9 * 8 + 28);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].length, 4u);  // "aaaa"
  EXPECT_EQ(lines[1].start, 6u);
  EXPECT_EQ(lines[1].length, 4u);  // "bbbb"
}

}  // namespace
