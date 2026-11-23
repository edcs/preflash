#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "window/pad.h"

namespace {

using preflash::window::Pad;
using Names = std::vector<std::string>;

TEST(Pad, PressAndRelease) {
  Pad pad;
  pad.press("PWR");
  pad.press("PWR");
  EXPECT_EQ(pad.held(), Names{"PWR"});
  pad.release("PWR");
  EXPECT_TRUE(pad.held().empty());
  pad.release("PWR");
  EXPECT_TRUE(pad.held().empty());
}

TEST(Pad, FocusLossReleasesEverything) {
  Pad pad;
  pad.press("PWR");
  pad.press("BOOT");
  EXPECT_EQ(pad.release_all(), (Names{"PWR", "BOOT"}));
  EXPECT_TRUE(pad.held().empty());
  EXPECT_TRUE(pad.release_all().empty());
}

TEST(Pad, KeepOnlyDropsWhatTheBoardLacks) {
  Pad pad;
  pad.press("PWR");
  pad.press("BOOT");
  pad.keep_only({"PWR"});
  EXPECT_EQ(pad.held(), Names{"PWR"});
}

TEST(Pad, ALatchHoldsUntilReleased) {
  Pad pad;
  pad.latch("BOOT");
  pad.latch("BOOT");
  EXPECT_EQ(pad.held(), Names{"BOOT"});
  EXPECT_TRUE(pad.latched("BOOT"));
  EXPECT_FALSE(pad.latched("PWR"));
  pad.release("BOOT");
  EXPECT_TRUE(pad.held().empty());
  EXPECT_FALSE(pad.latched("BOOT"));
}

// Focus loss and the pointer leaving let go of what the mouse holds, and leave a latch down.
TEST(Pad, FocusLossLeavesLatchedButtonsDown) {
  Pad pad;
  pad.press("PWR");
  pad.latch("BOOT");
  pad.press("RST");
  EXPECT_EQ(pad.release_all(), (Names{"PWR", "RST"}));
  EXPECT_EQ(pad.held(), Names{"BOOT"});
  EXPECT_TRUE(pad.latched("BOOT"));
  EXPECT_TRUE(pad.release_all().empty());
}

TEST(Pad, KeepOnlyForgetsALatchTheBoardLacks) {
  Pad pad;
  pad.latch("BOOT");
  pad.keep_only({"PWR"});
  EXPECT_TRUE(pad.held().empty());
  EXPECT_FALSE(pad.latched("BOOT"));
}

}  // namespace
