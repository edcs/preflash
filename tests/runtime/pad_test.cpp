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

}  // namespace
