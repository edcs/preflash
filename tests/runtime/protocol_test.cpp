#include <gtest/gtest.h>

#include "winlink/protocol.h"

using namespace preflash::winlink;

TEST(Protocol, DecodeRejectsALineWithNoOp) {
  EXPECT_FALSE(decode_line("{\"x\":1}"));
  EXPECT_FALSE(decode_line("{\"op\":3}"));
  EXPECT_FALSE(decode_line("not json"));
}

TEST(Protocol, DecodeReadsAnOp) {
  auto msg = decode_line("{\"op\":\"touch\",\"x\":1,\"y\":2,\"down\":true}");
  ASSERT_TRUE(msg);
  EXPECT_EQ((*msg)["op"], "touch");
  EXPECT_EQ((*msg)["x"], 1);
}

TEST(Protocol, EncodeEndsInOneNewline) {
  EXPECT_EQ(encode({{"op", "close"}}), "{\"op\":\"close\"}\n");
}
